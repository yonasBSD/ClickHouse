---
description: 'System table which exists only if ClickHouse Keeper or ZooKeeper are
  configured. It exposes data from the Keeper cluster defined in the config.'
keywords: ['system table', 'zookeeper']
slug: /operations/system-tables/zookeeper
title: 'system.zookeeper'
---

# system.zookeeper

The table does not exist unless ClickHouse Keeper or ZooKeeper is configured. The `system.zookeeper` table exposes data from the Keeper clusters defined in the config.
The query must either have a `path =`   condition or a `path IN`  condition set with the `WHERE` clause as shown below. This corresponds to the path of the children that you want to get data for.

The query `SELECT * FROM system.zookeeper WHERE path = '/clickhouse'` outputs data for all children on the `/clickhouse` node.
To output data for all root nodes, write path = '/'.
If the path specified in 'path' does not exist, an exception will be thrown.

The query `SELECT * FROM system.zookeeper WHERE path IN ('/', '/clickhouse')` outputs data for all children on the `/` and `/clickhouse` node.
If in the specified 'path' collection has does not exist path, an exception will be thrown.
It can be used to do a batch of Keeper path queries.

The query `SELECT * FROM system.zookeeper WHERE path = '/clickhouse' AND zookeeperName = 'auxiliary_cluster'` outputs data in `auxiliary_cluster` ZooKeeper cluster.
If the specified 'auxiliary_cluster' does not exists, an exception will be thrown.

Columns:

- `name` (String) — The name of the node.
- `path` (String) — The path to the node.
- `value` (String) — Node value.
- `zookeeperName` (String) — The name of default or one of auxiliary ZooKeeper cluster.
- `dataLength` (Int32) — Size of the value.
- `numChildren` (Int32) — Number of descendants.
- `czxid` (Int64) — ID of the transaction that created the node.
- `mzxid` (Int64) — ID of the transaction that last changed the node.
- `pzxid` (Int64) — ID of the transaction that last deleted or added descendants.
- `ctime` (DateTime) — Time of node creation.
- `mtime` (DateTime) — Time of the last modification of the node.
- `version` (Int32) — Node version: the number of times the node was changed.
- `cversion` (Int32) — Number of added or removed descendants.
- `aversion` (Int32) — Number of changes to the ACL.
- `ephemeralOwner` (Int64) — For ephemeral nodes, the ID of the session that owns this node.

Example:

```sql
SELECT *
FROM system.zookeeper
WHERE path = '/clickhouse/tables/01-08/visits/replicas'
FORMAT Vertical
```

```text
Row 1:
──────
name:           example01-08-1
value:
czxid:          932998691229
mzxid:          932998691229
ctime:          2015-03-27 16:49:51
mtime:          2015-03-27 16:49:51
version:        0
cversion:       47
aversion:       0
ephemeralOwner: 0
dataLength:     0
numChildren:    7
pzxid:          987021031383
path:           /clickhouse/tables/01-08/visits/replicas

Row 2:
──────
name:           example01-08-2
value:
czxid:          933002738135
mzxid:          933002738135
ctime:          2015-03-27 16:57:01
mtime:          2015-03-27 16:57:01
version:        0
cversion:       37
aversion:       0
ephemeralOwner: 0
dataLength:     0
numChildren:    7
pzxid:          987021252247
path:           /clickhouse/tables/01-08/visits/replicas
```
