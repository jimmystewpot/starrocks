---
displayed_sidebar: docs
description: "基于 Datadog DDSketch 计算具有相对误差保证的确定性近似百分位数或加权近似百分位数。"
---

# ddsketch_percentile, ddsketch_percentile_weighted

使用 Datadog DDSketch 在单次查询中计算 `expr`（或带有整数频次权重 `weight` 的 `expr`）的一个或多个具有相对误差保证的确定性近似百分位数。当 `p` 为标量 `DOUBLE` 时返回 `DOUBLE`，当 `p` 为 `ARRAY<DOUBLE>` 时返回 `ARRAY<DOUBLE>`。

当异步物化视图存储了具有相同 `relative_accuracy` 的 [`ddsketch_accumulate(expr [, relative_accuracy])`](./ddsketch_accumulate.md) 时，StarRocks 优化器会自动将针对基表的 `ddsketch_percentile(expr, p [, relative_accuracy])` 查询透明改写为针对物化视图的 [`ddsketch_quantile`](../scalar-functions/ddsketch_quantile.md)（上卷时配合 [`ddsketch_combine`](./ddsketch_combine.md)）。

:::note
- 每个正数或负数存储区的最大桶数（`max_bins`）在内部固定为 `2048`，采用确定性的最低桶折叠策略（`CollapsingLowestDenseStore`），保证多阶段分布式聚合下百分位数估计结果与数据到达及合并顺序无关。
- `relative_accuracy` 为可选参数，默认值为 `0.01`（1% 相对误差）。指定时必须为 `[0.001, 0.5]` 范围内的 `DOUBLE` 常量字面量。
- `p` 必须为 `[0.0, 1.0]` 范围内的非空 `DOUBLE` 常量，或每个元素均非空且位于 `[0.0, 1.0]` 范围内的非空常量数组 `ARRAY<DOUBLE>`。
- 若未累积任何有效有限观测值（在 `ddsketch_percentile_weighted` 中需 `weight > 0`），则返回 `NULL`。
:::

## 语法

```Haskell
DOUBLE ddsketch_percentile(DOUBLE expr, DOUBLE p [, DOUBLE relative_accuracy])
ARRAY<DOUBLE> ddsketch_percentile(DOUBLE expr, ARRAY<DOUBLE> p [, DOUBLE relative_accuracy])
DOUBLE ddsketch_percentile_weighted(DOUBLE expr, BIGINT weight, DOUBLE p [, DOUBLE relative_accuracy])
ARRAY<DOUBLE> ddsketch_percentile_weighted(DOUBLE expr, BIGINT weight, ARRAY<DOUBLE> p [, DOUBLE relative_accuracy])
```

- `expr`：要计算百分位数的数值表达式。
- `weight`：`ddsketch_percentile_weighted` 中每条观测值的整数权重（`BIGINT`）。`weight <= 0` 的行会被忽略。
- `p`：目标分位数（`[0.0, 1.0]` 内的 `DOUBLE`）或目标分位数常量数组（所有元素位于 `[0.0, 1.0]` 内的非空 `ARRAY<DOUBLE>`）。
- `relative_accuracy`：可选的 `DOUBLE` 常量，取值范围为 `[0.001, 0.5]`，默认值为 `0.01`。

## 示例

```SQL
-- 单次扫描计算 p99 以及百分位数数组 [p50, p95, p99]
SELECT
    service,
    ddsketch_percentile(latency_ms, 0.99) AS p99_ms,
    ddsketch_percentile(latency_ms, [0.50, 0.95, 0.99], 0.01) AS latency_quantiles,
    ddsketch_percentile_weighted(latency_ms, sample_count, 0.99) AS weighted_p99_ms
FROM rpc_traces
GROUP BY service;
```

## 关键词

DDSKETCH_PERCENTILE, DDSKETCH_PERCENTILE_WEIGHTED, DDSKETCH_ACCUMULATE, DDSKETCH_QUANTILE, PERCENTILE_APPROX
