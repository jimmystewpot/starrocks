---
displayed_sidebar: docs
description: "Datadog DDSketch を使用して、相対誤差保証付きの決定論的な近似パーセンタイルまたは重み付きパーセンタイルを計算します。"
---

# ddsketch_percentile, ddsketch_percentile_weighted

Datadog DDSketch を使用して、単一のクエリで `expr`（または整数の重み `weight` を伴う `expr`）に対する 1 つ以上の相対誤差保証付き決定論的近似パーセンタイルを計算します。`p` がスカラー `DOUBLE` の場合は `DOUBLE` を、`p` が `ARRAY<DOUBLE>` の場合は `ARRAY<DOUBLE>` を返します。

非同期マテリアライズドビューに同じ `relative_accuracy` の [`ddsketch_accumulate(expr [, relative_accuracy])`](./ddsketch_accumulate.md) が保存されている場合、StarRocks オプティマイザは `ddsketch_percentile(expr, p [, relative_accuracy])` クエリを自動的にマテリアライズドビューに対する [`ddsketch_quantile`](../scalar-functions/ddsketch_quantile.md)（ロールアップ時は [`ddsketch_combine`](./ddsketch_combine.md) との組み合わせ）に書き換えます。

:::note
- 正および負の各ストアあたりの最大ビン数（`max_bins`）は、決定論的な最小ビン折りたたみ（`CollapsingLowestDenseStore`）により内部で `2048` に固定されており、マルチフェーズ分散集計全体で順序に依存しないパーセンタイル推定を保証します。
- `relative_accuracy` は省略可能で、デフォルトは `0.01`（1% の相対誤差）です。指定する場合は、`[0.001, 0.5]` の範囲の `DOUBLE` 定数リテラルである必要があります。
- `p` は `[0.0, 1.0]` の範囲の非 NULL `DOUBLE` 定数、またはすべての要素が非 NULL かつ `[0.0, 1.0]` の範囲にある空でない定数配列 `ARRAY<DOUBLE>` である必要があります。
- 有効な有限観測値（`ddsketch_percentile_weighted` では `weight > 0`）が累積されなかった場合は `NULL` を返します。
:::

## 構文

```Haskell
DOUBLE ddsketch_percentile(DOUBLE expr, DOUBLE p [, DOUBLE relative_accuracy])
ARRAY<DOUBLE> ddsketch_percentile(DOUBLE expr, ARRAY<DOUBLE> p [, DOUBLE relative_accuracy])
DOUBLE ddsketch_percentile_weighted(DOUBLE expr, BIGINT weight, DOUBLE p [, DOUBLE relative_accuracy])
ARRAY<DOUBLE> ddsketch_percentile_weighted(DOUBLE expr, BIGINT weight, ARRAY<DOUBLE> p [, DOUBLE relative_accuracy])
```

- `expr`: パーセンタイルを計算する対象の数値式。
- `weight`: `ddsketch_percentile_weighted` における各観測値の整数頻度（`BIGINT`）。`weight <= 0` の行は無視されます。
- `p`: 目標分位数（`[0.0, 1.0]` の `DOUBLE`）または目標分位数の空でない定数配列（すべての要素が `[0.0, 1.0]` にある `ARRAY<DOUBLE>`）。
- `relative_accuracy`: `[0.001, 0.5]` の範囲のオプションの `DOUBLE` 定数。デフォルトは `0.01`。

## 例

```SQL
-- 1 回のスキャンで p99 とパーセンタイル配列 [p50, p95, p99] を計算
SELECT
    service,
    ddsketch_percentile(latency_ms, 0.99) AS p99_ms,
    ddsketch_percentile(latency_ms, [0.50, 0.95, 0.99], 0.01) AS latency_quantiles,
    ddsketch_percentile_weighted(latency_ms, sample_count, 0.99) AS weighted_p99_ms
FROM rpc_traces
GROUP BY service;
```

## キーワード

DDSKETCH_PERCENTILE, DDSKETCH_PERCENTILE_WEIGHTED, DDSKETCH_ACCUMULATE, DDSKETCH_QUANTILE, PERCENTILE_APPROX
