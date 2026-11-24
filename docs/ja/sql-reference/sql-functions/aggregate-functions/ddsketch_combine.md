---
displayed_sidebar: docs
description: "複数行のシリアライズされた Datadog DDSketch VARBINARY 値を決定論的にマージし、単一の結合スケッチとして返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ddsketch_combine

集計関数。行をまたいでシリアライズされた Datadog DDSketch ペイロード（`VARBINARY`）を決定論的にマージし、標準 `ddsketch.proto` 形式の単一の結合スケッチを `VARBINARY` として返します。

入力スケッチには、[`ddsketch_accumulate`](./ddsketch_accumulate.md)、[`ddsketch_accumulate_weighted`](./ddsketch_accumulate.md)、[`ddsketch_union`](../scalar-functions/ddsketch_union.md)、[`ddsketch_empty`](../scalar-functions/ddsketch_quantile.md) で生成されたもの、または Parquet や Iceberg から `ddsketch.proto` 形式の `VARBINARY` として外部から読み込まれたものを使用できます。

:::note
- マージされるすべてのスケッチは同一の `IndexMapping`（`[0.001, 0.5]` の範囲の `gamma` / `relative_accuracy`、デフォルト `0.01`、および `index_offset`）を共有している必要があります。異なる `relative_accuracy` パラメータで構築されたスケッチ（`ddsketch_empty(0.02)` で作成された異なる精度の空スケッチを含む）をマージすると、行の到着順序に関係なくエラーが返されます。
- 結合後のスケッチは、決定論的な最小ビン折りたたみにより正および負の各ストアで `max_bins = 2048` の固定最大容量を維持し、分散実行において 100% 順序に依存しない結果を保証します。
- `NULL` 値および 0 バイトの `VARBINARY` スライスは無視されます。グループ内のすべての入力が `NULL` または 0 バイトの場合は `NULL` を返します。
:::

## 構文

```Haskell
VARBINARY ddsketch_combine(VARBINARY sketch)
```

- `sketch`: シリアライズされた DDSketch を格納する `VARBINARY` 列。

## 例

```SQL
-- 分単位のスケッチをサービス単位のパーセンタイルおよび SLO 達成率にロールアップ
SELECT
    service,
    ddsketch_count(ddsketch_combine(latency_sketch)) AS total_requests,
    ddsketch_quantile(ddsketch_combine(latency_sketch), [0.50, 0.95, 0.99]) AS latency_quantiles,
    ddsketch_cdf(ddsketch_combine(latency_sketch), 200.0) AS slo_under_200ms_ratio
FROM mv_service_latency
GROUP BY service;
```

## キーワード

DDSKETCH_COMBINE, DDSKETCH_ACCUMULATE, DDSKETCH_UNION, DDSKETCH_QUANTILE, DDSKETCH_PERCENTILE
