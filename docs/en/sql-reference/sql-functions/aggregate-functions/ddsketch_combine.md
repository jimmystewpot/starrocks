---
displayed_sidebar: docs
description: "Merges serialized Datadog DDSketch VARBINARY values across rows into a single combined sketch with deterministic accuracy."
---

# ddsketch_combine

Aggregate function that deterministically merges serialized Datadog DDSketch payloads (`VARBINARY`) across rows and returns a single combined sketch in canonical `ddsketch.proto` format as `VARBINARY`.

Input sketches can be produced by [`ddsketch_accumulate`](./ddsketch_accumulate.md), [`ddsketch_accumulate_weighted`](./ddsketch_accumulate.md), [`ddsketch_union`](../scalar-functions/ddsketch_union.md), [`ddsketch_empty`](../scalar-functions/ddsketch_quantile.md), or loaded externally from Parquet or Iceberg in `ddsketch.proto` format.

:::note
- All merged sketches must share the same `IndexMapping` (`gamma` / `relative_accuracy` in `[0.001, 0.5]`, default `0.01`, and `index_offset`). Merging sketches built with different `relative_accuracy` parameters (including an empty sketch created with a different accuracy via `ddsketch_empty(0.02)`) returns an error, regardless of row arrival order.
- The combined sketch enforces a fixed maximum capacity of `max_bins = 2048` per positive or negative store via deterministic lowest-bin collapsing, ensuring 100% order-independent results across distributed execution.
- `NULL` values and 0-byte `VARBINARY` slices are ignored. Returns `NULL` if all inputs in a group are `NULL` or 0-byte slices.
:::

## Syntax

```Haskell
VARBINARY ddsketch_combine(VARBINARY sketch)
```

- `sketch`: a `VARBINARY` column containing serialized DDSketches.

## Examples

```SQL
-- Roll up minute-level sketches to service-level percentiles and SLO compliance
SELECT
    service,
    ddsketch_count(ddsketch_combine(latency_sketch)) AS total_requests,
    ddsketch_quantile(ddsketch_combine(latency_sketch), [0.50, 0.95, 0.99]) AS latency_quantiles,
    ddsketch_cdf(ddsketch_combine(latency_sketch), 200.0) AS slo_under_200ms_ratio
FROM mv_service_latency
GROUP BY service;
```

## Keywords

DDSKETCH_COMBINE, DDSKETCH_ACCUMULATE, DDSKETCH_UNION, DDSKETCH_QUANTILE, DDSKETCH_PERCENTILE
