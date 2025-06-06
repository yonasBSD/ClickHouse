---
description: 'Allows accessing all shards (configured in the `remote_servers` section)
  of a cluster without creating a Distributed table.'
sidebar_label: 'cluster'
sidebar_position: 30
slug: /sql-reference/table-functions/cluster
title: 'clusterAllReplicas'
---

# clusterAllReplicas Table Function

Allows accessing all shards (configured in the `remote_servers` section) of a cluster without creating a [Distributed](../../engines/table-engines/special/distributed.md) table. Only one replica of each shard is queried.

`clusterAllReplicas` function — same as `cluster`, but all replicas are queried. Each replica in a cluster is used as a separate shard/connection.

:::note
All available clusters are listed in the [system.clusters](../../operations/system-tables/clusters.md) table.
:::

## Syntax {#syntax}

```sql
cluster(['cluster_name', db.table, sharding_key])
cluster(['cluster_name', db, table, sharding_key])
clusterAllReplicas(['cluster_name', db.table, sharding_key])
clusterAllReplicas(['cluster_name', db, table, sharding_key])
```
## Arguments {#arguments}

| Arguments                   | Type                                                                                                                                              |
|-----------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------|
| `cluster_name`              | Name of a cluster that is used to build a set of addresses and connection parameters to remote and local servers, set `default` if not specified. |
| `db.table` or `db`, `table` | Name of a database and a table.                                                                                                                   |
| `sharding_key`              | A sharding key. Optional. Needs to be specified if the cluster has more than one shard.                                                           |

## Returned value {#returned_value}

The dataset from clusters.

## Using macros {#using_macros}

`cluster_name` can contain macros — substitution in curly brackets. The substituted value is taken from the [macros](../../operations/server-configuration-parameters/settings.md#macros) section of the server configuration file.

Example:

```sql
SELECT * FROM cluster('{cluster}', default.example_table);
```

## Usage and recommendations {#usage_recommendations}

Using the `cluster` and `clusterAllReplicas` table functions are less efficient than creating a `Distributed` table because in this case, the server connection is re-established for every request. When processing a large number of queries, please always create the `Distributed` table ahead of time, and do not use the `cluster` and `clusterAllReplicas` table functions.

The `cluster` and `clusterAllReplicas` table functions can be useful in the following cases:

- Accessing a specific cluster for data comparison, debugging, and testing.
- Queries to various ClickHouse clusters and replicas for research purposes.
- Infrequent distributed requests that are made manually.

Connection settings like `host`, `port`, `user`, `password`, `compression`, `secure` are taken from `<remote_servers>` config section. See details in [Distributed engine](../../engines/table-engines/special/distributed.md).

## Related {#related}

- [skip_unavailable_shards](../../operations/settings/settings.md#skip_unavailable_shards)
- [load_balancing](../../operations/settings/settings.md#load_balancing)
