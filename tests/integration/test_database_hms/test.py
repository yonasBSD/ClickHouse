#!/usr/bin/env python3
import glob
import json
import logging
import os
import random
import time
import uuid
from datetime import datetime, timedelta

import pyarrow as pa
import pytest
import requests
import urllib3
from minio import Minio
from pyiceberg.catalog import load_catalog
from pyiceberg.partitioning import PartitionField, PartitionSpec
from pyiceberg.schema import Schema
from pyiceberg.table.sorting import SortField, SortOrder
from pyiceberg.transforms import DayTransform, IdentityTransform
from pyiceberg.types import (
    DoubleType,
    FloatType,
    NestedField,
    StringType,
    StructType,
    TimestampType,
)
from pyiceberg.catalog.hive import HiveCatalog

from helpers.config_cluster import minio_access_key, minio_secret_key
from helpers.cluster import ClickHouseCluster, ClickHouseInstance, is_arm
from helpers.s3_tools import get_file_contents, list_s3_objects, prepare_s3_bucket
from helpers.test_tools import TSV, csv_compare

import boto3

from thrift.transport import TTransport
from thrift.transport import TSocket
from thrift.protocol import TBinaryProtocol
from hive_metastore import ThriftHiveMetastore

CATALOG_NAME = "default"

BASE_URL = "thrift://hive:9083"

DEFAULT_SCHEMA = Schema(
    NestedField(
        field_id=1, name="datetime", field_type=TimestampType(), required=False
    ),
    NestedField(field_id=2, name="symbol", field_type=StringType(), required=False),
    NestedField(field_id=3, name="bid", field_type=DoubleType(), required=False),
    NestedField(field_id=4, name="ask", field_type=DoubleType(), required=False),
    NestedField(
        field_id=5,
        name="details",
        field_type=StructType(
            NestedField(
                field_id=4,
                name="created_by",
                field_type=StringType(),
                required=False,
            ),
        ),
        required=False,
    ),
)

DEFAULT_CREATE_TABLE = "CREATE TABLE {}.`{}.{}`\\n(\\n    `datetime` Nullable(DateTime64(6)),\\n    `symbol` Nullable(String),\\n    `bid` Nullable(Float64),\\n    `ask` Nullable(Float64),\\n    `details` Tuple(created_by Nullable(String))\\n)\\nENGINE = Iceberg(\\'http://minio:9000/warehouse-hms/data/\\', \\'minio\\', \\'[HIDDEN]\\')\n"

DEFAULT_PARTITION_SPEC = PartitionSpec(
    PartitionField(
        source_id=1, field_id=1000, transform=DayTransform(), name="datetime_day"
    )
)

DEFAULT_SORT_ORDER = SortOrder(SortField(source_id=2, transform=IdentityTransform()))

def load_catalog_impl(started_cluster):
    return load_catalog(
        CATALOG_NAME,
        **{
            "uri": "thrift://0.0.0.0:9083",
            "type": "hive",
            "s3.endpoint": f"http://{started_cluster.get_instance_ip('minio')}:9000",
            "s3.access-key-id": minio_access_key,
            "s3.secret-access-key": minio_secret_key,
        },
    )

def create_table(
    catalog,
    namespace,
    table,
    schema=DEFAULT_SCHEMA,
    partition_spec=DEFAULT_PARTITION_SPEC,
    sort_order=DEFAULT_SORT_ORDER,
):
    return catalog.create_table(
        identifier=f"{namespace}.{table}",
        schema=schema,
        location=f"s3a://warehouse-hms/data",
        partition_spec=partition_spec,
        sort_order=sort_order,
    )


def generate_record():
    return {
        "datetime": datetime.now(),
        "symbol": str("kek"),
        "bid": round(random.uniform(100, 200), 2),
        "ask": round(random.uniform(200, 300), 2),
        "details": {"created_by": "Alice Smith"},
    }


def create_clickhouse_iceberg_database(
    started_cluster, node, name, additional_settings={}
):
    settings = {
        "catalog_type": "hive",
        "warehouse": "demo",
        "storage_endpoint": "http://minio:9000/warehouse-hms",
    }

    settings.update(additional_settings)
    node.query(
            f"""
    DROP DATABASE IF EXISTS {name};
    SET allow_experimental_database_hms_catalog=true;
    CREATE DATABASE {name} ENGINE = DataLakeCatalog('{BASE_URL}', '{minio_access_key}', '{minio_secret_key}')
    SETTINGS {",".join((k+"="+repr(v) for k, v in settings.items()))}
        """
        )


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster = ClickHouseCluster(__file__)
        cluster.add_instance(
            "node1",
            main_configs=[],
            user_configs=[],
            stay_alive=True,
            with_hms_catalog=True,
        )

        logging.info("Starting cluster...")
        cluster.start()

        # TODO: properly wait for container
        time.sleep(10)

        yield cluster

    finally:
        cluster.shutdown()


def test_list_tables(started_cluster):
    node = started_cluster.instances["node1"]

    root_namespace = f"clickhouse_{uuid.uuid4()}"
    namespace_1 = f"{root_namespace}"
    namespace_1_tables = ["table1", "table2"]

    catalog = load_catalog_impl(started_cluster)
    for namespace in [namespace_1]:
        catalog.create_namespace(namespace)

    found = False
    for namespace_list in catalog.list_namespaces():
        if root_namespace == namespace_list[0]:
            found = True
            break
    assert found

    found = False
    for namespace_list in catalog.list_namespaces():
        if root_namespace == namespace_list[0]:
            found = True
            break
    assert found

    for namespace in [namespace_1]:
        assert len(catalog.list_tables(namespace)) == 0

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    tables_list = ""
    for table in namespace_1_tables:
        create_table(catalog, namespace_1, table)
        if len(tables_list) > 0:
            tables_list += "\n"
        tables_list += f"{namespace_1}.{table}"

    assert (
        tables_list
        == node.query(
            f"SELECT name FROM system.tables WHERE database = '{CATALOG_NAME}' and name ILIKE '{root_namespace}%' ORDER BY name"
        ).strip()
    )
    node.restart_clickhouse()
    assert (
        tables_list
        == node.query(
            f"SELECT name FROM system.tables WHERE database = '{CATALOG_NAME}' and name ILIKE '{root_namespace}%' ORDER BY name"
        ).strip()
    )


### NOTE: pyiceberg with hive catalog does not support inheritable namespaces
def test_many_namespaces(started_cluster):
    node = started_cluster.instances["node1"]
    root_namespace_1 = f"a_{uuid.uuid4()}"
    root_namespace_2 = f"b_{uuid.uuid4()}"
    namespaces = [
        f"{root_namespace_1}",
        f"{root_namespace_2}",
    ]
    tables = ["table_a", "table_b", "table_c"]
    catalog = load_catalog_impl(started_cluster)

    for namespace in namespaces:
        catalog.create_namespace(namespace)
        for table in tables:
            create_table(catalog, namespace, table)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    for namespace in namespaces:
        for table in tables:
            table_name = f"{namespace}.{table}"
            assert int(
                node.query(
                    f"SELECT count() FROM system.tables WHERE database = '{CATALOG_NAME}' and name = '{table_name}'"
                )
            )


def test_select(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_list_tables_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    namespace = f"{root_namespace}"
    namespaces_to_create = [
        root_namespace
    ]

    catalog = load_catalog_impl(started_cluster)

    for namespace in namespaces_to_create:
        catalog.create_namespace(namespace)
        assert len(catalog.list_tables(namespace)) == 0

    table = create_table(catalog, namespace, table_name)

    num_rows = 10
    data = [generate_record() for _ in range(num_rows)]
    df = pa.Table.from_pylist(data)
    table.append(df)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    expected = DEFAULT_CREATE_TABLE.format(CATALOG_NAME, namespace, table_name)
    assert expected == node.query(
        f"SHOW CREATE TABLE {CATALOG_NAME}.`{namespace}.{table_name}`"
    )

    assert num_rows == int(
        node.query(f"SELECT count() FROM {CATALOG_NAME}.`{namespace}.{table_name}`")
    )

def test_hide_sensitive_info(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_hide_sensitive_info_{uuid.uuid4()}"
    table_name = f"{test_ref}_table"
    root_namespace = f"{test_ref}_namespace"

    namespace = f"{root_namespace}"
    catalog = load_catalog_impl(started_cluster)
    catalog.create_namespace(namespace)

    table = create_table(catalog, namespace, table_name)

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"catalog_credential": "SECRET_1"},
    )
    assert "SECRET_1" not in node.query(f"SHOW CREATE DATABASE {CATALOG_NAME}")

    create_clickhouse_iceberg_database(
        started_cluster,
        node,
        CATALOG_NAME,
        additional_settings={"auth_header": "SECRET_2"},
    )
    assert "SECRET_2" not in node.query(f"SHOW CREATE DATABASE {CATALOG_NAME}")


def test_tables_with_same_location(started_cluster):
    node = started_cluster.instances["node1"]

    test_ref = f"test_tables_with_same_location_{uuid.uuid4()}"
    namespace = f"{test_ref}_namespace"
    catalog = load_catalog_impl(started_cluster)

    table_name = f"{test_ref}_table"
    table_name_2 = f"{test_ref}_table_2"

    catalog.create_namespace(namespace)
    table = create_table(catalog, namespace, table_name)
    table_2 = create_table(catalog, namespace, table_name_2)

    def record(key):
        return {
            "datetime": datetime.now(),
            "symbol": str(key),
            "bid": round(random.uniform(100, 200), 2),
            "ask": round(random.uniform(200, 300), 2),
            "details": {"created_by": "Alice Smith"},
        }

    data = [record('aaa') for _ in range(3)]
    df = pa.Table.from_pylist(data)
    table.append(df)

    data = [record('bbb') for _ in range(3)]
    df = pa.Table.from_pylist(data)
    table_2.append(df)

    create_clickhouse_iceberg_database(started_cluster, node, CATALOG_NAME)

    assert 'aaa\naaa\naaa' == node.query(f"SELECT symbol FROM {CATALOG_NAME}.`{namespace}.{table_name}`").strip()
    assert 'bbb\nbbb\nbbb' == node.query(f"SELECT symbol FROM {CATALOG_NAME}.`{namespace}.{table_name_2}`").strip()
