-- { echoOn }
DROP TABLE IF EXISTS visits_order;
DROP TABLE IF EXISTS visits_order_dst;

CREATE TABLE visits_order
(
    user_id UInt64,
    user_name String,
    some_int UInt64
) ENGINE = MergeTree() PRIMARY KEY user_id PARTITION BY user_id SETTINGS index_granularity = 1;

CREATE TABLE visits_order_dst
(
    user_id UInt64,
    user_name String,
    some_int UInt64
) ENGINE = MergeTree() PRIMARY KEY user_id PARTITION BY user_id SETTINGS index_granularity = 1;

ALTER TABLE visits_order ADD PROJECTION user_name_projection (SELECT * ORDER BY user_name);
ALTER TABLE visits_order_dst ADD PROJECTION user_name_projection (SELECT * ORDER BY user_name);

INSERT INTO visits_order SELECT 2, 'user2', number from numbers(1, 10);
INSERT INTO visits_order SELECT 2, 'another_user2', number*2 from numbers(1, 10);
INSERT INTO visits_order SELECT 2, 'yet_another_user2', number*3 from numbers(1, 10);

-- Merge all parts so that projections can no longer help filter them,
-- which will result in projections not being used.
OPTIMIZE TABLE visits_order FINAL;

ALTER TABLE visits_order_dst ATTACH PARTITION ID '2' FROM visits_order;

SET enable_analyzer=0;

EXPLAIN SELECT * FROM visits_order_dst WHERE user_name='another_user2';

SET enable_analyzer=1;

EXPLAIN SELECT * FROM visits_order_dst WHERE user_name='another_user2';
