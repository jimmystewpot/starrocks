---
displayed_sidebar: docs
description: "Extracts quantiles, observation counts, and bucket-midpoint summary statistics from a serialized Datadog DDSketch VARBINARY."
---

# ddsketch_quantile, ddsketch_min, ddsketch_max, ddsketch_count, ddsketch_sum, ddsketch_avg, ddsketch_empty

Scalar functions that construct an empty Datadog DDSketch or extract quantiles, exact observation counts, and approximate summary statistics from a serialized DDSketch (`VARBINARY` in canonical `ddsketch.proto` format).

Input sketches can come from [`ddsketch_accumulate`](../aggregate-functions/ddsketch_accumulate.md), [`ddsketch_combine`](../aggregate-functions/ddsketch_combine.md), [`ddsketch_union`](./ddsketch_union.md), nested `MAP<VARCHAR, VARBINARY>` or `ARRAY<VARBINARY>` columns, or external Parquet/Iceberg files written by Datadog DDSketch SDKs.

:::note
- `ddsketch_count` returns the exact `BIGINT` observation count accumulated in the sketch (`0` for an empty or 0-byte sketch, `NULL` for a `NULL` sketch).
- `ddsketch_sum`, `ddsketch_avg`, `ddsketch_min`, and `ddsketch_max` are **bucket-midpoint approximations** computed from the sketch's logarithmic buckets rather than exact raw IEEE-754 floats. Specifically, `ddsketch_min(sketch)` is equivalent to `ddsketch_quantile(sketch, 0.0)`, `ddsketch_max(sketch)` is equivalent to `ddsketch_quantile(sketch, 1.0)`, and `ddsketch_sum(sketch)` sums `bin_midpoint(k) * count[k]` across all bins. If the lowest bins have collapsed due to the fixed `max_bins = 2048` limit, `ddsketch_min`, `ddsketch_sum`, and `ddsketch_avg` reflect the collapsed lowest bin. When exact `SUM`, `AVG`, `MIN`, or `MAX` values are required, compute standard SQL aggregates alongside the sketch.
- `ddsketch_empty([relative_accuracy])` returns a canonical 11-byte serialized empty sketch with an initialized `IndexMapping` (`count = 0`). `relative_accuracy` defaults to `0.01` and must be in `[0.001, 0.5]`.
:::

## Syntax

```Haskell
DOUBLE ddsketch_quantile(VARBINARY sketch, DOUBLE p)
ARRAY<DOUBLE> ddsketch_quantile(VARBINARY sketch, ARRAY<DOUBLE> p)
DOUBLE ddsketch_min(VARBINARY sketch)
DOUBLE ddsketch_max(VARBINARY sketch)
BIGINT ddsketch_count(VARBINARY sketch)
DOUBLE ddsketch_sum(VARBINARY sketch)
DOUBLE ddsketch_avg(VARBINARY sketch)
VARBINARY ddsketch_empty([DOUBLE relative_accuracy])
```

- `sketch`: a `VARBINARY` value containing a serialized DDSketch.
- `p`: target quantile (`DOUBLE` in `[0.0, 1.0]`) or array of target quantiles (`ARRAY<DOUBLE>`). In scalar `ddsketch_quantile`, out-of-range or `NULL` quantile targets return `NULL` (or `NULL` elements inside the returned `ARRAY<DOUBLE>`).
- `relative_accuracy`: optional `DOUBLE` in `[0.001, 0.5]` for `ddsketch_empty`, defaulting to `0.01`.

## Examples

```SQL
-- Query multiple percentiles and summary statistics from a stored sketch column
SELECT
    service,
    ddsketch_count(latency_sketch) AS total_requests,
    ddsketch_min(latency_sketch) AS approx_min_ms,
    ddsketch_max(latency_sketch) AS approx_max_ms,
    ddsketch_sum(latency_sketch) AS approx_sum_ms,
    ddsketch_avg(latency_sketch) AS approx_avg_ms,
    ddsketch_quantile(latency_sketch, 0.99) AS p99_ms,
    ddsketch_quantile(latency_sketch, [0.50, 0.95, 0.99]) AS p50_p95_p99_ms
FROM service_latency_sketches;

-- Extract quantiles from sketches nested inside a MAP<VARCHAR, VARBINARY> column
SELECT
    host,
    ddsketch_quantile(metric_sketches['rpc_ms'], 0.99) AS rpc_p99,
    ddsketch_quantile(metric_sketches['rpc_ms'], [0.50, 0.95, 0.99]) AS rpc_quantiles
FROM host_telemetry_sketches;
```

## Keywords

DDSKETCH_QUANTILE, DDSKETCH_MIN, DDSKETCH_MAX, DDSKETCH_COUNT, DDSKETCH_SUM, DDSKETCH_AVG, DDSKETCH_EMPTY
