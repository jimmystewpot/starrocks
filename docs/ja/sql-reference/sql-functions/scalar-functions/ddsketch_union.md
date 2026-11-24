---
displayed_sidebar: docs
description: "2 つのシリアライズされた Datadog DDSketch VARBINARY 値をペアごとにマージし、単一の結合スケッチを返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ddsketch_union

2 つのシリアライズされた Datadog DDSketch ペイロード（`VARBINARY`）に対するスカラーのペアごと和集合演算です。`sketch_a` と `sketch_b` を決定論的にマージし、標準 `ddsketch.proto` 形式の単一のシリアライズされたスケッチを `VARBINARY` として返します。

テーブルまたはグループ内の複数行にわたる集計マージには、[`ddsketch_combine`](../aggregate-functions/ddsketch_combine.md) を使用してください。

:::note
- 両方の入力スケッチは互換性のある `IndexMapping` パラメータ（`[0.001, 0.5]` の範囲の `gamma` / `relative_accuracy`、デフォルト `0.01`、および `index_offset`）を持っている必要があります。異なる `relative_accuracy` を持つ 2 つのスケッチ（`ddsketch_empty(0.02)` で作成された異なる精度の空スケッチを含む）をマージすると、引数の順序に関係なくエラーが返されます。
- マージされたスケッチは、決定論的な最小ビン折りたたみにより、正および負の各ストアで `max_bins = 2048` の固定最大容量を維持します。
- `sketch_a` または `sketch_b` のいずれかが `NULL` の場合は `NULL` を返します。
:::

## 構文

```Haskell
VARBINARY ddsketch_union(VARBINARY sketch_a, VARBINARY sketch_b)
```

- `sketch_a`、`sketch_b`: シリアライズされた DDSketch を含む `VARBINARY` 値。

## 例

```SQL
-- 同一行の 2 つの列にある読み取りレイテンシスケッチと書き込みレイテンシスケッチをマージ
SELECT
    host,
    ddsketch_quantile(ddsketch_union(read_latency_sk, write_latency_sk), 0.99) AS combined_p99_ms
FROM storage_node_sketches;
```

## キーワード

DDSKETCH_UNION, DDSKETCH_COMBINE, DDSKETCH_ACCUMULATE, DDSKETCH_QUANTILE
