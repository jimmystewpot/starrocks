---
displayed_sidebar: docs
description: "从序列化的 Datadog DDSketch VARBINARY 草图中提取分位数、观测计数以及基于桶中点的汇总统计信息。"
products: [oss, enterprise, cloud, anywhere]
---

# ddsketch_quantile, ddsketch_min, ddsketch_max, ddsketch_count, ddsketch_sum, ddsketch_avg, ddsketch_empty

标量函数。用于构造空的 Datadog DDSketch 草图，或从序列化的 DDSketch 草图（标准 `ddsketch.proto` 格式的 `VARBINARY`）中提取分位数、精确观测计数和近似汇总统计信息。

输入草图可以来自 [`ddsketch_accumulate`](../aggregate-functions/ddsketch_accumulate.md)、[`ddsketch_combine`](../aggregate-functions/ddsketch_combine.md)、[`ddsketch_union`](./ddsketch_union.md)、嵌套的 `MAP<VARCHAR, VARBINARY>` 或 `ARRAY<VARBINARY>` 列，或由外部 Datadog DDSketch SDK 写入的 Parquet/Iceberg 文件。

:::note
- `ddsketch_count` 返回草图中累积的精确 `BIGINT` 观测值计数（空草图或 0 字节返回 `0`，`NULL` 输入返回 `NULL`）。
- `ddsketch_sum`、`ddsketch_avg`、`ddsketch_min` 和 `ddsketch_max` 是根据草图的对数桶计算的**桶中点近似值（bucket-midpoint approximations）**，而非精确的原始 IEEE-754 浮点数。具体而言，`ddsketch_min(sketch)` 等价于 `ddsketch_quantile(sketch, 0.0)`，`ddsketch_max(sketch)` 等价于 `ddsketch_quantile(sketch, 1.0)`，`ddsketch_sum(sketch)` 对所有桶计算 `bin_midpoint(k) * count[k]` 之和。如果由于固定上限 `max_bins = 2048` 触发了最低桶折叠，则 `ddsketch_min`、`ddsketch_sum` 和 `ddsketch_avg` 会反映折叠后的最低桶中点值。如需精确的 `SUM`、`AVG`、`MIN` 或 `MAX`，请与草图一起使用标准 SQL 聚合函数。
- `ddsketch_empty([relative_accuracy])` 返回带有已初始化 `IndexMapping`（`count = 0`）的 11 字节标准序列化空草图。`relative_accuracy` 默认值为 `0.01`，取值范围为 `[0.001, 0.5]`。
:::

## 语法

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

- `sketch`：包含序列化 DDSketch 草图的 `VARBINARY` 值。
- `p`：目标分位数（`[0.0, 1.0]` 内的 `DOUBLE`）或目标分位数数组（`ARRAY<DOUBLE>`）。在标量函数 `ddsketch_quantile` 中，超出 `[0.0, 1.0]` 范围或为 `NULL` 的分位数参数会返回 `NULL`（或在返回的 `ARRAY<DOUBLE>` 中对应位置产生 `NULL` 元素）。
- `relative_accuracy`：`ddsketch_empty` 的可选 `DOUBLE` 参数，取值范围为 `[0.001, 0.5]`，默认值为 `0.01`。

## 示例

```SQL
-- 从存储的草图列中查询多个百分位数和汇总统计信息
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

-- 从嵌套在 MAP<VARCHAR, VARBINARY> 列中的草图提取分位数
SELECT
    host,
    ddsketch_quantile(metric_sketches['rpc_ms'], 0.99) AS rpc_p99,
    ddsketch_quantile(metric_sketches['rpc_ms'], [0.50, 0.95, 0.99]) AS rpc_quantiles
FROM host_telemetry_sketches;
```

## 关键词

DDSKETCH_QUANTILE, DDSKETCH_MIN, DDSKETCH_MAX, DDSKETCH_COUNT, DDSKETCH_SUM, DDSKETCH_AVG, DDSKETCH_EMPTY
