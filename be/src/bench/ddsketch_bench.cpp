// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

#include "types/ddsketch.h"
#include "types/tdigest.h"

namespace starrocks {

static void BM_DDSketchAdd(benchmark::State& state) {
    const int64_t num_values = state.range(0);
    std::vector<double> values(static_cast<size_t>(num_values));
    for (int64_t i = 0; i < num_values; ++i) {
        values[static_cast<size_t>(i)] = static_cast<double>((i * 7919) % 100003) + 0.1;
    }

    for (auto _ : state) {
        DDSketch sketch(0.01);
        for (double v : values) {
            sketch.add(v);
        }
        benchmark::DoNotOptimize(sketch.quantile(0.5));
        benchmark::DoNotOptimize(sketch.quantile(0.99));
    }
    state.SetItemsProcessed(state.iterations() * num_values);
}

static void BM_DDSketchMergeSingleton(benchmark::State& state) {
    std::vector<DDSketch> inputs(1024);
    for (size_t i = 0; i < inputs.size(); ++i) {
        inputs[i].reinit(0.01);
        inputs[i].add(static_cast<double>((i * 7919) % 100003) + 0.1);
    }

    const int64_t num_merges = state.range(0);
    for (auto _ : state) {
        DDSketch sketch(0.01);
        for (int64_t i = 0; i < num_merges; ++i) {
            auto st = sketch.merge(inputs[static_cast<size_t>(i) % inputs.size()]);
            benchmark::DoNotOptimize(st);
        }
        benchmark::DoNotOptimize(sketch.quantile(0.5));
        benchmark::DoNotOptimize(sketch.quantile(0.9));
    }
    state.SetItemsProcessed(state.iterations() * num_merges);
}

static void BM_DDSketchSerializeRoundTrip(benchmark::State& state) {
    DDSketch sketch(0.01);
    for (int i = 0; i < 10000; ++i) {
        sketch.add(static_cast<double>((i * 7919) % 100003) + 0.1);
    }
    std::vector<uint8_t> buf(sketch.serialize_size());

    for (auto _ : state) {
        size_t n = sketch.serialize(buf.data());
        DDSketch restored;
        auto st = restored.deserialize(Slice(buf.data(), n));
        benchmark::DoNotOptimize(st);
        benchmark::DoNotOptimize(restored.quantile(0.99));
    }
    state.SetItemsProcessed(state.iterations());
}

static void BM_TDigestAddCompare(benchmark::State& state) {
    const int64_t num_values = state.range(0);
    std::vector<float> values(static_cast<size_t>(num_values));
    for (int64_t i = 0; i < num_values; ++i) {
        values[static_cast<size_t>(i)] = static_cast<float>((i * 7919) % 100003) + 0.1f;
    }

    for (auto _ : state) {
        TDigest digest(10000);
        for (float v : values) {
            digest.add(v);
        }
        benchmark::DoNotOptimize(digest.quantile(0.5));
        benchmark::DoNotOptimize(digest.quantile(0.99));
    }
    state.SetItemsProcessed(state.iterations() * num_values);
}

BENCHMARK(BM_DDSketchAdd)->Arg(10000)->Arg(100000)->Arg(1000000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_DDSketchMergeSingleton)->Arg(10000)->Arg(100000)->Arg(1000000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_DDSketchSerializeRoundTrip)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_TDigestAddCompare)->Arg(10000)->Arg(100000)->Arg(1000000)->Unit(benchmark::kMillisecond);

} // namespace starrocks

BENCHMARK_MAIN();
