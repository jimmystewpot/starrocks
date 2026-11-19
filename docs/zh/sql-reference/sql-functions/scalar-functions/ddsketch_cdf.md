---
displayed_sidebar: docs
description: "从序列化的 Datadog DDSketch VARBINARY 草图中计算近似累积分布比例 P(X <= value)。"
---

# ddsketch_cdf

标量函数。从序列化的 Datadog DDSketch 草图（`VARBINARY`）中计算近似累积分布函数（CDF）比例 $P(X \le \text{value}) \in [0.0, 1.0]$。非常适合直接从预聚合草图计算服务等级目标（SLO）达标率（例如延迟不超过 `200 ms` 的请求比例）。

支持传入标量 `DOUBLE` 阈值（返回 `DOUBLE`）或 `ARRAY<DOUBLE>` 阈值数组（单次反序列化草图评估所有阈值并返回 `ARRAY<DOUBLE>`）。

:::note
- 若 `sketch` 为 `NULL`、0 字节或空草图（`count = 0`），则返回 `NULL`。
- 在标量重载中，若 `value` 为 `NULL` 或 `NaN`，则返回 `NULL`。在数组重载中，`values` 中的 `NULL` 或 `NaN` 元素会在返回的 `ARRAY<DOUBLE>` 对应位置生成 `NULL` 元素。
- 草图每个存储区的最大桶数固定为 `max_bins = 2048`（累积时 `relative_accuracy` 默认值为 `0.01`，范围 `[0.001, 0.5]`），函数会累加所有桶中点代表值小于或等于 `value` 的桶计数占总计数的比例。
:::

## 语法

```Haskell
DOUBLE ddsketch_cdf(VARBINARY sketch, DOUBLE value)
ARRAY<DOUBLE> ddsketch_cdf(VARBINARY sketch, ARRAY<DOUBLE> values)
```

- `sketch`：包含序列化 DDSketch 草图的 `VARBINARY` 值。
- `value` / `values`：用于评估 $P(X \le \text{value})$ 的 `DOUBLE` 阈值或 `ARRAY<DOUBLE>` 阈值数组。

## 示例

```SQL
-- 计算 SLO 达标率（耗时 <= 100ms、200ms、500ms 的请求占比）
SELECT
    service,
    ddsketch_cdf(ddsketch_combine(latency_sketch), 200.0) AS slo_under_200ms,
    ddsketch_cdf(ddsketch_combine(latency_sketch), [100.0, 200.0, 500.0]) AS slo_tiers
FROM mv_service_latency
GROUP BY service;
```

## 关键词

DDSKETCH_CDF, DDSKETCH_QUANTILE, DDSKETCH_ACCUMULATE, DDSKETCH_COMBINE
