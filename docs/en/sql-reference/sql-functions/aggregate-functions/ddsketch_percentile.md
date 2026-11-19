---
displayed_sidebar: docs
description: "Computes deterministic relative-error-bounded approximate percentiles or weighted percentiles using Datadog DDSketch."
---

# ddsketch_percentile, ddsketch_percentile_weighted

Computes one or more deterministic, relative-error-bounded approximate percentiles over `expr` (or weighted `expr` with integer frequency `weight`) in a single query using Datadog DDSketch. Returns a `DOUBLE` when `p` is a scalar `DOUBLE`, or an `ARRAY<DOUBLE>` when `p` is an `ARRAY<DOUBLE>`.

When an async Materialized View stores [`ddsketch_accumulate(expr [, relative_accuracy])`](./ddsketch_accumulate.md) with the same `relative_accuracy`, the StarRocks optimizer automatically rewrites `ddsketch_percentile(expr, p [, relative_accuracy])` queries into [`ddsketch_quantile`](../scalar-functions/ddsketch_quantile.md) (and [`ddsketch_combine`](./ddsketch_combine.md) when rolling up) against the Materialized View.

:::note
- The maximum number of bins per positive or negative store (`max_bins`) is fixed internally at `2048` using deterministic lowest-bin collapsing (`CollapsingLowestDenseStore`), guaranteeing order-independent percentile estimates across multi-phase distributed aggregation.
- `relative_accuracy` is optional and defaults to `0.01` (1% relative error). When specified, it must be a constant `DOUBLE` literal in `[0.001, 0.5]`.
- `p` must be a non-null constant `DOUBLE` in `[0.0, 1.0]` or a non-empty constant `ARRAY<DOUBLE>` where every element is non-null and in `[0.0, 1.0]`.
- Returns `NULL` if no valid finite observations (with `weight > 0` for `ddsketch_percentile_weighted`) are accumulated.
:::

## Syntax

```Haskell
DOUBLE ddsketch_percentile(DOUBLE expr, DOUBLE p [, DOUBLE relative_accuracy])
ARRAY<DOUBLE> ddsketch_percentile(DOUBLE expr, ARRAY<DOUBLE> p [, DOUBLE relative_accuracy])
DOUBLE ddsketch_percentile_weighted(DOUBLE expr, BIGINT weight, DOUBLE p [, DOUBLE relative_accuracy])
ARRAY<DOUBLE> ddsketch_percentile_weighted(DOUBLE expr, BIGINT weight, ARRAY<DOUBLE> p [, DOUBLE relative_accuracy])
```

- `expr`: numeric expression to evaluate percentiles over.
- `weight`: integer frequency (`BIGINT`) for each observation in `ddsketch_percentile_weighted`. Rows with `weight <= 0` are ignored.
- `p`: target quantile (`DOUBLE` in `[0.0, 1.0]`) or non-empty constant array of target quantiles (`ARRAY<DOUBLE>` with all elements in `[0.0, 1.0]`).
- `relative_accuracy`: optional constant `DOUBLE` in `[0.001, 0.5]`, defaulting to `0.01`.

## Examples

```SQL
-- Compute p99 and an array of percentiles [p50, p95, p99] in one pass
SELECT
    service,
    ddsketch_percentile(latency_ms, 0.99) AS p99_ms,
    ddsketch_percentile(latency_ms, [0.50, 0.95, 0.99], 0.01) AS latency_quantiles,
    ddsketch_percentile_weighted(latency_ms, sample_count, 0.99) AS weighted_p99_ms
FROM rpc_traces
GROUP BY service;
```

## Keywords

DDSKETCH_PERCENTILE, DDSKETCH_PERCENTILE_WEIGHTED, DDSKETCH_ACCUMULATE, DDSKETCH_QUANTILE, PERCENTILE_APPROX
