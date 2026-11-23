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

#pragma once

#include <cstdint>
#include <vector>

#include "base/string/slice.h"
#include "common/status.h"

namespace starrocks {

class CollapsingLowestDenseStore {
public:
    struct SparseBin {
        int32_t index;
        uint64_t count;
    };

    static constexpr uint32_t kDefaultMaxBins = 2048;
    static constexpr uint32_t kGrowthStep = 64;
    static constexpr uint32_t kMaxSparseBins = 16;

    CollapsingLowestDenseStore() = default;
    explicit CollapsingLowestDenseStore(uint32_t max_bins) : _max_bins(max_bins) {}

    void clear();
    void add(int32_t index, uint64_t count = 1);
    void merge(const CollapsingLowestDenseStore& other);

    bool is_empty() const { return _total_count == 0; }
    bool is_sparse() const { return !_sparse_bins.empty(); }
    uint64_t total_count() const { return _total_count; }
    int32_t offset() const { return _offset; }
    int32_t min_index() const { return !_sparse_bins.empty() ? _sparse_bins.front().index : _offset; }
    int32_t max_index() const {
        if (!_sparse_bins.empty()) {
            return _sparse_bins.back().index;
        }
        return _bins.empty()
                       ? _offset
                       : static_cast<int32_t>(static_cast<int64_t>(_offset) + static_cast<int64_t>(_bins.size()) - 1);
    }
    const std::vector<uint64_t>& bins() const { return _bins; }
    const std::vector<SparseBin>& sparse_bins() const { return _sparse_bins; }
    uint32_t max_bins() const { return _max_bins; }
    uint64_t mem_usage() const {
        return _bins.capacity() * sizeof(uint64_t) + _sparse_bins.capacity() * sizeof(SparseBin);
    }

    uint64_t prefix_count(int32_t max_inclusive_index) const;
    void init_from_packed_doubles(int32_t trimmed_offset, const uint8_t* trimmed_packed_doubles, size_t trimmed_len,
                                  uint64_t total_count, size_t nonzero_count);
    void merge_from_packed_doubles(int32_t trimmed_offset, const uint8_t* trimmed_packed_doubles, size_t trimmed_len,
                                   uint64_t total_count, size_t nonzero_count);

    // Canonical trim invariant: when _total_count > 0, front and back active bins have count > 0;
    // when _total_count == 0, _bins.empty() && _sparse_bins.empty() && _offset == 0.
    void trim();

private:
    void _promote_sparse_to_dense();

    int32_t _offset = 0;
    uint32_t _max_bins = kDefaultMaxBins;
    uint64_t _total_count = 0;
    std::vector<uint64_t> _bins;
    std::vector<SparseBin> _sparse_bins;
};

class DDSketch {
public:
    static constexpr double kDefaultRelativeAccuracy = 0.01;
    static constexpr double kMinRelativeAccuracy = 0.001;
    static constexpr double kMaxRelativeAccuracy = 0.5;
    static constexpr uint32_t kDefaultMaxBins = 2048;
    static constexpr uint32_t kMaxScaledBins = 32768;
    static constexpr uint64_t kMaxExactDoubleCount = 9007199254740992ULL; // 2^53
    static constexpr uint32_t kMaxWireBins = 65536;
    static constexpr int32_t kMinBinIndex = -1073741823;
    static constexpr int32_t kMaxBinIndex = 1073741823;

    static constexpr uint32_t kInterpolationNone = 0;
    static constexpr uint32_t kInterpolationCubic = 3;

    static uint32_t default_max_bins_for_accuracy(double relative_accuracy);

    // Default constructor leaves _mapping_initialized = false (adopts first merged initialized sketch,
    // or initializes with kDefaultRelativeAccuracy = 0.01 on first add() or serialize()).
    DDSketch();
    explicit DDSketch(double relative_accuracy, uint32_t max_bins = 0);

    static Status validate_relative_accuracy(double relative_accuracy);
    void reinit(double relative_accuracy, uint32_t max_bins = 0);
    void clear();

    void add(double value);
    void add_batch(const double* values, size_t count);
    Status add_weighted(double value, int64_t weight);
    void add(double value, double weight);
    Status merge(const DDSketch& other);
    Status merge_serialized(const Slice& src);

    double quantile(double q) const;
    void quantiles(const double* qs, size_t count, double* out) const;
    double cdf(double value) const;
    uint64_t total_count() const { return _negative_store.total_count() + _zero_count + _positive_store.total_count(); }
    double sum() const;
    double avg() const;
    double min_value() const { return quantile(0.0); }
    double max_value() const { return quantile(1.0); }
    bool is_empty() const { return total_count() == 0; }
    bool is_mapping_initialized() const { return _mapping_initialized; }
    double gamma() const { return _gamma; }
    double index_offset() const { return _index_offset; }
    uint32_t interpolation() const { return _interpolation; }
    double relative_accuracy() const { return _relative_accuracy; }

    int32_t key(double value) const;
    double bin_value(int32_t index) const;

    uint64_t mem_usage() const { return _positive_store.mem_usage() + _negative_store.mem_usage(); }
    uint64_t serialize_size() const;
    size_t serialize(uint8_t* writer) const;
    Status deserialize(const Slice& src);

private:
    void _init_mapping_from_gamma(double gamma, double index_offset, uint32_t interpolation = kInterpolationNone);
    void _ensure_default_mapping();
    int32_t _key_positive(double abs_val) const;
    double _mapping_log(double value) const;
    double _mapping_exp(double value) const;

    bool _mapping_initialized = false;
    uint32_t _interpolation = kInterpolationNone;
    double _gamma = (1.0 + kDefaultRelativeAccuracy) / (1.0 - kDefaultRelativeAccuracy);
    double _index_offset = 0.0;
    double _relative_accuracy = kDefaultRelativeAccuracy;
    double _multiplier = 0.0;
    double _bin_norm = 1.0 + kDefaultRelativeAccuracy;
    double _min_indexable_value = 0.0;
    double _max_indexable_value = 0.0;
    int32_t _min_safe_bin_index = kMinBinIndex;
    int32_t _max_safe_bin_index = kMaxBinIndex;
    uint64_t _zero_count = 0;
    CollapsingLowestDenseStore _positive_store;
    CollapsingLowestDenseStore _negative_store;
};

} // namespace starrocks
