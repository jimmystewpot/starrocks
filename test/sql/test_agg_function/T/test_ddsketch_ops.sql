-- name: test_ddsketch_ops
CREATE TABLE t1 (
  id BIGINT NOT NULL,
  grp VARCHAR(8) NOT NULL,
  val DOUBLE NOT NULL,
  weight BIGINT NOT NULL
)
DUPLICATE KEY(id)
DISTRIBUTED BY HASH(id) BUCKETS 4
PROPERTIES ("replication_num" = "1");

insert into t1 select generate_series, "A", cast(generate_series as double), 2 from table(generate_series(1, 1000));
insert into t1 select generate_series, "B", cast(generate_series as double), 3 from table(generate_series(501, 1500));

-- Use Case 1: Composable sketch lifecycle (ddsketch_empty, ddsketch_accumulate, ddsketch_accumulate_weighted,
-- ddsketch_combine, ddsketch_union, ddsketch_quantile, ddsketch_cdf, ddsketch_count, ddsketch_sum,
-- ddsketch_avg, ddsketch_min, ddsketch_max)
select length(ddsketch_empty()), length(ddsketch_empty(0.02)), ddsketch_count(ddsketch_empty()),
       ddsketch_quantile(ddsketch_empty(), 0.5), ddsketch_sum(ddsketch_empty()),
       ddsketch_avg(ddsketch_empty()), ddsketch_min(ddsketch_empty()), ddsketch_max(ddsketch_empty());

-- Exact formula verification on small weighted dataset: (10.0, 25), (20.0, 25), (50.0, 45), (100.0, 5)
-- gamma = 1.01/0.99, k(10)=115 (v=10.0747), k(20)=149 (v=19.8867), k(50)=195 (v=49.9030), k(100)=230 (v=100.4946)
with small_sk as (
  select ddsketch_accumulate_weighted(v, w) as s
  from (
    select 10.0 as v, 25 as w union all
    select 20.0 as v, 25 as w union all
    select 50.0 as v, 45 as w union all
    select 100.0 as v, 5 as w
  ) t
)
select ddsketch_count(s),
       round(ddsketch_min(s), 4),
       round(ddsketch_max(s), 4),
       round(ddsketch_sum(s), 4),
       round(ddsketch_avg(s), 4),
       round(ddsketch_quantile(s, 0.50), 4),
       round(ddsketch_quantile(s, 0.95), 4),
       round(ddsketch_quantile(s, 0.99), 4),
       ddsketch_cdf(s, 25.0),
       ddsketch_cdf(s, [25.0, 60.0])
from small_sk;

-- Per-group ddsketch_accumulate and scalar accessors (scalar & array quantile and cdf)
with grp_sk as (select grp, ddsketch_accumulate(val) as s from t1 where grp = 'A' group by grp)
select grp,
       ddsketch_count(s),
       round(ddsketch_min(s), 4),
       round(ddsketch_max(s), 4),
       round(ddsketch_quantile(s, 0.50), 4),
       round(ddsketch_quantile(s, 0.99), 4),
       round(ddsketch_quantile(s, [0.50, 0.95, 0.99])[1], 4),
       round(ddsketch_quantile(s, [0.50, 0.95, 0.99])[2], 4),
       round(ddsketch_quantile(s, [0.50, 0.95, 0.99])[3], 4),
       ddsketch_cdf(s, 500.0),
       ddsketch_cdf(s, [250.0, 500.0, 750.0])
from grp_sk;

-- Determinism: ddsketch_combine across groups vs ddsketch_union pairwise vs full table ddsketch_accumulate
with a as (select ddsketch_accumulate(val) as s from t1 where grp = 'A'),
     b as (select ddsketch_accumulate(val) as s from t1 where grp = 'B'),
     grp_sk as (select grp, ddsketch_accumulate(val) as s from t1 group by grp),
     combined as (select ddsketch_combine(s) as s from grp_sk),
     unioned as (select ddsketch_union(a.s, b.s) as s from a, b),
     full_sk as (select ddsketch_accumulate(val) as s from t1)
select cast(combined.s = unioned.s as int),
       cast(combined.s = full_sk.s as int),
       ddsketch_count(combined.s),
       round(ddsketch_quantile(combined.s, 0.50), 4),
       round(ddsketch_quantile(combined.s, 0.95), 4),
       round(ddsketch_quantile(combined.s, 0.99), 4)
from combined, unioned, full_sk;

-- Custom relative_accuracy (0.02) and NULL propagation
with custom_sk as (select ddsketch_accumulate(val, 0.02) as s from t1 where grp = 'A')
select ddsketch_count(s),
       cast(abs(ddsketch_quantile(s, 0.50) - 500.0) <= 0.02 * 500.0 as int),
       cast(abs(ddsketch_quantile(s, 0.99) - 990.0) <= 0.02 * 990.0 as int)
from custom_sk;

select ddsketch_quantile(NULL, 0.5),
       ddsketch_quantile(NULL, [0.5, 0.99]),
       ddsketch_cdf(NULL, 100.0),
       ddsketch_cdf(NULL, [100.0, 200.0]),
       ddsketch_count(NULL),
       ddsketch_sum(NULL),
       ddsketch_avg(NULL),
       ddsketch_min(NULL),
       ddsketch_max(NULL),
       cast(ddsketch_union(NULL, NULL) is NULL as int);

-- Use Case 2: Dynamic multi-metric telemetry nested in MAP<VARCHAR(32), VARBINARY> and ARRAY<VARBINARY>
CREATE TABLE host_telemetry_sketches (
  host VARCHAR(32) NOT NULL,
  metric_sketches MAP<VARCHAR(32), VARBINARY>,
  sketch_list ARRAY<VARBINARY>
)
DUPLICATE KEY(host)
DISTRIBUTED BY HASH(host) BUCKETS 2
PROPERTIES ("replication_num" = "1");

insert into host_telemetry_sketches
select 'web-01',
       map{'rpc_ms': a.s, 'db_ms': b.s},
       [a.s, b.s]
from (select ddsketch_accumulate(val) as s from t1 where grp = 'A') a,
     (select ddsketch_accumulate(val) as s from t1 where grp = 'B') b;

select host,
       ddsketch_count(metric_sketches['rpc_ms']),
       round(ddsketch_quantile(metric_sketches['rpc_ms'], 0.99), 4),
       round(ddsketch_quantile(metric_sketches['rpc_ms'], [0.50, 0.95, 0.99])[1], 4),
       round(ddsketch_quantile(metric_sketches['rpc_ms'], [0.50, 0.95, 0.99])[2], 4),
       round(ddsketch_quantile(metric_sketches['rpc_ms'], [0.50, 0.95, 0.99])[3], 4),
       ddsketch_count(sketch_list[1]),
       round(ddsketch_quantile(sketch_list[1], 0.50), 4)
from host_telemetry_sketches;

-- Use Case 3: One-shot ddsketch_percentile & ddsketch_percentile_weighted and async MV rollup & rewrite
select round(ddsketch_percentile(val, 0.50), 4),
       round(ddsketch_percentile(val, 0.99, 0.01), 4),
       round(ddsketch_percentile(val, [0.50, 0.95, 0.99])[1], 4),
       round(ddsketch_percentile(val, [0.50, 0.95, 0.99])[2], 4),
       round(ddsketch_percentile(val, [0.50, 0.95, 0.99])[3], 4),
       round(ddsketch_percentile_weighted(val, weight, 0.50), 4),
       round(ddsketch_percentile_weighted(val, weight, [0.50, 0.99], 0.01)[1], 4),
       round(ddsketch_percentile_weighted(val, weight, [0.50, 0.99], 0.01)[2], 4)
from t1 where grp = 'A';

CREATE MATERIALIZED VIEW mv_ddsketch
DISTRIBUTED BY HASH(grp) BUCKETS 2
REFRESH MANUAL
AS SELECT grp, ddsketch_accumulate(val) as sk FROM t1 GROUP BY grp;

[UC]REFRESH MATERIALIZED VIEW mv_ddsketch with sync mode;

select grp,
       ddsketch_count(sk),
       round(ddsketch_quantile(sk, 0.50), 4),
       round(ddsketch_quantile(sk, 0.99), 4)
from mv_ddsketch
where grp = 'A';

select ddsketch_count(ddsketch_combine(sk)),
       round(ddsketch_quantile(ddsketch_combine(sk), 0.50), 4),
       round(ddsketch_quantile(ddsketch_combine(sk), 0.99), 4)
from mv_ddsketch;

select round(ddsketch_percentile(val, 0.50), 4),
       round(ddsketch_percentile(val, 0.99), 4),
       round(ddsketch_percentile(val, [0.50, 0.95, 0.99])[1], 4),
       round(ddsketch_percentile(val, [0.50, 0.95, 0.99])[2], 4),
       round(ddsketch_percentile(val, [0.50, 0.95, 0.99])[3], 4)
from t1;

-- 1-phase and 2-phase aggregation nullability on NOT NULL columns when all inputs in a group are ignored
CREATE TABLE t_not_null_ignored (
  id BIGINT NOT NULL,
  grp VARCHAR(8) NOT NULL,
  val DOUBLE NOT NULL,
  weight BIGINT NOT NULL,
  sk VARBINARY NOT NULL
)
DUPLICATE KEY(id)
DISTRIBUTED BY HASH(id) BUCKETS 2
PROPERTIES ("replication_num" = "1");

insert into t_not_null_ignored values
  (1, 'EMPTY', 10.0, 0, cast('' as varbinary)),
  (2, 'EMPTY', 20.0, -5, cast('' as varbinary));

SET new_planner_agg_stage = 1;
select grp,
       ddsketch_accumulate_weighted(val, weight),
       ddsketch_combine(sk),
       ddsketch_percentile_weighted(val, weight, 0.5),
       ddsketch_percentile_weighted(val, weight, [0.5, 0.99])
from t_not_null_ignored
group by grp;

SET new_planner_agg_stage = 2;
select grp,
       ddsketch_accumulate_weighted(val, weight),
       ddsketch_combine(sk),
       ddsketch_percentile_weighted(val, weight, 0.5),
       ddsketch_percentile_weighted(val, weight, [0.5, 0.99])
from t_not_null_ignored
group by grp;

SET new_planner_agg_stage = 0;
