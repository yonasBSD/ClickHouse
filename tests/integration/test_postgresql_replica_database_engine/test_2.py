import os.path as p
import random
import threading
import time
import uuid
from random import randrange

import psycopg2
import pytest
from psycopg2.extensions import ISOLATION_LEVEL_AUTOCOMMIT

from helpers.cluster import ClickHouseCluster
from helpers.postgres_utility import (
    PostgresManager,
    assert_nested_table_is_created,
    assert_number_of_columns,
    check_several_tables_are_synchronized,
    check_tables_are_synchronized,
    create_postgres_schema,
    create_postgres_table,
    create_postgres_table_with_schema,
    create_replication_slot,
    drop_postgres_schema,
    drop_postgres_table,
    drop_postgres_table_with_schema,
    drop_replication_slot,
    get_postgres_conn,
    postgres_table_template,
    postgres_table_template_2,
    postgres_table_template_3,
    postgres_table_template_4,
    postgres_table_template_5,
    postgres_table_template_6,
    queries,
)
from helpers.test_tools import TSV, assert_eq_with_retry

cluster = ClickHouseCluster(__file__)
instance = cluster.add_instance(
    "instance",
    main_configs=["configs/log_conf.xml"],
    user_configs=["configs/users.xml"],
    with_postgres=True,
    stay_alive=True,
)

instance2 = cluster.add_instance(
    "instance2",
    main_configs=["configs/log_conf.xml", "configs/merge_tree_too_many_parts.xml"],
    user_configs=["configs/users.xml"],
    with_postgres=True,
    stay_alive=True,
)


pg_manager = PostgresManager()
pg_manager2 = PostgresManager()
pg_manager_instance2 = PostgresManager()


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()
        pg_manager.init(
            instance,
            cluster.postgres_ip,
            cluster.postgres_port,
            default_database="postgres_database",
        )
        pg_manager_instance2.init(
            instance2,
            cluster.postgres_ip,
            cluster.postgres_port,
            default_database="postgres_database",
            postgres_db_exists=True,
        )
        pg_manager2.init(
            instance2, cluster.postgres_ip, cluster.postgres_port, "postgres_database2"
        )

        yield cluster

    finally:
        cluster.shutdown()


@pytest.fixture(autouse=True)
def setup_teardown():
    print("PostgreSQL is available - running test")
    yield  # run test
    pg_manager.restart()


def test_add_new_table_to_replication(started_cluster):
    NUM_TABLES = 5

    pg_manager.create_and_fill_postgres_tables(NUM_TABLES, 10000)
    pg_manager.create_materialized_db(
        ip=started_cluster.postgres_ip, port=started_cluster.postgres_port
    )
    check_several_tables_are_synchronized(instance, NUM_TABLES)

    result = instance.query("SHOW TABLES FROM test_database")
    assert (
        result
        == "postgresql_replica_0\npostgresql_replica_1\npostgresql_replica_2\npostgresql_replica_3\npostgresql_replica_4\n"
    )

    table_name = "postgresql_replica_5"
    pg_manager.create_and_fill_postgres_table(table_name)

    result = instance.query("SHOW CREATE DATABASE test_database")
    assert (
        result[:63]
        == "CREATE DATABASE test_database\\nENGINE = MaterializedPostgreSQL("
    )  # Check without ip
    assert result[-51:] == "\\'postgres_database\\', \\'postgres\\', \\'[HIDDEN]\\')\n"

    result = instance.query_and_get_error(
        "ALTER DATABASE test_database MODIFY SETTING materialized_postgresql_tables_list='tabl1'"
    )
    assert (
        "Changing setting `materialized_postgresql_tables_list` is not allowed"
        in result
    )

    result = instance.query_and_get_error(
        "ALTER DATABASE test_database MODIFY SETTING materialized_postgresql_tables='tabl1'"
    )
    assert "Database engine MaterializedPostgreSQL does not support setting" in result

    instance.query(f"ATTACH TABLE test_database.{table_name}")

    result = instance.query("SHOW TABLES FROM test_database")
    assert (
        result
        == "postgresql_replica_0\npostgresql_replica_1\npostgresql_replica_2\npostgresql_replica_3\npostgresql_replica_4\npostgresql_replica_5\n"
    )

    check_tables_are_synchronized(instance, table_name)
    instance.query(
        f"INSERT INTO postgres_database.{table_name} SELECT number, number from numbers(10000, 10000)"
    )
    check_tables_are_synchronized(instance, table_name)

    result = instance.query_and_get_error(f"ATTACH TABLE test_database.{table_name}")
    assert "Table test_database.postgresql_replica_5 already exists" in result

    result = instance.query_and_get_error("ATTACH TABLE test_database.unknown_table")
    assert "PostgreSQL table unknown_table does not exist" in result

    result = instance.query("SHOW CREATE DATABASE test_database")
    assert (
        result[:63]
        == "CREATE DATABASE test_database\\nENGINE = MaterializedPostgreSQL("
    )
    assert (
        result[-180:]
        == ")\\nSETTINGS materialized_postgresql_tables_list = \\'postgresql_replica_0,postgresql_replica_1,postgresql_replica_2,postgresql_replica_3,postgresql_replica_4,postgresql_replica_5\\'\n"
    )

    table_name = "postgresql_replica_6"
    pg_manager.create_postgres_table(table_name)
    instance.query(
        "INSERT INTO postgres_database.{} SELECT number, number from numbers(10000)".format(
            table_name
        )
    )
    instance.query(f"ATTACH TABLE test_database.{table_name}")

    instance.restart_clickhouse()

    table_name = "postgresql_replica_7"
    pg_manager.create_postgres_table(table_name)
    instance.query(
        "INSERT INTO postgres_database.{} SELECT number, number from numbers(10000)".format(
            table_name
        )
    )
    instance.query(f"ATTACH TABLE test_database.{table_name}")

    result = instance.query("SHOW CREATE DATABASE test_database")
    assert (
        result[:63]
        == "CREATE DATABASE test_database\\nENGINE = MaterializedPostgreSQL("
    )
    assert (
        result[-222:]
        == ")\\nSETTINGS materialized_postgresql_tables_list = \\'postgresql_replica_0,postgresql_replica_1,postgresql_replica_2,postgresql_replica_3,postgresql_replica_4,postgresql_replica_5,postgresql_replica_6,postgresql_replica_7\\'\n"
    )

    instance.query(
        f"INSERT INTO postgres_database.{table_name} SELECT number, number from numbers(10000, 10000)"
    )

    result = instance.query("SHOW TABLES FROM test_database")
    assert (
        result
        == "postgresql_replica_0\npostgresql_replica_1\npostgresql_replica_2\npostgresql_replica_3\npostgresql_replica_4\npostgresql_replica_5\npostgresql_replica_6\npostgresql_replica_7\n"
    )
    check_several_tables_are_synchronized(instance, NUM_TABLES + 3)


def test_remove_table_from_replication(started_cluster):
    NUM_TABLES = 5
    pg_manager.create_and_fill_postgres_tables(NUM_TABLES, 10000)
    pg_manager.create_materialized_db(
        ip=started_cluster.postgres_ip, port=started_cluster.postgres_port
    )
    check_several_tables_are_synchronized(instance, NUM_TABLES)

    result = instance.query("SHOW TABLES FROM test_database")
    assert (
        result
        == "postgresql_replica_0\npostgresql_replica_1\npostgresql_replica_2\npostgresql_replica_3\npostgresql_replica_4\n"
    )

    result = instance.query("SHOW CREATE DATABASE test_database")
    assert (
        result[:63]
        == "CREATE DATABASE test_database\\nENGINE = MaterializedPostgreSQL("
    )
    assert result[-51:] == "\\'postgres_database\\', \\'postgres\\', \\'[HIDDEN]\\')\n"

    table_name = "postgresql_replica_4"
    instance.query(f"DETACH TABLE test_database.{table_name} PERMANENTLY")
    result = instance.query_and_get_error(f"SELECT * FROM test_database.{table_name}")
    assert "UNKNOWN_TABLE" in result

    result = instance.query("SHOW TABLES FROM test_database")
    assert (
        result
        == "postgresql_replica_0\npostgresql_replica_1\npostgresql_replica_2\npostgresql_replica_3\n"
    )

    result = instance.query("SHOW CREATE DATABASE test_database")
    assert (
        result[:63]
        == "CREATE DATABASE test_database\\nENGINE = MaterializedPostgreSQL("
    )
    assert (
        result[-138:]
        == ")\\nSETTINGS materialized_postgresql_tables_list = \\'postgresql_replica_0,postgresql_replica_1,postgresql_replica_2,postgresql_replica_3\\'\n"
    )

    instance.query(f"ATTACH TABLE test_database.{table_name}")
    check_tables_are_synchronized(instance, table_name)
    check_several_tables_are_synchronized(instance, NUM_TABLES)
    instance.query(
        f"INSERT INTO postgres_database.{table_name} SELECT number, number from numbers(10000, 10000)"
    )
    check_tables_are_synchronized(instance, table_name)

    result = instance.query("SHOW CREATE DATABASE test_database")
    assert (
        result[:63]
        == "CREATE DATABASE test_database\\nENGINE = MaterializedPostgreSQL("
    )
    assert (
        result[-159:]
        == ")\\nSETTINGS materialized_postgresql_tables_list = \\'postgresql_replica_0,postgresql_replica_1,postgresql_replica_2,postgresql_replica_3,postgresql_replica_4\\'\n"
    )

    table_name = "postgresql_replica_1"
    instance.query(f"DETACH TABLE test_database.{table_name} PERMANENTLY")
    result = instance.query("SHOW CREATE DATABASE test_database")
    assert (
        result[:63]
        == "CREATE DATABASE test_database\\nENGINE = MaterializedPostgreSQL("
    )
    assert (
        result[-138:]
        == ")\\nSETTINGS materialized_postgresql_tables_list = \\'postgresql_replica_0,postgresql_replica_2,postgresql_replica_3,postgresql_replica_4\\'\n"
    )

    pg_manager.execute(f"drop table if exists postgresql_replica_0;")

    # Removing from replication table which does not exist in PostgreSQL must be ok.
    instance.query("DETACH TABLE test_database.postgresql_replica_0 PERMANENTLY")
    assert instance.contains_in_log(
        "from publication, because table does not exist in PostgreSQL"
    )


def test_predefined_connection_configuration(started_cluster):
    pg_manager.execute(f"DROP TABLE IF EXISTS test_table")
    pg_manager.execute(
        f"CREATE TABLE test_table (key integer PRIMARY KEY, value integer)"
    )
    pg_manager.execute(f"INSERT INTO test_table SELECT 1, 2")
    instance.query(
        "CREATE DATABASE test_database ENGINE = MaterializedPostgreSQL(postgres1) SETTINGS materialized_postgresql_tables_list='test_table'"
    )
    check_tables_are_synchronized(instance, "test_table")
    pg_manager.drop_materialized_db()


insert_counter = 0


def test_database_with_single_non_default_schema(started_cluster):
    cursor = pg_manager.get_db_cursor()
    NUM_TABLES = 5
    schema_name = "test_schema"
    materialized_db = "test_database"
    clickhouse_postgres_db = "postgres_database_with_schema"
    global insert_counter
    insert_counter = 0

    def insert_into_tables():
        global insert_counter
        clickhouse_postgres_db = "postgres_database_with_schema"
        for i in range(NUM_TABLES):
            table_name = f"postgresql_replica_{i}"
            instance.query(
                f"INSERT INTO {clickhouse_postgres_db}.{table_name} SELECT number, number from numbers(1000 * {insert_counter}, 1000)"
            )
        insert_counter += 1

    def assert_show_tables(expected):
        result = instance.query("SHOW TABLES FROM test_database")
        assert result == expected
        print("assert show tables Ok")

    def check_all_tables_are_synchronized():
        for i in range(NUM_TABLES):
            print("checking table", i)
            check_tables_are_synchronized(
                instance,
                f"postgresql_replica_{i}",
                postgres_database=clickhouse_postgres_db,
            )
        print("synchronization Ok")

    create_postgres_schema(cursor, schema_name)
    pg_manager.create_clickhouse_postgres_db(
        database_name=clickhouse_postgres_db,
        schema_name=schema_name,
        postgres_database="postgres_database",
    )

    for i in range(NUM_TABLES):
        create_postgres_table_with_schema(
            cursor, schema_name, f"postgresql_replica_{i}"
        )

    insert_into_tables()
    pg_manager.create_materialized_db(
        ip=started_cluster.postgres_ip,
        port=started_cluster.postgres_port,
        settings=[
            f"materialized_postgresql_schema = '{schema_name}'",
        ],
    )

    insert_into_tables()
    check_all_tables_are_synchronized()
    assert_show_tables(
        "postgresql_replica_0\npostgresql_replica_1\npostgresql_replica_2\npostgresql_replica_3\npostgresql_replica_4\n"
    )

    instance.restart_clickhouse()
    check_all_tables_are_synchronized()
    assert_show_tables(
        "postgresql_replica_0\npostgresql_replica_1\npostgresql_replica_2\npostgresql_replica_3\npostgresql_replica_4\n"
    )
    insert_into_tables()
    check_all_tables_are_synchronized()

    altered_table = random.randint(0, NUM_TABLES - 1)
    pg_manager.execute(
        "ALTER TABLE test_schema.postgresql_replica_{} ADD COLUMN value2 integer".format(
            altered_table
        )
    )

    instance.query(
        f"INSERT INTO {clickhouse_postgres_db}.postgresql_replica_{altered_table} SELECT number, number, number from numbers(5000, 1000)"
    )

    assert instance.wait_for_log_line(
        f"Table postgresql_replica_{altered_table} is skipped from replication stream"
    )
    instance.query(
        f"DETACH TABLE test_database.postgresql_replica_{altered_table} PERMANENTLY"
    )
    assert not instance.contains_in_log(
        "from publication, because table does not exist in PostgreSQL"
    )
    instance.query(f"ATTACH TABLE test_database.postgresql_replica_{altered_table}")

    check_tables_are_synchronized(
        instance,
        f"postgresql_replica_{altered_table}",
        postgres_database=clickhouse_postgres_db,
    )


def test_database_with_multiple_non_default_schemas_1(started_cluster):
    cursor = pg_manager.get_db_cursor()

    NUM_TABLES = 5
    schema_name = "test_schema"
    clickhouse_postgres_db = "postgres_database_with_schema"
    materialized_db = "test_database"
    publication_tables = ""
    global insert_counter
    insert_counter = 0

    def insert_into_tables():
        global insert_counter
        clickhouse_postgres_db = "postgres_database_with_schema"
        for i in range(NUM_TABLES):
            table_name = f"postgresql_replica_{i}"
            instance.query(
                f"INSERT INTO {clickhouse_postgres_db}.{table_name} SELECT number, number from numbers(1000 * {insert_counter}, 1000)"
            )
        insert_counter += 1

    def assert_show_tables(expected):
        result = instance.query("SHOW TABLES FROM test_database")
        assert result == expected
        print("assert show tables Ok")

    def check_all_tables_are_synchronized():
        for i in range(NUM_TABLES):
            print("checking table", i)
            check_tables_are_synchronized(
                instance,
                "postgresql_replica_{}".format(i),
                schema_name=schema_name,
                postgres_database=clickhouse_postgres_db,
            )
        print("synchronization Ok")

    create_postgres_schema(cursor, schema_name)
    pg_manager.create_clickhouse_postgres_db(
        database_name=clickhouse_postgres_db,
        schema_name=schema_name,
        postgres_database="postgres_database",
    )

    for i in range(NUM_TABLES):
        table_name = "postgresql_replica_{}".format(i)
        create_postgres_table_with_schema(cursor, schema_name, table_name)
        if publication_tables != "":
            publication_tables += ", "
        publication_tables += schema_name + "." + table_name

    insert_into_tables()
    pg_manager.create_materialized_db(
        ip=started_cluster.postgres_ip,
        port=started_cluster.postgres_port,
        settings=[
            f"materialized_postgresql_tables_list = '{publication_tables}'",
            "materialized_postgresql_tables_list_with_schema=1",
        ],
    )

    check_all_tables_are_synchronized()
    assert_show_tables(
        "test_schema.postgresql_replica_0\ntest_schema.postgresql_replica_1\ntest_schema.postgresql_replica_2\ntest_schema.postgresql_replica_3\ntest_schema.postgresql_replica_4\n"
    )

    instance.restart_clickhouse()
    check_all_tables_are_synchronized()
    assert_show_tables(
        "test_schema.postgresql_replica_0\ntest_schema.postgresql_replica_1\ntest_schema.postgresql_replica_2\ntest_schema.postgresql_replica_3\ntest_schema.postgresql_replica_4\n"
    )

    insert_into_tables()
    check_all_tables_are_synchronized()

    altered_table = random.randint(0, NUM_TABLES - 1)
    pg_manager.execute(
        "ALTER TABLE test_schema.postgresql_replica_{} ADD COLUMN value2 integer".format(
            altered_table
        )
    )

    instance.query(
        f"INSERT INTO {clickhouse_postgres_db}.postgresql_replica_{altered_table} SELECT number, number, number from numbers(5000, 1000)"
    )

    assert instance.wait_for_log_line(
        f"Table test_schema.postgresql_replica_{altered_table} is skipped from replication stream"
    )
    altered_materialized_table = (
        f"{materialized_db}.`test_schema.postgresql_replica_{altered_table}`"
    )
    instance.query(f"DETACH TABLE {altered_materialized_table} PERMANENTLY")
    assert not instance.contains_in_log(
        "from publication, because table does not exist in PostgreSQL"
    )
    instance.query(f"ATTACH TABLE {altered_materialized_table}")

    assert_show_tables(
        "test_schema.postgresql_replica_0\ntest_schema.postgresql_replica_1\ntest_schema.postgresql_replica_2\ntest_schema.postgresql_replica_3\ntest_schema.postgresql_replica_4\n"
    )
    check_tables_are_synchronized(
        instance,
        f"postgresql_replica_{altered_table}",
        schema_name=schema_name,
        postgres_database=clickhouse_postgres_db,
    )


def test_database_with_multiple_non_default_schemas_2(started_cluster):
    cursor = pg_manager.get_db_cursor()
    NUM_TABLES = 2
    schemas_num = 2
    schema_list = "schema0, schema1"
    materialized_db = "test_database"
    global insert_counter
    insert_counter = 0

    def check_all_tables_are_synchronized():
        for i in range(schemas_num):
            schema_name = f"schema{i}"
            clickhouse_postgres_db = f"clickhouse_postgres_db{i}"
            for ti in range(NUM_TABLES):
                table_name = f"postgresql_replica_{ti}"
                print(f"checking table {schema_name}.{table_name}")
                check_tables_are_synchronized(
                    instance,
                    f"{table_name}",
                    schema_name=schema_name,
                    postgres_database=clickhouse_postgres_db,
                )
        print("synchronized Ok")

    def insert_into_tables():
        global insert_counter
        for i in range(schemas_num):
            clickhouse_postgres_db = f"clickhouse_postgres_db{i}"
            for ti in range(NUM_TABLES):
                table_name = f"postgresql_replica_{ti}"
                instance.query(
                    f"INSERT INTO {clickhouse_postgres_db}.{table_name} SELECT number, number from numbers(1000 * {insert_counter}, 1000)"
                )
        insert_counter += 1

    def assert_show_tables(expected):
        result = instance.query("SHOW TABLES FROM test_database")
        assert result == expected
        print("assert show tables Ok")

    for i in range(schemas_num):
        schema_name = f"schema{i}"
        clickhouse_postgres_db = f"clickhouse_postgres_db{i}"
        create_postgres_schema(cursor, schema_name)
        pg_manager.create_clickhouse_postgres_db(
            database_name=clickhouse_postgres_db,
            schema_name=schema_name,
            postgres_database="postgres_database",
        )
        for ti in range(NUM_TABLES):
            table_name = f"postgresql_replica_{ti}"
            create_postgres_table_with_schema(cursor, schema_name, table_name)

    insert_into_tables()
    pg_manager.create_materialized_db(
        ip=started_cluster.postgres_ip,
        port=started_cluster.postgres_port,
        settings=[
            f"materialized_postgresql_schema_list = '{schema_list}'",
        ],
    )

    check_all_tables_are_synchronized()
    insert_into_tables()
    assert_show_tables(
        "schema0.postgresql_replica_0\nschema0.postgresql_replica_1\nschema1.postgresql_replica_0\nschema1.postgresql_replica_1\n"
    )

    instance.restart_clickhouse()
    assert_show_tables(
        "schema0.postgresql_replica_0\nschema0.postgresql_replica_1\nschema1.postgresql_replica_0\nschema1.postgresql_replica_1\n"
    )
    check_all_tables_are_synchronized()
    insert_into_tables()
    check_all_tables_are_synchronized()

    print("ALTER")
    altered_schema = random.randint(0, schemas_num - 1)
    altered_table = random.randint(0, NUM_TABLES - 1)
    clickhouse_postgres_db = f"clickhouse_postgres_db{altered_schema}"
    pg_manager.execute(
        f"ALTER TABLE schema{altered_schema}.postgresql_replica_{altered_table} ADD COLUMN value2 integer"
    )

    instance.query(
        f"INSERT INTO clickhouse_postgres_db{altered_schema}.postgresql_replica_{altered_table} SELECT number, number, number from numbers(1000 * {insert_counter}, 1000)"
    )

    assert instance.wait_for_log_line(
        f"Table schema{altered_schema}.postgresql_replica_{altered_table} is skipped from replication stream"
    )

    altered_materialized_table = (
        f"{materialized_db}.`schema{altered_schema}.postgresql_replica_{altered_table}`"
    )
    instance.query(f"DETACH TABLE {altered_materialized_table} PERMANENTLY")
    assert not instance.contains_in_log(
        "from publication, because table does not exist in PostgreSQL"
    )
    instance.query(f"ATTACH TABLE {altered_materialized_table}")

    assert_show_tables(
        "schema0.postgresql_replica_0\nschema0.postgresql_replica_1\nschema1.postgresql_replica_0\nschema1.postgresql_replica_1\n"
    )
    check_tables_are_synchronized(
        instance,
        f"postgresql_replica_{altered_table}",
        schema_name=f"schema{altered_schema}",
        postgres_database=clickhouse_postgres_db,
    )


if __name__ == "__main__":
    cluster.start()
    input("Cluster created, press any key to destroy...")
    cluster.shutdown()
