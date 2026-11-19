---
displayed_sidebar: docs
description: "跨行确定性地合并序列化的 Datadog DDSketch VARBINARY 草图，返回单个合并后的草图。"
---

# ddsketch_combine

聚合函数。跨行确定性地合并序列化的 Datadog DDSketch 草图（`VARBINARY`），并以 `VARBINARY` 格式返回标准 `ddsketch.proto` 格式的合并草图。

输入草图可以由 [`ddsketch_accumulate`](./ddsketch_accumulate.md)、[`ddsketch_accumulate_weighted`](./ddsketch_accumulate.md)、[`ddsketch_union`](../scalar-functions/ddsketch_union.md)、[`ddsketch_empty`](../scalar-functions/ddsketch_quantile.md) 生成，也可以从 Parquet 或 Iceberg 作为 `ddsketch.proto` 格式的 `VARBINARY` 外部加载。

:::note
- 所有参与合并的草图必须具有相同的 `IndexMapping`（`[0.001, 0.5]` 范围内的 `gamma` / `relative_accuracy`，默认值为 `0.01`，以及 `index_offset`）。合并使用不同 `relative_accuracy` 参数构建的草图（包括通过 `ddsketch_empty(0.02)` 创建的不同精度空草图）将报错，且与行到达顺序无关。
- 合并后的草图通过确定性最低桶折叠将每个正数或负数存储区的最大容量固定为 `max_bins = 2048`，确保分布式执行下结果 100% 与顺序无关。
- `NULL` 值和 0 字节 `VARBINARY` 会被忽略。若分组内所有输入均为 `NULL` 或 0 字节，则返回 `NULL`。
:::

## 语法

```Haskell
VARBINARY ddsketch_combine(VARBINARY sketch)
```

- `sketch`：包含序列化 DDSketch 草图的 `VARBINARY` 列。

## 示例

```SQL
-- 将分钟级草图上卷为服务级分位数与 SLO 达标率
SELECT
    service,
    ddsketch_count(ddsketch_combine(latency_sketch)) AS total_requests,
    ddsketch_quantile(ddsketch_combine(latency_sketch), [0.50, 0.95, 0.99]) AS latency_quantiles,
    ddsketch_cdf(ddsketch_combine(latency_sketch), 200.0) AS slo_under_200ms_ratio
FROM mv_service_latency
GROUP BY service;
```

## 关键词

DDSKETCH_COMBINE, DDSKETCH_ACCUMULATE, DDSKETCH_UNION, DDSKETCH_QUANTILE, DDSKETCH_PERCENTILE
