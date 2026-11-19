---
displayed_sidebar: docs
description: "シリアライズされた Datadog DDSketch VARBINARY から近似累積分布比 P(X <= value) を計算します。"
---

# ddsketch_cdf

スカラー関数。シリアライズされた Datadog DDSketch（`VARBINARY`）から近似累積分布関数（CDF）の比率 $P(X \le \text{value}) \in [0.0, 1.0]$ を評価します。事前集計されたスケッチから直接サービスレベル目標（SLO）の達成率（たとえば `200 ms` 以内に完了したリクエストの割合）を計算するのに適しています。

スカラー `DOUBLE` 閾値（`DOUBLE` を返す）または閾値の配列 `ARRAY<DOUBLE>`（1 回のスケッチデシリアライズですべての閾値を評価し `ARRAY<DOUBLE>` を返す）を受け付けます。

:::note
- `sketch` が `NULL`、0 バイト、または空スケッチ（`count = 0`）の場合は `NULL` を返します。
- スカラーオーバーロードでは、`value` が `NULL` または `NaN` の場合に `NULL` を返します。配列オーバーロードでは、`values` 内の `NULL` または `NaN` 要素に対して、返される `ARRAY<DOUBLE>` の対応する位置に `NULL` 要素を生成します。
- スケッチはストアごとに固定された最大容量 `max_bins = 2048`（累積時の `relative_accuracy` は `[0.001, 0.5]` の範囲でデフォルト `0.01`）を使用し、ビン中点の代表値が `value` 以下であるすべてのビンのカウントを合計します。
:::

## 構文

```Haskell
DOUBLE ddsketch_cdf(VARBINARY sketch, DOUBLE value)
ARRAY<DOUBLE> ddsketch_cdf(VARBINARY sketch, ARRAY<DOUBLE> values)
```

- `sketch`: シリアライズされた DDSketch を含む `VARBINARY` 値。
- `value` / `values`: $P(X \le \text{value})$ を評価する閾値の `DOUBLE` または `ARRAY<DOUBLE>`。

## 例

```SQL
-- SLO 達成率（100ms、200ms、500ms 以内のリクエストの割合）を計算
SELECT
    service,
    ddsketch_cdf(ddsketch_combine(latency_sketch), 200.0) AS slo_under_200ms,
    ddsketch_cdf(ddsketch_combine(latency_sketch), [100.0, 200.0, 500.0]) AS slo_tiers
FROM mv_service_latency
GROUP BY service;
```

## キーワード

DDSKETCH_CDF, DDSKETCH_QUANTILE, DDSKETCH_ACCUMULATE, DDSKETCH_COMBINE
