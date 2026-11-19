---
displayed_sidebar: docs
description: "数値列または重み付き数値から Datadog DDSketch を構築し、シリアライズされた protobuf スケッチを VARBINARY として返します。"
products: [oss, enterprise, cloud, anywhere]
---

# ddsketch_accumulate, ddsketch_accumulate_weighted

数値式 `expr`（または整数の重み `weight` を伴う `expr`）から決定論的な Datadog DDSketch を構築し、標準 `ddsketch.proto` 形式でシリアライズされたスケッチを `VARBINARY` として返します。[ddsketch_combine](./ddsketch_combine.md)、[ddsketch_quantile](../scalar-functions/ddsketch_quantile.md)、および [ddsketch_cdf](../scalar-functions/ddsketch_cdf.md) と組み合わせることで、テーブルや非同期マテリアライズドビュー内でスケッチの永続化、ロールアップ、クエリを行えます。

シリアライズされた `VARBINARY` ペイロードは Datadog の標準 `ddsketch.proto` ワイヤフォーマット（`Interpolation::NONE` の対数 `IndexMapping` と連続した稠密ビンカウント）を使用しており、`ARRAY<VARBINARY>`、`MAP<VARCHAR, VARBINARY>`、`STRUCT` へのネストや、外部の Datadog DDSketch SDK との相互運用が可能です。

:::note
- 正および負の各ストアあたりの最大ビン数（`max_bins`）は、決定論的な最小ビン折りたたみ（`CollapsingLowestDenseStore`）により内部で `2048` に固定されています。
- `relative_accuracy` は省略可能で、デフォルトは `0.01`（1% の相対誤差）です。指定する場合は、`[0.001, 0.5]` の範囲の `DOUBLE` 定数リテラルである必要があります。
- `expr` が `NULL` または非有限値（`NaN`、`±Inf`）の行は無視されます。`ddsketch_accumulate_weighted` では、`weight <= 0` または `weight` が `NULL` の行は無視され、個々の重みまたはスケッチの合計カウントが `2^53`（`9007199254740992`）を超えるとエラーを返します。
- グループ内に有効な観測値が 1 つも累積されなかった場合は `NULL` を返します。
:::

## 構文

```Haskell
VARBINARY ddsketch_accumulate(DOUBLE expr [, DOUBLE relative_accuracy])
VARBINARY ddsketch_accumulate_weighted(DOUBLE expr, BIGINT weight [, DOUBLE relative_accuracy])
```

- `expr`: 分布を要約する対象の数値式。
- `weight`: `ddsketch_accumulate_weighted` における各観測値の整数頻度（`BIGINT`）。`weight <= 0` の行は無視されます。
- `relative_accuracy`: 目標相対精度 $\alpha$ を指定する `[0.001, 0.5]` の範囲のオプションの `DOUBLE` 定数。デフォルトは `0.01`。

## 例

```SQL
-- テーブルまたは非同期マテリアライズドビューにサービスごとのスケッチを永続化
CREATE TABLE service_latency_sketches AS
SELECT
    service,
    ddsketch_accumulate(latency_ms) AS latency_sketch,
    ddsketch_accumulate_weighted(latency_ms, sample_count, 0.01) AS weighted_sketch
FROM rpc_traces
GROUP BY service;

-- 保存されたスケッチから複数の分位数とカウントを取得
SELECT
    service,
    ddsketch_count(latency_sketch) AS total_requests,
    ddsketch_quantile(latency_sketch, 0.50) AS p50_ms,
    ddsketch_quantile(latency_sketch, 0.99) AS p99_ms
FROM service_latency_sketches;
```

## キーワード

DDSKETCH_ACCUMULATE, DDSKETCH_ACCUMULATE_WEIGHTED, DDSKETCH_COMBINE, DDSKETCH_QUANTILE, DDSKETCH_PERCENTILE
