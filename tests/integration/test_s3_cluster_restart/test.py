import csv
import logging
import os
import shutil
from uuid import uuid4

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.config_cluster import minio_secret_key
from helpers.mock_servers import start_mock_servers
from helpers.test_tools import TSV

logging.getLogger().setLevel(logging.INFO)
logging.getLogger().addHandler(logging.StreamHandler())

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
S3_DATA = []

generated_rows = 0

def create_buckets_s3(cluster):
    global generated_rows

    minio = cluster.minio_client

    for file_number in range(100):
        file_name = f"data/generated/file_{file_number}.csv"
        os.makedirs(os.path.join(SCRIPT_DIR, "data/generated/"), exist_ok=True)
        S3_DATA.append(file_name)
        with open(os.path.join(SCRIPT_DIR, file_name), "w+", encoding="utf-8") as f:
            # a String, b UInt64
            data = []

            # Make all files a bit different
            for number in range(100 + file_number):
                data.append(
                    ["str_" + str(number + file_number) * 10, number + file_number]
                )
                generated_rows += 1

            writer = csv.writer(f)
            writer.writerows(data)

    for file in S3_DATA:
        minio.fput_object(
            bucket_name=cluster.minio_bucket,
            object_name=file,
            file_path=os.path.join(SCRIPT_DIR, file),
        )
    for obj in minio.list_objects(cluster.minio_bucket, recursive=True):
        print(obj.object_name)


def run_s3_mocks(started_cluster):
    script_dir = os.path.join(os.path.dirname(__file__), "s3_mocks")
    start_mock_servers(
        started_cluster,
        script_dir,
        [
            ("s3_mock.py", "resolver", "8080"),
        ],
    )


cluster = ClickHouseCluster(__file__)
node1 = cluster.add_instance(
    "node1",
    main_configs=["configs/cluster.xml"],
    user_configs=["configs/users.xml"],
    with_minio=True,
    with_zookeeper=True,
    macros={"replica": "node1"},
)
node2 = cluster.add_instance(
    "node2",
    main_configs=["configs/cluster.xml"],
    user_configs=["configs/users.xml"],
    stay_alive=True,
    with_zookeeper=True,
    macros={"replica": "node2"},
)
node3 = cluster.add_instance(
    "node3",
    main_configs=["configs/cluster.xml"],
    user_configs=["configs/users.xml"],
    stay_alive=True,
    with_zookeeper=True,
    macros={"replica": "node3"},
)

@pytest.fixture(scope="module")
def started_cluster():
    try:

        logging.info("Starting cluster...")
        cluster.start()
        logging.info("Cluster started")

        create_buckets_s3(cluster)

        run_s3_mocks(cluster)

        yield cluster
    finally:
        shutil.rmtree(os.path.join(SCRIPT_DIR, "data/generated/"))
        cluster.shutdown()


def test_reconnect_after_nodes_restart(started_cluster):
    uuid = str(uuid4())
    result = node1.query(
        f"""
        SELECT count(*) from s3Cluster(
            'cluster_simple',
            'http://minio1:9001/root/data/generated/*.csv',
            'minio', '{minio_secret_key}', 'CSV', 'a String, b UInt64')
        """
        , query_id=uuid
    )

    assert result == "14950\n"

    node1.query("system flush logs query_log")

    assert (
        node1.query(
            f"""SELECT ProfileEvents['DistributedConnectionReconnectCount'] FROM system.query_log WHERE query_id = '{uuid}' and type = 'QueryFinish';"""
        ) == "0\n"
    )

    node2.restart_clickhouse()
    node3.restart_clickhouse()

    uuid = str(uuid4())
    result = node1.query(
        f"""
        SELECT count(*) from s3Cluster(
            'cluster_simple',
            'http://minio1:9001/root/data/generated/*.csv',
            'minio', '{minio_secret_key}', 'CSV', 'a String, b UInt64')
        """
        ,query_id=uuid
    )

    assert result == "14950\n"

    node1.query("system flush logs query_log")

    assert (
        node1.query(
            f"""SELECT ProfileEvents['DistributedConnectionReconnectCount'] FROM system.query_log WHERE query_id = '{uuid}' and type = 'QueryFinish';"""
        ) == "2\n"
    )


def test_reconnect_after_nodes_restart_no_wait(started_cluster):
    uuid = str(uuid4())
    result = node1.query(
        f"""
        SELECT count(*) from s3Cluster(
            'cluster_simple',
            'http://minio1:9001/root/data/generated/*.csv',
            'minio', '{minio_secret_key}', 'CSV', 'a String, b UInt64')
        """
        , query_id=uuid
    )

    assert result == "14950\n"

    node1.query("system flush logs query_log")

    assert (
        node1.query(
            f"""SELECT ProfileEvents['DistributedConnectionReconnectCount'] FROM system.query_log WHERE query_id = '{uuid}' and type = 'QueryFinish';"""
        ) == "0\n"
    )

    node2.stop()
    node2.start()

    uuid = str(uuid4())
    result = node1.query(
        f"""
        SELECT count(*) from s3Cluster(
            'cluster_simple',
            'http://minio1:9001/root/data/generated/*.csv',
            'minio', '{minio_secret_key}', 'CSV', 'a String, b UInt64')
        """
        ,query_id=uuid
    )

    assert result == "14950\n"

    node1.query("system flush logs query_log")

    assert (
        node1.query(
            f"""SELECT ProfileEvents['DistributedConnectionReconnectCount'], ProfileEvents['DistributedConnectionFailTry'] > 0 FROM system.query_log WHERE query_id = '{uuid}' and type = 'QueryFinish';"""
        ) == "1\t1\n"
    )

    # avoid leaving the test w/o started node, so next test will start with fully runnning cluster
    node2.wait_for_start(30)


def createTable(table, missing_table):
    node1.query(
        f"""
    CREATE TABLE {table} (a String, b UInt64)
    ENGINE=ReplicatedMergeTree('/clickhouse/tables/f15b1936-ae89-416b-8626-7c88d9fbe6a3/{table}', '{{replica}}')
    ORDER BY (a, b);
        """
    )
    node2.query(
        f"""
    CREATE TABLE {table} (a String, b UInt64)
    ENGINE=ReplicatedMergeTree('/clickhouse/tables/f15b1936-ae89-416b-8626-7c88d9fbe6a3/{table}', '{{replica}}')
    ORDER BY (a, b);
        """
    )
    if (not missing_table):
        node3.query(
            f"""
        CREATE TABLE {table} (a String, b UInt64)
        ENGINE=ReplicatedMergeTree('/clickhouse/tables/f15b1936-ae89-416b-8626-7c88d9fbe6a3/{table}', '{{replica}}')
        ORDER BY (a, b);
            """
        )


@pytest.mark.parametrize(
    "wait_restart, missing_table",
    [
        pytest.param(False, False),
        pytest.param(False, True),
        pytest.param(True, False),
        pytest.param(True, True),
    ],
)
def test_insert_select(started_cluster, wait_restart, missing_table):
    table = 't_rmt_target'

    node1.query(
        f"""DROP TABLE IF EXISTS {table} ON CLUSTER 'cluster_simple' SYNC;"""
    )

    createTable(table, missing_table);

    if (wait_restart):
        node2.restart_clickhouse()
    else:
        node2.stop()
        node2.start()

    uuid = str(uuid4())
    node1.query(
        f"""
        INSERT INTO {table} SELECT * FROM s3Cluster(
            'cluster_simple',
            'http://minio1:9001/root/data/generated/*.csv', 'minio', '{minio_secret_key}', 'CSV','a String, b UInt64'
        ) SETTINGS parallel_distributed_insert_select=1;
        """
        , query_id = uuid
    )

    if (not wait_restart):
        node2.wait_for_start(30)

    node1.query(f"SYSTEM SYNC REPLICA {table}")

    # Check whether we inserted at least something
    assert (
        int(
            node1.query(
                f"""SELECT count(*) FROM {table};"""
            ).strip()
        ) == generated_rows
    )

    if (not wait_restart):
        node1.query("SYSTEM FLUSH LOGS query_log");
        assert ( node1.query(f"select ProfileEvents['DistributedConnectionFailTry'] from system.query_log where query_id = '{uuid}' and type = 'QueryFinish'") == "1\n")


    if (missing_table):
        node1.query("SYSTEM FLUSH LOGS query_log");
        assert ( node1.query(f"select ProfileEvents['DistributedConnectionMissingTable'] from system.query_log where query_id = '{uuid}' and type = 'QueryFinish'") == "1\n")

    node1.query(
        f"""DROP TABLE IF EXISTS {table} ON CLUSTER 'cluster_simple' SYNC;"""
    )
