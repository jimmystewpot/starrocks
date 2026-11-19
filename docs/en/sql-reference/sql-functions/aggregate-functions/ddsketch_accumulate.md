---
displayed_sidebar: docs
description: "Builds a Datadog DDSketch from numeric column values or weighted values and returns a serialized protobuf sketch as VARBINARY."
---

# ddsketch_accumulate, ddsketch_accumulate_weighted

Builds a deterministic Datadog DDSketch over `expr` (or weighted `expr` with integer frequency `weight`) and returns the sketch serialized in the canonical `ddsketch.proto` format as `VARBINARY`. Pair with [ddsketch_combine](./ddsketch_combine.md), [ddsketch_quantile](../scalar-functions/ddsketch_quantile.md), and [ddsketch_cdf](../scalar-functions/ddsketch_cdf.md) to materialize, roll up, and query sketches across multiple dimensions or inside async Materialized Views.

The serialized `VARBINARY` payload uses Datadog's standard `ddsketch.proto` wire format (logarithmic `IndexMapping` with `Interpolation::NONE` and contiguous dense bin counts) and can be nested inside `ARRAY<VARBINARY>`, `MAP<VARCHAR, VARBINARY>`, or `STRUCT`, or exchanged with external Datadog DDSketch SDKs.

:::note
- The maximum number of bins per positive or negative store (`max_bins`) is fixed internally at `2048` using deterministic lowest-bin collapsing (`CollapsingLowestDenseStore`).
- `relative_accuracy` is optional and defaults to `0.01` (1% relative error). When specified, it must be a constant `DOUBLE` literal in `[0.001, 0.5]`.
- `NULL` and non-finite (`NaN`, `±Inf`) values of `expr` are ignored. In `ddsketch_accumulate_weighted`, rows with `weight <= 0` or `NULL` weight are ignored, and individual weights or total sketch counts exceeding `2^53` (`9007199254740992`) return an error.
- If no valid observations are accumulated in a group, the function returns `NULL`.
:::

## Syntax

```Haskell
VARBINARY ddsketch_accumulate(DOUBLE expr [, DOUBLE relative_accuracy])
VARBINARY ddsketch_accumulate_weighted(DOUBLE expr, BIGINT weight [, DOUBLE relative_accuracy])
```

- `expr`: numeric expression whose distribution is summarized.
- `weight`: integer frequency (`BIGINT`) for each observation in `ddsketch_accumulate_weighted`. Rows with `weight <= 0` are ignored.
- `relative_accuracy`: optional constant `DOUBLE` in `[0.001, 0.5]` specifying the target relative accuracy $\alpha$. Defaults to `0.01`.

## Examples

```SQL
-- Materialize per-service sketches in a table or async Materialized View
CREATE TABLE service_latency_sketches AS
SELECT
    service,
    ddsketch_accumulate(latency_ms) AS latency_sketch,
    ddsketch_accumulate_weighted(latency_ms, sample_count, 0.01) AS weighted_sketch
FROM rpc_traces
GROUP BY service;

-- Query multiple quantiles and count from the stored sketch
SELECT
    service,
    ddsketch_count(latency_sketch) AS total_requests,
    ddsketch_quantile(latency_sketch, 0.50) AS p50_ms,
    ddsketch_quantile(latency_sketch, 0.99) AS p99_ms
FROM service_latency_sketches;
```

## Keywords

DDSKETCH_ACCUMULATE, DDSKETCH_ACCUMULATE_WEIGHTED, DDSKETCH_COMBINE, DDSKETCH_QUANTILE, DDSKETCH_PERCENTILE
