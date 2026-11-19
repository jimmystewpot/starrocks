---
displayed_sidebar: docs
description: "シリアライズされた Datadog DDSketch VARBINARY から分位数、観測カウント、およびビン中点近似の要約統計量を抽出します。"
---

# ddsketch_quantile, ddsketch_min, ddsketch_max, ddsketch_count, ddsketch_sum, ddsketch_avg, ddsketch_empty

スカラー関数。空の Datadog DDSketch を構築するか、シリアライズされた DDSketch（標準 `ddsketch.proto` 形式の `VARBINARY`）から分位数、正確な観測カウント、および近似要約統計量を抽出します。

入力スケッチには、[`ddsketch_accumulate`](../aggregate-functions/ddsketch_accumulate.md)、[`ddsketch_combine`](../aggregate-functions/ddsketch_combine.md)、[`ddsketch_union`](./ddsketch_union.md)、ネストされた `MAP<VARCHAR, VARBINARY>` や `ARRAY<VARBINARY>` 列、または外部の Datadog DDSketch SDK によって書き込まれた Parquet/Iceberg ファイルを使用できます。

:::note
- `ddsketch_count` はスケッチに累積された正確な `BIGINT` 観測カウントを返します（空スケッチまたは 0 バイトの場合は `0`、`NULL` スケッチの場合は `NULL`）。
- `ddsketch_sum`、`ddsketch_avg`、`ddsketch_min`、および `ddsketch_max` は、正確な生の IEEE-754 浮動小数点数ではなく、スケッチの対数ビンから計算される**ビン中点近似値（bucket-midpoint approximations）**です。具体的には、`ddsketch_min(sketch)` は `ddsketch_quantile(sketch, 0.0)` と等価であり、`ddsketch_max(sketch)` は `ddsketch_quantile(sketch, 1.0)` と等価であり、`ddsketch_sum(sketch)` はすべてのビンにわたる `bin_midpoint(k) * count[k]` の合計を返します。固定された `max_bins = 2048` の上限により最小ビンの折りたたみが発生した場合、`ddsketch_min`、`ddsketch_sum`、および `ddsketch_avg` は折りたたまれた最小ビンの中点値を反映します。正確な `SUM`、`AVG`、`MIN`、または `MAX` が必要な場合は、スケッチと併せて標準の SQL 集計関数を使用してください。
- `ddsketch_empty([relative_accuracy])` は初期化された `IndexMapping`（`count = 0`）を持つ標準の 11 バイトのシリアライズされた空スケッチを返します。`relative_accuracy` のデフォルトは `0.01` で、範囲は `[0.001, 0.5]` です。
:::

## 構文

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

- `sketch`: シリアライズされた DDSketch を含む `VARBINARY` 値。
- `p`: 目標分位数（`[0.0, 1.0]` の `DOUBLE`）または目標分位数の配列（`ARRAY<DOUBLE>`）。スカラー関数 `ddsketch_quantile` では、`[0.0, 1.0]` の範囲外または `NULL` の分位数ターゲットに対して `NULL`（または返される `ARRAY<DOUBLE>` 内の `NULL` 要素）を返します。
- `relative_accuracy`: `ddsketch_empty` のオプションの `DOUBLE` パラメータ（範囲 `[0.001, 0.5]`、デフォルト `0.01`）。

## 例

```SQL
-- 保存されたスケッチ列から複数のパーセンタイルと要約統計量を取得
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

-- MAP<VARCHAR, VARBINARY> 列にネストされたスケッチから分位数を抽出
SELECT
    host,
    ddsketch_quantile(metric_sketches['rpc_ms'], 0.99) AS rpc_p99,
    ddsketch_quantile(metric_sketches['rpc_ms'], [0.50, 0.95, 0.99]) AS rpc_quantiles
FROM host_telemetry_sketches;
```

## キーワード

DDSKETCH_QUANTILE, DDSKETCH_MIN, DDSKETCH_MAX, DDSKETCH_COUNT, DDSKETCH_SUM, DDSKETCH_AVG, DDSKETCH_EMPTY
