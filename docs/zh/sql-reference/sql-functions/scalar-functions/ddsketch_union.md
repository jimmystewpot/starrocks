---
displayed_sidebar: docs
description: "对两个序列化的 Datadog DDSketch VARBINARY 草图执行标量两两合并，返回合并后的单个草图。"
products: [oss, enterprise, cloud, anywhere]
---

# ddsketch_union

对两个序列化的 Datadog DDSketch 草图（`VARBINARY`）执行标量两两并集运算。确定性地合并 `sketch_a` 和 `sketch_b`，并以 `VARBINARY` 格式返回标准 `ddsketch.proto` 格式的合并草图。

如需跨行对多个草图执行聚合合并，请使用 [`ddsketch_combine`](../aggregate-functions/ddsketch_combine.md)。

:::note
- 两个输入草图必须具有兼容的 `IndexMapping` 参数（`gamma` / `relative_accuracy` 位于 `[0.001, 0.5]`，默认 `0.01`，以及 `index_offset`）。合并具有不同 `relative_accuracy` 的两个草图（包括通过 `ddsketch_empty(0.02)` 创建的不同精度空草图）在任意参数顺序下都会报错。
- 合并后的草图通过确定性最低桶折叠将每个正数或负数存储区的最大容量固定为 `max_bins = 2048`。
- 若 `sketch_a` 或 `sketch_b` 任一为 `NULL`，则返回 `NULL`。
:::

## 语法

```Haskell
VARBINARY ddsketch_union(VARBINARY sketch_a, VARBINARY sketch_b)
```

- `sketch_a`、`sketch_b`：包含序列化 DDSketch 草图的 `VARBINARY` 值。

## 示例

```SQL
-- 合并同一行两个列中的读延迟草图与写延迟草图
SELECT
    host,
    ddsketch_quantile(ddsketch_union(read_latency_sk, write_latency_sk), 0.99) AS combined_p99_ms
FROM storage_node_sketches;
```

## 关键词

DDSKETCH_UNION, DDSKETCH_COMBINE, DDSKETCH_ACCUMULATE, DDSKETCH_QUANTILE
