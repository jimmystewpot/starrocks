---
displayed_sidebar: docs
description: "Computes the approximate cumulative distribution ratio P(X <= value) from a serialized Datadog DDSketch VARBINARY."
---

# ddsketch_cdf

Scalar function that evaluates the approximate cumulative distribution function (CDF) ratio $P(X \le \text{value}) \in [0.0, 1.0]$ from a serialized Datadog DDSketch (`VARBINARY`). Useful for computing service-level objective (SLO) compliance ratios (for example, the fraction of requests completing within `200 ms`) directly from pre-aggregated sketches.

Accepts either a scalar `DOUBLE` threshold (returning `DOUBLE`) or an `ARRAY<DOUBLE>` of thresholds (evaluating all thresholds in a single sketch deserialization and returning `ARRAY<DOUBLE>`).

:::note
- Returns `NULL` if `sketch` is `NULL`, 0-byte, or empty (`count = 0`).
- In the scalar overload, returns `NULL` if `value` is `NULL` or `NaN`. In the array overload, `NULL` or `NaN` elements in `values` produce `NULL` elements at the corresponding positions in the returned `ARRAY<DOUBLE>`.
- The sketch uses a fixed maximum capacity of `max_bins = 2048` per store (`relative_accuracy` defaults to `0.01` in `[0.001, 0.5]` when accumulated) and sums the counts of all buckets whose bucket-midpoint representative value is less than or equal to `value`.
:::

## Syntax

```Haskell
DOUBLE ddsketch_cdf(VARBINARY sketch, DOUBLE value)
ARRAY<DOUBLE> ddsketch_cdf(VARBINARY sketch, ARRAY<DOUBLE> values)
```

- `sketch`: a `VARBINARY` value containing a serialized DDSketch.
- `value` / `values`: threshold `DOUBLE` or `ARRAY<DOUBLE>` at which to evaluate $P(X \le \text{value})$.

## Examples

```SQL
-- Compute SLO compliance ratios (% of requests <= 100ms, 200ms, 500ms)
SELECT
    service,
    ddsketch_cdf(ddsketch_combine(latency_sketch), 200.0) AS slo_under_200ms,
    ddsketch_cdf(ddsketch_combine(latency_sketch), [100.0, 200.0, 500.0]) AS slo_tiers
FROM mv_service_latency
GROUP BY service;
```

## Keywords

DDSKETCH_CDF, DDSKETCH_QUANTILE, DDSKETCH_ACCUMULATE, DDSKETCH_COMBINE
