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

#include "types/ddsketch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace starrocks {

namespace {

void append_varint(std::string* out, uint64_t v) {
    while (v >= 0x80) {
        out->push_back(static_cast<char>((v & 0x7F) | 0x80));
        v >>= 7;
    }
    out->push_back(static_cast<char>(v));
}

void append_sint32_zigzag(std::string* out, int32_t v) {
    uint32_t zz = (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
    append_varint(out, zz);
}

void append_tag(std::string* out, uint32_t field_number, uint32_t wire_type) {
    append_varint(out, (static_cast<uint64_t>(field_number) << 3) | wire_type);
}

void append_double_le(std::string* out, double d) {
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(double));
    for (int i = 0; i < 8; ++i) {
        out->push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));
    }
}

void append_length_delimited(std::string* out, uint32_t field_number, const std::string& payload) {
    append_tag(out, field_number, 2);
    append_varint(out, payload.size());
    out->append(payload);
}

std::string make_index_mapping_msg(double gamma, double index_offset = 0.0, uint32_t interpolation = 0) {
    std::string m;
    append_tag(&m, 1, 1);
    append_double_le(&m, gamma);
    if (index_offset != 0.0 || std::isnan(index_offset)) {
        append_tag(&m, 2, 1);
        append_double_le(&m, index_offset);
    }
    if (interpolation != 0) {
        append_tag(&m, 3, 0);
        append_varint(&m, interpolation);
    }
    return m;
}

std::string serialize_sketch(const DDSketch& sketch) {
    std::string out(sketch.serialize_size(), '\0');
    size_t written = sketch.serialize(reinterpret_cast<uint8_t*>(out.data()));
    EXPECT_EQ(out.size(), written);
    return out;
}

} // namespace

class DDSketchTest : public ::testing::Test {};

TEST_F(DDSketchTest, TestRelativeAccuracyValidation) {
    EXPECT_TRUE(DDSketch::validate_relative_accuracy(0.001).ok());
    EXPECT_TRUE(DDSketch::validate_relative_accuracy(0.01).ok());
    EXPECT_TRUE(DDSketch::validate_relative_accuracy(0.05).ok());
    EXPECT_TRUE(DDSketch::validate_relative_accuracy(0.5).ok());

    for (double bad : {0.0, -0.01, 0.000999999, 0.5000001, 1.0, std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        Status st = DDSketch::validate_relative_accuracy(bad);
        EXPECT_TRUE(st.is_invalid_argument()) << "Expected invalid argument for " << bad;
        EXPECT_NE(st.message().find("ddsketch relative_accuracy must be a constant in [0.001, 0.5]"),
                  std::string::npos);
    }
}

TEST_F(DDSketchTest, TestCollapsingLowestDenseStore) {
    CollapsingLowestDenseStore store;
    EXPECT_TRUE(store.is_empty());
    EXPECT_EQ(0ULL, store.total_count());
    EXPECT_EQ(0, store.offset());
    EXPECT_TRUE(store.bins().empty());
    // Lazy allocation invariant: empty store uses 0 heap bytes.
    EXPECT_EQ(0ULL, store.mem_usage());

    // Single-bin store allocates 1 bin (8 bytes).
    store.add(100, 5);
    EXPECT_FALSE(store.is_empty());
    EXPECT_EQ(5ULL, store.total_count());
    EXPECT_EQ(100, store.offset());
    ASSERT_EQ(1U, store.bins().size());
    EXPECT_EQ(5ULL, store.bins()[0]);
    EXPECT_EQ(sizeof(uint64_t), store.mem_usage());

    // Expanding within kGrowthStep = 64 reserves in multiples of 64.
    store.add(110, 3);
    EXPECT_EQ(8ULL, store.total_count());
    EXPECT_EQ(100, store.offset());
    ASSERT_EQ(11U, store.bins().size());
    EXPECT_EQ(5ULL, store.bins().front());
    EXPECT_EQ(3ULL, store.bins().back());
    EXPECT_EQ(64ULL * sizeof(uint64_t), store.mem_usage());

    // Test collapsing lowest bins with a small max_bins = 4.
    CollapsingLowestDenseStore small_store(4);
    small_store.add(10, 1);
    small_store.add(11, 2);
    small_store.add(12, 3);
    small_store.add(13, 4);
    EXPECT_EQ(10, small_store.offset());
    ASSERT_EQ(4U, small_store.bins().size());

    // Adding index 15 pushes floor to 15 - 4 + 1 = 12. Indices 10, 11, 12 collapse into bin[0] at offset 12.
    small_store.add(15, 7);
    EXPECT_EQ(12, small_store.offset());
    ASSERT_EQ(4U, small_store.bins().size());
    EXPECT_EQ(1ULL + 2ULL + 3ULL, small_store.bins()[0]); // index 12 holds collapsed 10..12
    EXPECT_EQ(4ULL, small_store.bins()[1]);               // index 13
    EXPECT_EQ(0ULL, small_store.bins()[2]);               // index 14
    EXPECT_EQ(7ULL, small_store.bins()[3]);               // index 15
    EXPECT_EQ(17ULL, small_store.total_count());

    // Adding index 5 (< current offset 12 when already at max_bins = 4) collapses directly into bins[0].
    small_store.add(5, 9);
    EXPECT_EQ(12, small_store.offset());
    ASSERT_EQ(4U, small_store.bins().size());
    EXPECT_EQ(15ULL, small_store.bins()[0]);
    EXPECT_EQ(26ULL, small_store.total_count());

    // Test int64_t span math across extreme negative and positive indices without int32_t overflow.
    CollapsingLowestDenseStore extreme_a;
    extreme_a.add(DDSketch::kMinBinIndex, 10);
    CollapsingLowestDenseStore extreme_b;
    extreme_b.add(DDSketch::kMaxBinIndex, 20);
    extreme_a.merge(extreme_b);
    EXPECT_EQ(30ULL, extreme_a.total_count());
    EXPECT_EQ(DDSketch::kMaxBinIndex - static_cast<int32_t>(CollapsingLowestDenseStore::kDefaultMaxBins) + 1,
              extreme_a.offset());
    ASSERT_EQ(CollapsingLowestDenseStore::kDefaultMaxBins, extreme_a.bins().size());
    EXPECT_EQ(10ULL, extreme_a.bins().front());
    EXPECT_EQ(20ULL, extreme_a.bins().back());

    // Clear releases capacity.
    extreme_a.clear();
    EXPECT_TRUE(extreme_a.is_empty());
    EXPECT_EQ(0, extreme_a.offset());
    EXPECT_EQ(0ULL, extreme_a.mem_usage());
}

TEST_F(DDSketchTest, TestOrderIndependenceAndDeterminism) {
    // Generate dataset combining uniform, log-normal, pareto (spanning > 2048 bins to trigger collapsing),
    // negative values, zeros, and subnormals.
    std::mt19937_64 rng(20261117ULL);
    std::uniform_real_distribution<double> uniform_dist(-500.0, 500.0);
    std::lognormal_distribution<double> lognorm_dist(0.0, 4.0);

    std::vector<double> dataset;
    dataset.reserve(4000);
    for (int i = 0; i < 1500; ++i) {
        dataset.push_back(uniform_dist(rng));
    }
    for (int i = 0; i < 1500; ++i) {
        dataset.push_back(lognorm_dist(rng));
    }
    // Wide dynamic range values spanning 1e-40 to 1e+40 (well beyond 2048 bins at alpha = 0.01)
    for (int exp = -40; exp <= 40; ++exp) {
        double v = std::pow(10.0, exp);
        dataset.push_back(v);
        dataset.push_back(-v);
    }
    // Zeros, -0.0, and subnormal values
    for (int i = 0; i < 50; ++i) {
        dataset.push_back(0.0);
        dataset.push_back(-0.0);
        dataset.push_back(std::numeric_limits<double>::denorm_min());
        dataset.push_back(-std::numeric_limits<double>::denorm_min());
    }

    DDSketch reference_sketch(0.01);
    for (double v : dataset) {
        reference_sketch.add(v);
    }
    const std::string reference_bytes = serialize_sketch(reference_sketch);
    ASSERT_FALSE(reference_bytes.empty());

    const std::vector<double> test_quantiles = {0.0, 0.01, 0.1, 0.25, 0.5, 0.75, 0.9, 0.99, 0.999, 1.0};
    const std::vector<double> test_cdf_points = {-1e30, -100.0, -1.0, 0.0, 1.0, 100.0, 1e30};

    // 20 random permutations and random binary merge trees
    for (int trial = 0; trial < 20; ++trial) {
        std::vector<double> shuffled = dataset;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);

        // 1. Sequential insertion in shuffled order
        DDSketch seq_sketch(0.01);
        for (double v : shuffled) {
            seq_sketch.add(v);
        }
        EXPECT_EQ(reference_bytes, serialize_sketch(seq_sketch)) << "Sequential shuffle mismatch at trial " << trial;

        // 2. Random partition and random binary merge tree
        std::vector<DDSketch> leaves;
        size_t idx = 0;
        std::uniform_int_distribution<size_t> chunk_dist(1, 137);
        while (idx < shuffled.size()) {
            size_t chunk_len = std::min(chunk_dist(rng), shuffled.size() - idx);
            DDSketch leaf(0.01);
            for (size_t j = 0; j < chunk_len; ++j) {
                leaf.add(shuffled[idx + j]);
            }
            leaves.push_back(std::move(leaf));
            idx += chunk_len;
        }
        // Also inject an untouched default DDSketch() and an explicit empty DDSketch(0.01) into the merge tree
        leaves.emplace_back();
        leaves.emplace_back(0.01);

        while (leaves.size() > 1) {
            std::uniform_int_distribution<size_t> pick_a(0, leaves.size() - 1);
            size_t a = pick_a(rng);
            std::uniform_int_distribution<size_t> pick_b(0, leaves.size() - 2);
            size_t b = pick_b(rng);
            if (b >= a) {
                ++b;
            }
            ASSERT_TRUE(leaves[a].merge(leaves[b]).ok());
            leaves.erase(leaves.begin() + static_cast<std::ptrdiff_t>(b));
        }

        const DDSketch& tree_sketch = leaves[0];
        EXPECT_EQ(reference_bytes, serialize_sketch(tree_sketch)) << "Merge tree mismatch at trial " << trial;
        EXPECT_EQ(reference_sketch.total_count(), tree_sketch.total_count());
        EXPECT_DOUBLE_EQ(reference_sketch.sum(), tree_sketch.sum());
        EXPECT_DOUBLE_EQ(reference_sketch.avg(), tree_sketch.avg());
        for (double q : test_quantiles) {
            EXPECT_DOUBLE_EQ(reference_sketch.quantile(q), tree_sketch.quantile(q));
        }
        for (double x : test_cdf_points) {
            EXPECT_DOUBLE_EQ(reference_sketch.cdf(x), tree_sketch.cdf(x));
        }
    }
}

TEST_F(DDSketchTest, TestNegativeZeroAndNonFiniteValues) {
    DDSketch sketch(0.01);
    EXPECT_TRUE(sketch.is_empty());
    EXPECT_TRUE(std::isnan(sketch.quantile(0.5)));
    EXPECT_TRUE(std::isnan(sketch.cdf(0.0)));
    EXPECT_TRUE(std::isnan(sketch.sum()));
    EXPECT_TRUE(std::isnan(sketch.avg()));

    // Non-finite values are ignored
    sketch.add(std::numeric_limits<double>::quiet_NaN());
    sketch.add(std::numeric_limits<double>::infinity());
    sketch.add(-std::numeric_limits<double>::infinity());
    EXPECT_TRUE(sketch.add_weighted(std::numeric_limits<double>::quiet_NaN(), 5).ok());
    EXPECT_TRUE(sketch.add_weighted(std::numeric_limits<double>::infinity(), 5).ok());
    EXPECT_TRUE(sketch.is_empty());

    // Negative zero and subnormals increment zero_count and return +0.0 (no negative signbit)
    sketch.add(-0.0);
    sketch.add(0.0);
    sketch.add(std::numeric_limits<double>::denorm_min());
    sketch.add(-std::numeric_limits<double>::denorm_min());
    EXPECT_EQ(4ULL, sketch.total_count());

    double q50 = sketch.quantile(0.5);
    EXPECT_DOUBLE_EQ(0.0, q50);
    EXPECT_FALSE(std::signbit(q50));
    EXPECT_DOUBLE_EQ(1.0, sketch.cdf(0.0));
    EXPECT_DOUBLE_EQ(0.0, sketch.cdf(-1.0));
    EXPECT_DOUBLE_EQ(0.0, sketch.cdf(-std::numeric_limits<double>::infinity()));
    EXPECT_DOUBLE_EQ(1.0, sketch.cdf(std::numeric_limits<double>::infinity()));
    EXPECT_TRUE(std::isnan(sketch.cdf(std::numeric_limits<double>::quiet_NaN())));
    EXPECT_TRUE(std::isnan(sketch.quantile(-0.01)));
    EXPECT_TRUE(std::isnan(sketch.quantile(1.01)));
    EXPECT_TRUE(std::isnan(sketch.quantile(std::numeric_limits<double>::quiet_NaN())));

    // Weighted insertions: weight <= 0 is ignored; weight > 2^53 fails
    EXPECT_TRUE(sketch.add_weighted(10.0, 0).ok());
    EXPECT_TRUE(sketch.add_weighted(10.0, -5).ok());
    EXPECT_EQ(4ULL, sketch.total_count());

    Status st_weight = sketch.add_weighted(10.0, static_cast<int64_t>(DDSketch::kMaxExactDoubleCount + 1));
    EXPECT_TRUE(st_weight.is_invalid_argument());
    EXPECT_NE(st_weight.message().find("ddsketch weight exceeds 2^53"), std::string::npos);

    // Extreme values near DBL_MAX are clamped to _max_safe_bin_index with finite bin_value
    sketch.add(std::numeric_limits<double>::max());
    sketch.add(-std::numeric_limits<double>::max());
    EXPECT_EQ(6ULL, sketch.total_count());
    EXPECT_TRUE(std::isfinite(sketch.quantile(0.0)));
    EXPECT_TRUE(std::isfinite(sketch.quantile(1.0)));
    EXPECT_DOUBLE_EQ(sketch.min_value(), sketch.quantile(0.0));
    EXPECT_DOUBLE_EQ(sketch.max_value(), sketch.quantile(1.0));
    EXPECT_LT(sketch.min_value(), 0.0);
    EXPECT_GT(sketch.max_value(), 0.0);
}

TEST_F(DDSketchTest, TestProtobufWireCompatibility) {
    // 1. Default empty sketch serializes canonical 11-byte IndexMapping (0x0a 0x09 0x09 <8-byte LE gamma>)
    {
        DDSketch empty_sketch;
        EXPECT_FALSE(empty_sketch.is_mapping_initialized());
        std::string bytes = serialize_sketch(empty_sketch);
        ASSERT_EQ(11U, bytes.size());
        EXPECT_EQ('\x0a', bytes[0]);
        EXPECT_EQ('\x09', bytes[1]);
        EXPECT_EQ('\x09', bytes[2]);

        DDSketch restored;
        ASSERT_TRUE(restored.deserialize(Slice(bytes)).ok());
        EXPECT_TRUE(restored.is_mapping_initialized());
        EXPECT_TRUE(restored.is_empty());
        EXPECT_NEAR(0.01, restored.relative_accuracy(), 1e-12);
    }

    // 2. 0-byte Slice resets sketch to uninitialized empty state
    {
        DDSketch sketch(0.01);
        sketch.add(42.0);
        ASSERT_TRUE(sketch.deserialize(Slice()).ok());
        EXPECT_FALSE(sketch.is_mapping_initialized());
        EXPECT_TRUE(sketch.is_empty());
    }

    // 3. Single-value sketch, positive + negative + zero counts, boundary quantiles 0.0 and 1.0
    {
        DDSketch sketch(0.01);
        sketch.add(100.0);
        EXPECT_NEAR(100.0, sketch.quantile(0.0), 100.0 * 0.01);
        EXPECT_NEAR(100.0, sketch.quantile(0.5), 100.0 * 0.01);
        EXPECT_NEAR(100.0, sketch.quantile(1.0), 100.0 * 0.01);

        sketch.add(-50.0);
        sketch.add(0.0);
        std::string bytes = serialize_sketch(sketch);

        DDSketch restored;
        ASSERT_TRUE(restored.deserialize(Slice(bytes)).ok());
        EXPECT_EQ(3ULL, restored.total_count());
        EXPECT_NEAR(-50.0, restored.min_value(), 50.0 * 0.01);
        EXPECT_DOUBLE_EQ(0.0, restored.quantile(0.5));
        EXPECT_NEAR(100.0, restored.max_value(), 100.0 * 0.01);
        EXPECT_EQ(bytes, serialize_sketch(restored));
    }

    // 4. Sparse bin_counts (field 1, tag 0x0a), unpacked contiguous_bin_counts (field 2, tag 0x11),
    // and non-zero index_offset in IndexMapping
    {
        const double gamma = (1.0 + 0.01) / (1.0 - 0.01);
        const double index_offset = 12.5;
        std::string mapping_msg = make_index_mapping_msg(gamma, index_offset);

        // Build positive Store with sparse map entries (bin 10 -> 3.0, bin 12 -> 5.0)
        // and unpacked contiguous_bin_counts at offset -2 (bin -2 -> 1.0, bin -1 -> 2.0)
        std::string pos_store;
        for (const auto& kv : std::vector<std::pair<int32_t, double>>{{10, 3.0}, {12, 5.0}}) {
            std::string entry;
            append_tag(&entry, 1, 0);
            append_sint32_zigzag(&entry, kv.first);
            append_tag(&entry, 2, 1);
            append_double_le(&entry, kv.second);
            append_length_delimited(&pos_store, 1, entry);
        }
        append_tag(&pos_store, 2, 1);
        append_double_le(&pos_store, 1.0);
        append_tag(&pos_store, 2, 1);
        append_double_le(&pos_store, 2.0);
        append_tag(&pos_store, 3, 0);
        append_sint32_zigzag(&pos_store, -2);

        std::string proto;
        append_length_delimited(&proto, 1, mapping_msg);
        append_length_delimited(&proto, 2, pos_store);
        append_tag(&proto, 4, 1);
        append_double_le(&proto, 4.0); // zero_count = 4

        DDSketch sketch;
        ASSERT_TRUE(sketch.deserialize(Slice(proto)).ok());
        EXPECT_TRUE(sketch.is_mapping_initialized());
        EXPECT_DOUBLE_EQ(gamma, sketch.gamma());
        EXPECT_DOUBLE_EQ(index_offset, sketch.index_offset());
        EXPECT_EQ(3ULL + 5ULL + 1ULL + 2ULL + 4ULL, sketch.total_count());

        // Round-trip re-serialization should preserve all counts and index_offset
        std::string reserialized = serialize_sketch(sketch);
        DDSketch roundtrip;
        ASSERT_TRUE(roundtrip.deserialize(Slice(reserialized)).ok());
        EXPECT_EQ(sketch.total_count(), roundtrip.total_count());
        EXPECT_DOUBLE_EQ(sketch.index_offset(), roundtrip.index_offset());
        EXPECT_DOUBLE_EQ(sketch.quantile(0.5), roundtrip.quantile(0.5));
    }
}

TEST_F(DDSketchTest, TestMergeMappingCompatibility) {
    // 1. Untouched default DDSketch() adopts first merged initialized sketch
    {
        DDSketch untouched;
        DDSketch init_002(0.02);
        init_002.add(10.0);
        ASSERT_TRUE(untouched.merge(init_002).ok());
        EXPECT_TRUE(untouched.is_mapping_initialized());
        EXPECT_NEAR(0.02, untouched.relative_accuracy(), 1e-12);
        EXPECT_EQ(1ULL, untouched.total_count());

        // Merging an untouched default sketch into an initialized sketch is a no-op
        DDSketch another_untouched;
        ASSERT_TRUE(untouched.merge(another_untouched).ok());
        EXPECT_EQ(1ULL, untouched.total_count());
    }

    // 2. Both (empty_002, non_empty_001) and (non_empty_001, empty_002) MUST fail identically!
    {
        DDSketch empty_002(0.02);
        DDSketch non_empty_001(0.01);
        non_empty_001.add(42.0);

        DDSketch target_a = empty_002;
        Status st_ab = target_a.merge(non_empty_001);
        EXPECT_TRUE(st_ab.is_invalid_argument());
        EXPECT_NE(st_ab.message().find("ddsketch_combine: cannot merge sketches with incompatible IndexMapping"),
                  std::string::npos);

        DDSketch target_b = non_empty_001;
        Status st_ba = target_b.merge(empty_002);
        EXPECT_TRUE(st_ba.is_invalid_argument());
        EXPECT_NE(st_ba.message().find("ddsketch_combine: cannot merge sketches with incompatible IndexMapping"),
                  std::string::npos);
    }

    // 3. Two empty initialized sketches with different relative_accuracy also fail
    {
        DDSketch empty_002(0.02);
        DDSketch empty_001(0.01);
        Status st = empty_002.merge(empty_001);
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_NE(st.message().find("ddsketch_combine: cannot merge sketches with incompatible IndexMapping"),
                  std::string::npos);
    }
}

TEST_F(DDSketchTest, TestMalformedAndAdversarialPayloads) {
    const double valid_gamma = (1.0 + 0.01) / (1.0 - 0.01);
    const std::string valid_mapping = make_index_mapping_msg(valid_gamma);

    // Class 1: Truncated or overlong (>10-byte) varint
    {
        std::string bad_varint(11, '\x80');
        DDSketch sketch;
        Status st = sketch.deserialize(Slice(bad_varint));
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_NE(st.message().find("ddsketch: malformed varint"), std::string::npos);
    }

    // Class 2: Length-delimited field overrunning buffer
    {
        std::string overrun;
        append_tag(&overrun, 1, 2);
        append_varint(&overrun, 50); // claims 50 bytes, only 4 follow
        overrun.append("abcd");
        DDSketch sketch;
        Status st = sketch.deserialize(Slice(overrun));
        EXPECT_TRUE(st.is_invalid_argument());
    }

    // Class 3: Odd-length packed contiguous_bin_counts (not a multiple of 8)
    {
        std::string bad_store;
        append_tag(&bad_store, 2, 2);
        append_varint(&bad_store, 7); // 7 bytes is not divisible by 8
        bad_store.append(7, '\0');

        std::string proto;
        append_length_delimited(&proto, 1, valid_mapping);
        append_length_delimited(&proto, 2, bad_store);

        DDSketch sketch;
        Status st = sketch.deserialize(Slice(proto));
        EXPECT_TRUE(st.is_invalid_argument());
    }

    // Class 4: Store exceeding kMaxWireBins = 65536
    {
        std::string huge_store;
        append_tag(&huge_store, 2, 2);
        size_t num_bins = DDSketch::kMaxWireBins + 1;
        append_varint(&huge_store, num_bins * 8);
        huge_store.append(num_bins * 8, '\0');

        std::string proto;
        append_length_delimited(&proto, 1, valid_mapping);
        append_length_delimited(&proto, 2, huge_store);

        DDSketch sketch;
        Status st = sketch.deserialize(Slice(proto));
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_NE(st.message().find("ddsketch: wire store exceeds maximum allowed bin entries (65536)"),
                  std::string::npos);
    }

    // Class 5: Duplicate sparse map keys and repeated Store submessages
    {
        std::string store1;
        for (int i = 0; i < 2; ++i) {
            std::string entry;
            append_tag(&entry, 1, 0);
            append_sint32_zigzag(&entry, 5);
            append_tag(&entry, 2, 1);
            append_double_le(&entry, 3.0);
            append_length_delimited(&store1, 1, entry);
        }
        std::string store2;
        {
            std::string entry;
            append_tag(&entry, 1, 0);
            append_sint32_zigzag(&entry, 5);
            append_tag(&entry, 2, 1);
            append_double_le(&entry, 4.0);
            append_length_delimited(&store2, 1, entry);
        }
        std::string proto;
        append_length_delimited(&proto, 1, valid_mapping);
        append_length_delimited(&proto, 2, store1);
        append_length_delimited(&proto, 2, store2);

        DDSketch sketch;
        ASSERT_TRUE(sketch.deserialize(Slice(proto)).ok());
        EXPECT_EQ(10ULL, sketch.total_count());
    }

    // Class 6: Invalid protobuf wire types (3, 4, 6, 7)
    for (uint32_t bad_wt : {3U, 4U, 6U, 7U}) {
        std::string proto;
        append_length_delimited(&proto, 1, valid_mapping);
        append_tag(&proto, 99, bad_wt);
        DDSketch sketch;
        Status st = sketch.deserialize(Slice(proto));
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_NE(st.message().find("ddsketch: invalid protobuf wire type"), std::string::npos);
    }

    // Class 7: Invalid bin counts / zero_count (NaN, Inf, negative, fractional, > 2^53)
    for (double bad_count : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.0,
                             1.5, 9007199254740994.0}) {
        std::string proto;
        append_length_delimited(&proto, 1, valid_mapping);
        append_tag(&proto, 4, 1);
        append_double_le(&proto, bad_count);

        DDSketch sketch;
        Status st = sketch.deserialize(Slice(proto));
        EXPECT_TRUE(st.is_invalid_argument()) << "Expected invalid argument for count=" << bad_count;
        EXPECT_NE(st.message().find("ddsketch: bin count must be an exact non-negative integer <= 2^53"),
                  std::string::npos);
    }

    // Class 8: Invalid IndexMapping (missing, bad interpolation, bad gamma, bad index_offset, bin index overflow)
    {
        // Missing IndexMapping
        std::string no_mapping;
        append_tag(&no_mapping, 4, 1);
        append_double_le(&no_mapping, 1.0);
        DDSketch sketch;
        Status st = sketch.deserialize(Slice(no_mapping));
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_NE(st.message().find("ddsketch: missing IndexMapping"), std::string::npos);
    }
    {
        // Unsupported interpolation (e.g. LINEAR = 1, QUADRATIC = 2)
        std::string proto;
        append_length_delimited(&proto, 1, make_index_mapping_msg(valid_gamma, 0.0, 1));
        DDSketch sketch;
        Status st = sketch.deserialize(Slice(proto));
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_NE(st.message().find("ddsketch: unsupported IndexMapping interpolation mode; only NONE (logarithmic) "
                                    "and CUBIC are supported"),
                  std::string::npos);
    }
    for (double bad_gamma :
         {0.5, 1.0, 1.0001, 3.5, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        std::string proto;
        append_length_delimited(&proto, 1, make_index_mapping_msg(bad_gamma));
        DDSketch sketch;
        EXPECT_TRUE(sketch.deserialize(Slice(proto)).is_invalid_argument());
    }
    for (double bad_offset : {1e7, -1e7, std::numeric_limits<double>::quiet_NaN()}) {
        std::string proto;
        append_length_delimited(&proto, 1, make_index_mapping_msg(valid_gamma, bad_offset));
        DDSketch sketch;
        EXPECT_TRUE(sketch.deserialize(Slice(proto)).is_invalid_argument());
    }
    {
        // Bin index out of range [kMinBinIndex, kMaxBinIndex]
        std::string bad_store;
        append_tag(&bad_store, 2, 1);
        append_double_le(&bad_store, 1.0);
        append_tag(&bad_store, 3, 0);
        append_sint32_zigzag(&bad_store, DDSketch::kMaxBinIndex + 1);

        std::string proto;
        append_length_delimited(&proto, 1, valid_mapping);
        append_length_delimited(&proto, 2, bad_store);

        DDSketch sketch;
        Status st = sketch.deserialize(Slice(proto));
        EXPECT_TRUE(st.is_invalid_argument());
        EXPECT_NE(st.message().find("ddsketch: bin index out of range"), std::string::npos);
    }
}

TEST_F(DDSketchTest, TestSparseModeAndOptimizations) {
    // 1. Sparse mode for widely separated bins (span > 64, <= 2048, distinct bins <= 16)
    {
        CollapsingLowestDenseStore store;
        store.add(0, 10);
        EXPECT_FALSE(store.is_sparse());
        store.add(200, 20); // span = 201 > 64 -> transitions to sparse buffer (capacity 4 -> 64 bytes)
        EXPECT_TRUE(store.is_sparse());
        EXPECT_EQ(2U, store.sparse_bins().size());
        EXPECT_EQ(0, store.min_index());
        EXPECT_EQ(200, store.max_index());
        EXPECT_EQ(30ULL, store.total_count());
        EXPECT_LE(store.mem_usage(), 4ULL * sizeof(CollapsingLowestDenseStore::SparseBin));

        // Add up to 16 distinct bins within span <= 2048 -> stays sparse
        for (int i = 1; i <= 14; ++i) {
            store.add(i * 10, 1);
        }
        EXPECT_TRUE(store.is_sparse());
        EXPECT_EQ(16U, store.sparse_bins().size());

        // 17th distinct bin promotes to dense vector
        store.add(199, 5);
        EXPECT_FALSE(store.is_sparse());
        EXPECT_EQ(201U, store.bins().size());
        EXPECT_EQ(49ULL, store.total_count());
    }

    // 2. Batch quantiles(), O(1) cdf(), and merge_serialized() equivalence
    {
        DDSketch sk_a(0.01);
        DDSketch sk_b(0.01);
        DDSketch sk_ref(0.01);
        for (double v : {-500.0, -10.0, 0.0, 0.5, 10.0, 250.0, 1000.0}) {
            sk_a.add(v);
            sk_ref.add(v);
        }
        for (double v : {-200.0, -1.0, 0.0, 5.0, 50.0, 5000.0}) {
            sk_b.add(v);
            sk_ref.add(v);
        }

        std::string wire_b = serialize_sketch(sk_b);
        ASSERT_TRUE(sk_a.merge_serialized(Slice(wire_b)).ok());
        EXPECT_EQ(serialize_sketch(sk_ref), serialize_sketch(sk_a));

        // Batch quantiles (including unsorted and out-of-range elements) matches single quantile()
        const double qs[7] = {0.95, 0.0, 0.50, -0.1, 1.0, 0.25, 1.5};
        double batch_out[7] = {0};
        sk_a.quantiles(qs, 7, batch_out);
        for (size_t i = 0; i < 7; ++i) {
            double single = sk_a.quantile(qs[i]);
            if (std::isnan(single)) {
                EXPECT_TRUE(std::isnan(batch_out[i]));
            } else {
                EXPECT_DOUBLE_EQ(single, batch_out[i]);
            }
        }

        // Verify cdf() across negative, zero, and positive values
        for (double x : {-1000.0, -500.0, -200.0, -10.0, -1.0, -1e-305, 0.0, 1e-305, 0.5, 10.0, 250.0, 10000.0}) {
            double c = sk_a.cdf(x);
            EXPECT_GE(c, 0.0);
            EXPECT_LE(c, 1.0);
        }
    }
}

TEST_F(DDSketchTest, TestUnalignedBuffersAndTruncationFuzz) {
    // 1. Unaligned buffer encode/decode/merge_serialized across alignment offsets 0..15 with 0xFE guard canaries
    DDSketch sk_sparse(0.01);
    for (double v : {-1000.0, -5.0, 0.0, 0.01, 1.0, 50.0, 500.0, 25000.0}) {
        sk_sparse.add(v);
    }
    DDSketch sk_dense(0.01);
    for (int i = 1; i <= 500; ++i) {
        sk_dense.add(static_cast<double>(i) * 0.73);
        sk_dense.add(-static_cast<double>(i) * 1.19);
    }
    sk_dense.add(0.0);

    for (const DDSketch* sk : {&sk_sparse, &sk_dense}) {
        const size_t ser_size = static_cast<size_t>(sk->serialize_size());
        for (size_t align_offset = 0; align_offset < 16; ++align_offset) {
            constexpr size_t kGuard = 16;
            std::vector<uint8_t> raw(kGuard + align_offset + ser_size + kGuard, 0xFE);
            uint8_t* target = raw.data() + kGuard + align_offset;

            const size_t written = sk->serialize(target);
            ASSERT_EQ(ser_size, written);
            for (size_t i = 0; i < kGuard + align_offset; ++i) {
                EXPECT_EQ(0xFE, raw[i]);
            }
            for (size_t i = kGuard + align_offset + ser_size; i < raw.size(); ++i) {
                EXPECT_EQ(0xFE, raw[i]);
            }

            Slice unaligned_slice(reinterpret_cast<const char*>(target), written);
            DDSketch decoded;
            ASSERT_TRUE(decoded.deserialize(unaligned_slice).ok());
            EXPECT_EQ(sk->total_count(), decoded.total_count());
            EXPECT_DOUBLE_EQ(sk->quantile(0.5), decoded.quantile(0.5));
            EXPECT_DOUBLE_EQ(sk->quantile(0.99), decoded.quantile(0.99));

            DDSketch merged;
            ASSERT_TRUE(merged.merge_serialized(unaligned_slice).ok());
            EXPECT_EQ(sk->total_count(), merged.total_count());
            EXPECT_DOUBLE_EQ(sk->quantile(0.5), merged.quantile(0.5));
            EXPECT_DOUBLE_EQ(sk->quantile(0.99), merged.quantile(0.99));
        }

        // 2. Exhaustive prefix truncation [0..ser_size] in exact-sized heap buffers (traps 1-byte ASan overreads)
        std::vector<uint8_t> full(ser_size);
        sk->serialize(full.data());
        for (size_t prefix_len = 0; prefix_len <= ser_size; ++prefix_len) {
            std::unique_ptr<uint8_t[]> exact_buf(prefix_len > 0 ? new uint8_t[prefix_len] : nullptr);
            if (prefix_len > 0) {
                std::memcpy(exact_buf.get(), full.data(), prefix_len);
            }
            Slice s(reinterpret_cast<const char*>(exact_buf.get()), prefix_len);

            DDSketch d;
            Status st_d = d.deserialize(s);
            if (prefix_len == 0 || prefix_len == ser_size) {
                EXPECT_TRUE(st_d.ok());
            }

            DDSketch m(0.01);
            m.add(1.0);
            Status st_m = m.merge_serialized(s);
            if (prefix_len == 0 || prefix_len == ser_size) {
                EXPECT_TRUE(st_m.ok());
            }
        }
    }

    // 3. Deterministic byte-level mutation fuzzing in exact-sized ASan heap allocations
    std::string seed_bytes = serialize_sketch(sk_sparse);
    std::mt19937_64 rng(20261120ULL);
    for (int iter = 0; iter < 2000; ++iter) {
        std::string payload = seed_bytes;
        size_t num_mut = (rng() % 3) + 1;
        for (size_t m = 0; m < num_mut; ++m) {
            size_t pos = rng() % payload.size();
            payload[pos] ^= static_cast<char>((rng() & 0xFF) | 1);
        }
        std::unique_ptr<uint8_t[]> exact_buf(new uint8_t[payload.size()]);
        std::memcpy(exact_buf.get(), payload.data(), payload.size());
        Slice fuzzed(reinterpret_cast<const char*>(exact_buf.get()), payload.size());

        DDSketch d;
        if (d.deserialize(fuzzed).ok()) {
            (void)d.quantile(0.5);
            (void)d.cdf(0.0);
            (void)d.sum();
        }
        DDSketch m(0.01);
        m.add(10.0);
        if (m.merge_serialized(fuzzed).ok()) {
            (void)m.quantile(0.5);
            (void)m.cdf(10.0);
            (void)m.sum();
        }
    }
}

namespace {

std::string hex_to_bytes(const char* hex) {
    std::string out;
    auto hex_nibble = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
        if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
        return 0;
    };
    for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0'; i += 2) {
        uint8_t b = static_cast<uint8_t>((hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1]));
        out.push_back(static_cast<char>(b));
    }
    return out;
}

} // namespace

// Golden fixtures generated directly by official com.datadoghq:sketches-java:0.8.3
// (DDSketchProtoBinding.toProto(sketch).toByteArray()).
TEST_F(DDSketchTest, TestOfficialDatadogJavaGoldenFixtures) {
    struct GoldenCase {
        const char* name;
        const char* hex;
        uint64_t count;
        double sum;
        double avg;
        double p0;
        double p50;
        double p90;
        double p99;
        double p100;
    };

    const GoldenCase cases[] = {
            // Case 1: LogarithmicMapping(0.01), single observation 100.0 (UnboundedSizeDenseStore)
            {"log_single_100_alpha_0_01_dense", "0a0909fd4a815abf52f03f120d1208000000000000f03f18cc031a00", 1ULL,
             100.49456770856492, 100.49456770856492, 100.49456770856492, 100.49456770856492, 100.49456770856492,
             100.49456770856492, 100.49456770856492},
            // Case 2: LogarithmicMapping(0.01), {0.005, 0.5, 1.0, 10.0, 100.0, 1000.0} (SparseStore)
            {"log_powers_and_fractions_alpha_0_01_sparse",
             "0a0909fd4a815abf52f03f12520a0c08910411000000000000f03f0a0b084511000000000000f03f0a0b080011000000000000f03"
             "f"
             "0a0c08e60111000000000000f03f0a0c08cc0311000000000000f03f0a0c08b20511000000000000f03f1a00",
             6ULL, 1114.5138529928140, 185.75230883213567, 0.0050406192044765320, 1.0100000000000000,
             100.49456770856492, 100.49456770856492, 1002.4280085221300},
            // Case 3: LogarithmicMapping(0.01), {-500.0, -10.0, -1.0, 0.0, 0.0, 1.0, 10.0, 500.0} (SparseStore)
            {"log_signed_and_zero_alpha_0_01_sparse",
             "0a0909fd4a815abf52f03f12290a0b080011000000000000f03f0a0c08e60111000000000000f03f0a0c08ec0411000000000000f"
             "0"
             "3f1a290a0b080011000000000000f03f0a0c08e60111000000000000f03f0a0c08ec0411000000000000f03f21000000000000004"
             "0",
             8ULL, 0.0, 0.0, -497.77940145581560, 0.0, 10.074696689511330, 10.074696689511330, 497.77940145581560},
            // Case 4: LogarithmicMapping(0.01), [10.0 .. 25.0] (UnboundedSizeDenseStore contiguous counts)
            {"log_contiguous_10_to_25_alpha_0_01_dense",
             "0a0909fd4a815abf52f03f12f60212f002000000000000f03f0000000000000000000000000000000000000000000000000000000"
             "0"
             "0000f03f0000000000000000000000000000000000000000000000000000000000000000000000000000f03f00000000000000000"
             "0"
             "000000000000000000000000000000000000000000f03f00000000000000000000000000000000000000000000f03f00000000000"
             "0"
             "000000000000000000000000000000000000000000000000f03f00000000000000000000000000000000000000000000f03f00000"
             "0"
             "00000000000000000000000000000000000000f03f00000000000000000000000000000000000000000000f03f000000000000000"
             "0"
             "0000000000000000000000000000f03f0000000000000000000000000000f03f00000000000000000000000000000000000000000"
             "0"
             "00f03f0000000000000000000000000000f03f0000000000000000000000000000f03f0000000000000000000000000000f03f000"
             "0"
             "000000000000000000000000f03f18e6011a00",
             16ULL, 279.47307544471540, 17.467067215294712, 10.074696689511330, 16.946212143511012, 22.875222481776756,
             23.808809768044550, 24.780498769903320},
            // Case 5: LogarithmicMapping(0.001), 100x 1.0 + 20 log-spaced up to 1000.0 (SparseStore)
            {"log_bimodal_1_to_1000_alpha_0_001_sparse",
             "0a0909830052403308f03f12a5020a0b08001100000000000059400a0c08d80211000000000000f03f0a0c08b2051100000000000"
             "0"
             "f03f0a0c088c0811000000000000f03f0a0c08e40a11000000000000f03f0a0c08be0d11000000000000f03f0a0c0898101100000"
             "0"
             "000000f03f0a0c08f01211000000000000f03f0a0c08ca1511000000000000f03f0a0c08a41811000000000000f03f0a0c08fc1a1"
             "1"
             "000000000000f03f0a0c08d61d11000000000000f03f0a0c08b02011000000000000f03f0a0c088a2311000000000000f03f0a0c0"
             "8"
             "e22511000000000000f03f0a0c08bc2811000000000000f03f0a0c08962b11000000000000f03f0a0c08ee2d11000000000000f03"
             "f"
             "0a0c08c83011000000000000f03f0a0c08a23311000000000000f03f0a0c08fa3511000000000000f03f1a00",
             120ULL, 3520.4500898422425, 29.337084082018688, 1.0010000000000000, 1.0010000000000000, 15.847320319275351,
             501.19817027865133, 999.24680714442010},
            // Case 6: Default Java CubicallyInterpolatedMapping(0.01) + SparseStore
            {"cubic_default_java_mixed_alpha_0_01_sparse",
             "0a0b09c2f1e7e9ed51f03f180312530a0c088b0111000000000000f03f0a0b080011000000000000f03f0a0c08f80211000000000"
             "0"
             "00f03f0a0c08d00311000000000000f03f0a0c08b80511000000000000f03f0a0c08c40811000000000000f03f1a1b0a0b0800110"
             "0"
             "0000000000f03f0a0c08ac0411000000000000f03f21000000000000f03f",
             9ULL, 51046.604341873810, 5671.8449268748680, -248.76017323768866, 1.0100000000000000, 995.08612904744850,
             995.08612904744850, 50158.312820030056},
            // Case 7: Default Java DDSketches.unboundedDense(0.01) over [10.0 .. 25.0] (contiguous dense + CUBIC)
            {"cubic_default_java_contiguous_10_to_25_alpha_0_01_dense",
             "0a0b09c2f1e7e9ed51f03f180312fe0212f802000000000000f03f000000000000000000000000000000000000000000000000000"
             "0"
             "000000000000000000000000f03f000000000000000000000000000000000000000000000000000000000000f03f0000000000000"
             "0"
             "0000000000000000000000000000000000000000000000f03f0000000000000000000000000000000000000000000000000000000"
             "0"
             "0000f03f00000000000000000000000000000000000000000000f03f00000000000000000000000000000000000000000000f03f0"
             "0"
             "0000000000000000000000000000000000000000000000000000000000f03f0000000000000000000000000000f03f00000000000"
             "0"
             "00000000000000000000000000000000f03f00000000000000000000000000000000000000000000f03f000000000000000000000"
             "0"
             "000000f03f00000000000000000000000000000000000000000000f03f0000000000000000000000000000f03f000000000000000"
             "0"
             "000000000000f03f0000000000000000000000000000f03f18e8011a00",
             16ULL, 280.16044981553650, 17.510028113471030, 10.044983161068599, 17.156494165845820, 23.068115741661620,
             24.002636914918774, 24.977983750285134},
    };

    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.name);
        const std::string wire = hex_to_bytes(tc.hex);

        // 1. Direct deserialize() from official Java 0.8.3 protobuf bytes
        DDSketch decoded;
        Status st = decoded.deserialize(Slice(wire));
        ASSERT_TRUE(st.ok()) << tc.name << ": " << st.to_string();
        EXPECT_EQ(tc.count, decoded.total_count());
        EXPECT_NEAR(tc.sum, decoded.sum(), 1e-9);
        EXPECT_NEAR(tc.avg, decoded.avg(), 1e-9);
        EXPECT_NEAR(tc.p0, decoded.quantile(0.0), 1e-11);
        EXPECT_NEAR(tc.p50, decoded.quantile(0.5), 1e-11);
        EXPECT_NEAR(tc.p90, decoded.quantile(0.9), 1e-11);
        EXPECT_NEAR(tc.p99, decoded.quantile(0.99), 1e-11);
        EXPECT_NEAR(tc.p100, decoded.quantile(1.0), 1e-11);

        // 2. Direct merge_serialized() from official Java 0.8.3 protobuf bytes
        DDSketch merged;
        Status st_m = merged.merge_serialized(Slice(wire));
        ASSERT_TRUE(st_m.ok()) << tc.name << ": " << st_m.to_string();
        EXPECT_EQ(tc.count, merged.total_count());
        EXPECT_NEAR(tc.p50, merged.quantile(0.5), 1e-11);
        EXPECT_NEAR(tc.p99, merged.quantile(0.99), 1e-11);

        // 3. Re-serialize from C++ (including CUBIC interpolation tag 0x18 0x03) and re-decode
        std::string reserialized = serialize_sketch(decoded);
        DDSketch roundtrip;
        ASSERT_TRUE(roundtrip.deserialize(Slice(reserialized)).ok());
        EXPECT_EQ(decoded.interpolation(), roundtrip.interpolation());
        EXPECT_EQ(tc.count, roundtrip.total_count());
        EXPECT_NEAR(tc.sum, roundtrip.sum(), 1e-9);
        EXPECT_NEAR(tc.p0, roundtrip.quantile(0.0), 1e-11);
        EXPECT_NEAR(tc.p50, roundtrip.quantile(0.5), 1e-11);
        EXPECT_NEAR(tc.p90, roundtrip.quantile(0.9), 1e-11);
        EXPECT_NEAR(tc.p99, roundtrip.quantile(0.99), 1e-11);
        EXPECT_NEAR(tc.p100, roundtrip.quantile(1.0), 1e-11);
    }

    // 4. Verify native C++ accumulation produces identical quantile, sum, and avg to Java 0.8.3
    {
        DDSketch native_single(0.01);
        native_single.add(100.0);
        EXPECT_EQ(230, native_single.key(100.0));
        EXPECT_NEAR(100.49456770856492, native_single.quantile(0.5), 1e-12);

        DDSketch native_powers(0.01);
        for (double v : {0.005, 0.5, 1.0, 10.0, 100.0, 1000.0}) {
            native_powers.add(v);
        }
        EXPECT_EQ(6ULL, native_powers.total_count());
        EXPECT_NEAR(1114.5138529928140, native_powers.sum(), 1e-9);
        EXPECT_NEAR(185.75230883213567, native_powers.avg(), 1e-9);
        EXPECT_NEAR(0.0050406192044765320, native_powers.quantile(0.0), 1e-12);
        EXPECT_NEAR(1.0100000000000000, native_powers.quantile(0.5), 1e-12);
        EXPECT_NEAR(100.49456770856492, native_powers.quantile(0.9), 1e-12);
        EXPECT_NEAR(1002.4280085221300, native_powers.quantile(1.0), 1e-11);
    }

    // 5. Verify native C++ accumulation at alpha = 0.001 over [1.0 .. 1000.0] matches Java 0.8.3 without collapsing
    {
        DDSketch native_001(0.001);
        for (int i = 0; i < 100; ++i) {
            native_001.add(1.0);
        }
        for (int i = 1; i <= 20; ++i) {
            native_001.add(std::pow(1000.0, static_cast<double>(i) / 20.0));
        }
        EXPECT_EQ(120ULL, native_001.total_count());
        EXPECT_NEAR(3520.4500898422425, native_001.sum(), 1e-9);
        EXPECT_NEAR(29.337084082018688, native_001.avg(), 1e-9);
        EXPECT_NEAR(1.001, native_001.quantile(0.0), 1e-11);
        EXPECT_NEAR(1.001, native_001.quantile(0.5), 1e-11);
        EXPECT_NEAR(15.847320319275351, native_001.quantile(0.9), 1e-11);
        EXPECT_NEAR(501.19817027865133, native_001.quantile(0.99), 1e-11);
        EXPECT_NEAR(999.24680714442010, native_001.quantile(1.0), 1e-11);
    }

    // 6. Verify merging a CUBIC sketch with a NONE (logarithmic) sketch fails with incompatible IndexMapping
    {
        DDSketch cubic_sk;
        ASSERT_TRUE(cubic_sk.deserialize(Slice(hex_to_bytes(cases[5].hex))).ok());
        EXPECT_EQ(DDSketch::kInterpolationCubic, cubic_sk.interpolation());

        DDSketch log_sk(0.01);
        log_sk.add(100.0);
        EXPECT_EQ(DDSketch::kInterpolationNone, log_sk.interpolation());

        EXPECT_TRUE(cubic_sk.merge(log_sk).is_invalid_argument());
        EXPECT_TRUE(log_sk.merge(cubic_sk).is_invalid_argument());
    }
}

} // namespace starrocks
