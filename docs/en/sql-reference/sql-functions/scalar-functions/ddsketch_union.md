---
displayed_sidebar: docs
description: "Merges two serialized Datadog DDSketch VARBINARY values pairwise into a single combined sketch."
products: [oss, enterprise, cloud, anywhere]
---

# ddsketch_union

Scalar pairwise union of two serialized Datadog DDSketch payloads (`VARBINARY`). Deterministically merges `sketch_a` and `sketch_b` and returns a single serialized sketch in canonical `ddsketch.proto` format as `VARBINARY`.

For an aggregate union across rows in a table or group, use [`ddsketch_combine`](../aggregate-functions/ddsketch_combine.md).

:::note
- Both input sketches must have compatible `IndexMapping` parameters (`gamma` / `relative_accuracy` in `[0.001, 0.5]`, default `0.01`, and `index_offset`). Merging two sketches with mismatched `relative_accuracy` (including an empty sketch created with a different accuracy via `ddsketch_empty(0.02)`) returns an error in either argument order.
- The merged sketch enforces a fixed maximum capacity of `max_bins = 2048` per positive or negative store via deterministic lowest-bin collapsing.
- Returns `NULL` if either `sketch_a` or `sketch_b` is `NULL`.
:::

## Syntax

```Haskell
VARBINARY ddsketch_union(VARBINARY sketch_a, VARBINARY sketch_b)
```

- `sketch_a`, `sketch_b`: `VARBINARY` values containing serialized DDSketches.

## Examples

```SQL
-- Merge read and write latency sketches sitting in two columns of the same row
SELECT
    host,
    ddsketch_quantile(ddsketch_union(read_latency_sk, write_latency_sk), 0.99) AS combined_p99_ms
FROM storage_node_sketches;
```

## Keywords

DDSKETCH_UNION, DDSKETCH_COMBINE, DDSKETCH_ACCUMULATE, DDSKETCH_QUANTILE
