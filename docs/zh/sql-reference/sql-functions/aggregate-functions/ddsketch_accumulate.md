---
displayed_sidebar: docs
description: "将数值列或带权重的数值累积到 Datadog DDSketch 草图中，并以 VARBINARY 格式返回序列化的 protobuf 草图。"
products: [oss, enterprise, cloud, anywhere]
---

# ddsketch_accumulate, ddsketch_accumulate_weighted

对数值表达式 `expr`（或带有整数频次权重 `weight` 的 `expr`）构建确定性的 Datadog DDSketch 草图，并以 `VARBINARY` 格式返回标准 `ddsketch.proto` 序列化的草图。与 [ddsketch_combine](./ddsketch_combine.md)、[ddsketch_quantile](../scalar-functions/ddsketch_quantile.md) 和 [ddsketch_cdf](../scalar-functions/ddsketch_cdf.md) 配合使用，可在多维表或异步物化视图中持久化、上卷和查询草图。

序列化的 `VARBINARY` 负载采用 Datadog 标准 `ddsketch.proto` 线上格式（对数 `IndexMapping`、`Interpolation::NONE` 以及连续稠密桶计数），可嵌套在 `ARRAY<VARBINARY>`、`MAP<VARCHAR, VARBINARY>` 或 `STRUCT` 中，也可与外部 Datadog DDSketch SDK 互操作。

:::note
- 每个正数或负数存储区的最大桶数（`max_bins`）在内部固定为 `2048`，采用确定性的最低桶折叠策略（`CollapsingLowestDenseStore`）。
- `relative_accuracy` 为可选参数，默认值为 `0.01`（1% 相对误差）。指定时必须为 `[0.001, 0.5]` 范围内的 `DOUBLE` 常量字面量。
- `expr` 为 `NULL` 或非有限值（`NaN`、`±Inf`）的行会被忽略。在 `ddsketch_accumulate_weighted` 中，`weight <= 0` 或 `weight` 为 `NULL` 的行会被忽略；单行权重或草图总计数超过 `2^53`（`9007199254740992`）时将报错。
- 若分组内没有累积任何有效观测值，则返回 `NULL`。
:::

## 语法

```Haskell
VARBINARY ddsketch_accumulate(DOUBLE expr [, DOUBLE relative_accuracy])
VARBINARY ddsketch_accumulate_weighted(DOUBLE expr, BIGINT weight [, DOUBLE relative_accuracy])
```

- `expr`：用于汇总分布的数值表达式。
- `weight`：`ddsketch_accumulate_weighted` 中每条观测值的整数权重（`BIGINT`）。`weight <= 0` 的行会被忽略。
- `relative_accuracy`：可选的 `DOUBLE` 常量，取值范围为 `[0.001, 0.5]`，表示目标相对精度 $\alpha$，默认值为 `0.01`。

## 示例

```SQL
-- 在表或异步物化视图中按服务持久化延迟草图
CREATE TABLE service_latency_sketches AS
SELECT
    service,
    ddsketch_accumulate(latency_ms) AS latency_sketch,
    ddsketch_accumulate_weighted(latency_ms, sample_count, 0.01) AS weighted_sketch
FROM rpc_traces
GROUP BY service;

-- 从存储的草图中查询多个分位数和观测计数
SELECT
    service,
    ddsketch_count(latency_sketch) AS total_requests,
    ddsketch_quantile(latency_sketch, 0.50) AS p50_ms,
    ddsketch_quantile(latency_sketch, 0.99) AS p99_ms
FROM service_latency_sketches;
```

## 关键词

DDSKETCH_ACCUMULATE, DDSKETCH_ACCUMULATE_WEIGHTED, DDSKETCH_COMBINE, DDSKETCH_QUANTILE, DDSKETCH_PERCENTILE
