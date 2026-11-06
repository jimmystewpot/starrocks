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

#include <arrow/builder.h>
#include <arrow/io/file.h>
#include <arrow/ipc/writer.h>
#include <arrow/util/compression.h>
#include <benchmark/benchmark.h>
#include <glog/logging.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <utility>
#include <vector>

#include "base/utility/defer_op.h"
#include "column/chunk.h"
#include "common/simdjson_util.h"
#include "compute_env/load/load_stream_mgr.h"
#include "compute_env/load/stream_load_pipe.h"
#include "connector/file/scanner/arrow_scanner.h"
#include "connector/file/scanner/json_scanner.h"
#include "gen_cpp/Descriptors_types.h"
#include "runtime/current_thread.h"
#include "runtime/descriptors.h"
#include "runtime/mem_tracker.h"
#include "runtime/runtime_state.h"
#include "runtime/service_contexts.h"
#include "types/type_descriptor.h"

namespace starrocks {

static void generate_test_files(const std::string& arrow_path, const std::string& arrow_lz4_path,
                                const std::string& json_path, const std::string& json_unicode_path,
                                const std::string& json_number_path, const std::string& json_wide_path,
                                int64_t num_rows) {
    // 1. Generate Arrow IPC Stream File
    arrow::Int32Builder int_builder;
    arrow::StringBuilder str_builder;
    arrow::DoubleBuilder double_builder;

    for (int64_t i = 0; i < num_rows; ++i) {
        (void)int_builder.Append(static_cast<int32_t>(i));
        (void)str_builder.Append("varchar_str_" + std::to_string(i % 100));
        (void)double_builder.Append(static_cast<double>(i) * 1.5);
    }

    std::shared_ptr<arrow::Array> int_array;
    std::shared_ptr<arrow::Array> str_array;
    std::shared_ptr<arrow::Array> double_array;

    (void)int_builder.Finish(&int_array);
    (void)str_builder.Finish(&str_array);
    (void)double_builder.Finish(&double_array);

    auto schema = arrow::schema({arrow::field("c0_int", arrow::int32()), arrow::field("c1_str", arrow::utf8()),
                                 arrow::field("c2_double", arrow::float64())});

    auto batch = arrow::RecordBatch::Make(schema, num_rows, {int_array, str_array, double_array});

    auto out_file_res = arrow::io::FileOutputStream::Open(arrow_path);
    if (!out_file_res.ok()) {
        LOG(FATAL) << "Failed to open Arrow stream file: " << out_file_res.status().ToString();
    }
    auto out_file = out_file_res.ValueOrDie();

    auto writer_res = arrow::ipc::MakeStreamWriter(out_file, schema);
    if (!writer_res.ok()) {
        LOG(FATAL) << "Failed to create Arrow stream writer: " << writer_res.status().ToString();
    }
    auto writer = writer_res.ValueOrDie();

    (void)writer->WriteRecordBatch(*batch);
    (void)writer->Close();
    (void)out_file->Close();

    // 2. Generate LZ4-Frame Compressed Arrow IPC Stream File
    arrow::ipc::IpcWriteOptions lz4_write_options = arrow::ipc::IpcWriteOptions::Defaults();
    auto lz4_codec_res = arrow::util::Codec::Create(arrow::Compression::LZ4_FRAME);
    if (!lz4_codec_res.ok()) {
        LOG(FATAL) << "Failed to create LZ4_FRAME codec: " << lz4_codec_res.status().ToString();
    }
    lz4_write_options.codec = std::move(lz4_codec_res).ValueOrDie();

    auto lz4_out_file_res = arrow::io::FileOutputStream::Open(arrow_lz4_path);
    if (!lz4_out_file_res.ok()) {
        LOG(FATAL) << "Failed to open LZ4 Arrow stream file: " << lz4_out_file_res.status().ToString();
    }
    auto lz4_out_file = lz4_out_file_res.ValueOrDie();

    auto lz4_writer_res = arrow::ipc::MakeStreamWriter(lz4_out_file, schema, lz4_write_options);
    if (!lz4_writer_res.ok()) {
        LOG(FATAL) << "Failed to create LZ4 Arrow stream writer: " << lz4_writer_res.status().ToString();
    }
    auto lz4_writer = lz4_writer_res.ValueOrDie();

    (void)lz4_writer->WriteRecordBatch(*batch);
    (void)lz4_writer->Close();
    (void)lz4_out_file->Close();

    // 3. Generate Line-delimited JSON (NDJSON) File
    std::ofstream json_file(json_path);
    if (!json_file.is_open()) {
        LOG(FATAL) << "Failed to open JSON file for writing: " << json_path;
    }
    for (int64_t i = 0; i < num_rows; ++i) {
        json_file << "{\"c0_int\":" << i << ",\"c1_str\":\"varchar_str_" << (i % 100)
                  << "\",\"c2_double\":" << (static_cast<double>(i) * 1.5) << "}\n";
    }
    json_file.close();

    // 4. Generate Unicode-escape-heavy NDJSON File
    std::ofstream json_unicode_file(json_unicode_path);
    if (!json_unicode_file.is_open()) {
        LOG(FATAL) << "Failed to open JSON unicode file for writing: " << json_unicode_path;
    }
    for (int64_t i = 0; i < num_rows; ++i) {
        json_unicode_file << "{\"c0_int\":" << i << ",\"c1_str\":\"prefix_\\u6C49\\u5B57\\u4E2D\\u6587\\u6D4B\\u8BD5_"
                          << (i % 100) << "\",\"c2_double\":" << (static_cast<double>(i) * 1.5) << "}\n";
    }
    json_unicode_file.close();

    // 5. Generate Number-heavy NDJSON File
    std::ofstream json_number_file(json_number_path);
    if (!json_number_file.is_open()) {
        LOG(FATAL) << "Failed to open JSON number file for writing: " << json_number_path;
    }
    json_number_file << std::fixed << std::setprecision(6);
    for (int64_t i = 0; i < num_rows; ++i) {
        json_number_file << "{\"c0_int\":" << (100000000 + i)
                         << ",\"c1_str\":\"n\",\"c2_double\":" << (123456.789012 + static_cast<double>(i) * 0.125)
                         << "}\n";
    }
    json_number_file.close();

    // 6. Generate Wide-record NDJSON File (16 unprojected fields followed by 3 projected table columns)
    std::ofstream json_wide_file(json_wide_path);
    if (!json_wide_file.is_open()) {
        LOG(FATAL) << "Failed to open JSON wide file for writing: " << json_wide_path;
    }
    static constexpr const char* kWidePrefix =
            "{\"f01\":\"payload_padding_string_0123456789\","
            "\"f02\":\"payload_padding_string_0123456789\","
            "\"f03\":\"payload_padding_string_0123456789\","
            "\"f04\":\"payload_padding_string_0123456789\","
            "\"f05\":\"payload_padding_string_0123456789\","
            "\"f06\":\"payload_padding_string_0123456789\","
            "\"f07\":\"payload_padding_string_0123456789\","
            "\"f08\":\"payload_padding_string_0123456789\","
            "\"f09\":\"payload_padding_string_0123456789\","
            "\"f10\":\"payload_padding_string_0123456789\","
            "\"f11\":\"payload_padding_string_0123456789\","
            "\"f12\":\"payload_padding_string_0123456789\","
            "\"f13\":\"payload_padding_string_0123456789\","
            "\"f14\":\"payload_padding_string_0123456789\","
            "\"f15\":\"payload_padding_string_0123456789\","
            "\"f16\":\"payload_padding_string_0123456789\",";
    for (int64_t i = 0; i < num_rows; ++i) {
        json_wide_file << kWidePrefix << "\"c0_int\":" << i << ",\"c1_str\":\"varchar_str_" << (i % 100)
                       << "\",\"c2_double\":" << (static_cast<double>(i) * 1.5) << "}\n";
    }
    json_wide_file.close();
}

static DescriptorTbl* create_descriptor_table(RuntimeState* state, ObjectPool* pool) {
    TDescriptorTable t_desc_table;

    // Table Descriptor
    TTableDescriptor t_tab_desc;
    t_tab_desc.id = 0;
    t_tab_desc.tableType = TTableType::BROKER_TABLE;
    t_tab_desc.numClusteringCols = 0;
    t_desc_table.tableDescriptors.push_back(t_tab_desc);
    t_desc_table.__isset.tableDescriptors = true;

    // Tuple Descriptor
    TTupleDescriptor t_tuple_desc;
    t_tuple_desc.id = 0;
    t_tuple_desc.byteSize = 32;
    t_tuple_desc.numNullBytes = 0;
    t_tuple_desc.tableId = 0;
    t_desc_table.tupleDescriptors.push_back(t_tuple_desc);

    // Slot 0: c0_int (INT)
    TSlotDescriptor slot0;
    slot0.id = 0;
    slot0.parent = 0;
    slot0.slotType = TypeDescriptor(TYPE_INT).to_protobuf();
    slot0.columnPos = 0;
    slot0.byteOffset = 0;
    slot0.nullIndicatorByte = 0;
    slot0.nullIndicatorBit = -1;
    slot0.colName = "c0_int";
    slot0.slotIdx = 0;
    slot0.isMaterialized = true;
    t_desc_table.slotDescriptors.push_back(slot0);

    // Slot 1: c1_str (VARCHAR)
    TSlotDescriptor slot1;
    slot1.id = 1;
    slot1.parent = 0;
    slot1.slotType = TypeDescriptor::create_varchar_type(TypeDescriptor::MAX_VARCHAR_LENGTH).to_protobuf();
    slot1.columnPos = 1;
    slot1.byteOffset = 8;
    slot1.nullIndicatorByte = 0;
    slot1.nullIndicatorBit = -1;
    slot1.colName = "c1_str";
    slot1.slotIdx = 1;
    slot1.isMaterialized = true;
    t_desc_table.slotDescriptors.push_back(slot1);

    // Slot 2: c2_double (DOUBLE)
    TSlotDescriptor slot2;
    slot2.id = 2;
    slot2.parent = 0;
    slot2.slotType = TypeDescriptor(TYPE_DOUBLE).to_protobuf();
    slot2.columnPos = 2;
    slot2.byteOffset = 24;
    slot2.nullIndicatorByte = 0;
    slot2.nullIndicatorBit = -1;
    slot2.colName = "c2_double";
    slot2.slotIdx = 2;
    slot2.isMaterialized = true;
    t_desc_table.slotDescriptors.push_back(slot2);

    DescriptorTbl* desc_tbl = nullptr;
    Status st = DescriptorTbl::create(state, pool, t_desc_table, &desc_tbl, config::vector_chunk_size);
    if (!st.ok()) {
        LOG(FATAL) << "Failed to create descriptor table: " << st.to_string();
    }
    return desc_tbl;
}

static TBrokerScanRange* create_scan_range(ObjectPool* pool, const std::string& path, TFileFormatType::type format,
                                          const DescriptorTbl* desc_tbl) {
    auto* scan_range = pool->add(new TBrokerScanRange());
    scan_range->params.src_tuple_id = 0;
    scan_range->params.src_slot_ids = {0, 1, 2};

    for (int i = 0; i < 3; ++i) {
        TTypeDesc type_desc;
        TTypeNode node;
        node.type = TTypeNodeType::SCALAR;
        TScalarType scalar_type;
        if (i == 0) {
            scalar_type.type = TPrimitiveType::INT;
        } else if (i == 1) {
            scalar_type.type = TPrimitiveType::VARCHAR;
            scalar_type.__set_len(100);
        } else {
            scalar_type.type = TPrimitiveType::DOUBLE;
        }
        node.__set_scalar_type(scalar_type);
        type_desc.types.push_back(node);

        TExpr expr;
        TExprNode expr_node;
        expr_node.node_type = TExprNodeType::SLOT_REF;
        expr_node.type = type_desc;
        expr_node.num_children = 0;
        TSlotRef slot_ref;
        slot_ref.slot_id = i;
        slot_ref.tuple_id = 0;
        expr_node.__set_slot_ref(slot_ref);
        expr.nodes.push_back(expr_node);

        scan_range->params.expr_of_dest_slot[i] = expr;
    }

    scan_range->params.dest_tuple_id = 0;
    scan_range->params.dest_sid_to_src_sid_without_trans = {{0, 0}, {1, 1}, {2, 2}};

    TBrokerRangeDesc range_desc;
    range_desc.path = path;
    range_desc.start_offset = 0;
    range_desc.size = -1;
    range_desc.format_type = format;
    range_desc.file_type = TFileType::FILE_LOCAL;
    range_desc.splittable = false;
    range_desc.__set_strip_outer_array(false);
    range_desc.__set_jsonpaths("");
    range_desc.__set_json_root("");
    scan_range->ranges.push_back(range_desc);

    return scan_range;
}

static std::string g_arrow_file_path;
static std::string g_arrow_lz4_file_path;
static std::string g_json_file_path;
static std::string g_json_unicode_file_path;
static std::string g_json_number_file_path;
static std::string g_json_wide_file_path;
static constexpr int64_t kNumRows = 100000;

static void setup_global_files() {
    char arrow_pattern[] = "/tmp/arrow_bench_XXXXXX.arrow";
    int fd_arrow = mkstemps(arrow_pattern, 6);
    if (fd_arrow != -1) {
        close(fd_arrow);
        g_arrow_file_path = arrow_pattern;
    } else {
        g_arrow_file_path = "/tmp/arrow_bench.arrow";
    }

    char arrow_lz4_pattern[] = "/tmp/arrow_lz4_bench_XXXXXX.arrow";
    int fd_arrow_lz4 = mkstemps(arrow_lz4_pattern, 6);
    if (fd_arrow_lz4 != -1) {
        close(fd_arrow_lz4);
        g_arrow_lz4_file_path = arrow_lz4_pattern;
    } else {
        g_arrow_lz4_file_path = "/tmp/arrow_lz4_bench.arrow";
    }

    char json_pattern[] = "/tmp/json_bench_XXXXXX.json";
    int fd_json = mkstemps(json_pattern, 5);
    if (fd_json != -1) {
        close(fd_json);
        g_json_file_path = json_pattern;
    } else {
        g_json_file_path = "/tmp/json_bench.json";
    }

    char json_unicode_pattern[] = "/tmp/json_unicode_bench_XXXXXX.json";
    int fd_json_unicode = mkstemps(json_unicode_pattern, 5);
    if (fd_json_unicode != -1) {
        close(fd_json_unicode);
        g_json_unicode_file_path = json_unicode_pattern;
    } else {
        g_json_unicode_file_path = "/tmp/json_unicode_bench.json";
    }

    char json_number_pattern[] = "/tmp/json_number_bench_XXXXXX.json";
    int fd_json_number = mkstemps(json_number_pattern, 5);
    if (fd_json_number != -1) {
        close(fd_json_number);
        g_json_number_file_path = json_number_pattern;
    } else {
        g_json_number_file_path = "/tmp/json_number_bench.json";
    }

    char json_wide_pattern[] = "/tmp/json_wide_bench_XXXXXX.json";
    int fd_json_wide = mkstemps(json_wide_pattern, 5);
    if (fd_json_wide != -1) {
        close(fd_json_wide);
        g_json_wide_file_path = json_wide_pattern;
    } else {
        g_json_wide_file_path = "/tmp/json_wide_bench.json";
    }

    generate_test_files(g_arrow_file_path, g_arrow_lz4_file_path, g_json_file_path, g_json_unicode_file_path,
                        g_json_number_file_path, g_json_wide_file_path, kNumRows);
}

static void cleanup_global_files() {
    std::error_code ec;
    std::filesystem::remove(g_arrow_file_path, ec);
    std::filesystem::remove(g_arrow_lz4_file_path, ec);
    std::filesystem::remove(g_json_file_path, ec);
    std::filesystem::remove(g_json_unicode_file_path, ec);
    std::filesystem::remove(g_json_number_file_path, ec);
    std::filesystem::remove(g_json_wide_file_path, ec);
}

static void BM_ArrowScanner(benchmark::State& state) {
    ObjectPool pool;
    TQueryOptions query_options;
    TQueryGlobals query_globals;
    auto* runtime_state = pool.add(new RuntimeState(TUniqueId(), query_options, query_globals, nullptr));
    runtime_state->init_instance_mem_tracker();

    auto* desc_tbl = create_descriptor_table(runtime_state, &pool);
    runtime_state->set_desc_tbl(desc_tbl);

    auto* scan_range = create_scan_range(&pool, g_arrow_file_path, TFileFormatType::FORMAT_ARROW, desc_tbl);

    for (auto _ : state) {
        auto profile = pool.add(new RuntimeProfile("arrow_bench_prof"));
        auto counter = pool.add(new ScannerCounter());
        auto scanner = std::make_unique<ArrowScanner>(runtime_state, profile, *scan_range, counter);

        Status st = scanner->open();
        if (!st.ok()) {
            state.SkipWithError(("Failed to open ArrowScanner: " + st.to_string()).c_str());
            break;
        }

        int64_t total_rows = 0;
        while (true) {
            auto res = scanner->get_next();
            if (res.status().is_end_of_file()) {
                break;
            }
            if (!res.ok()) {
                state.SkipWithError(("Failed to scan chunk: " + res.status().to_string()).c_str());
                break;
            }
            auto chunk = res.value();
            if (chunk) {
                total_rows += chunk->num_rows();
            }
        }
        scanner->close();
        benchmark::DoNotOptimize(total_rows);
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kNumRows);
}

class BenchDiscreteStreamLoadPipe : public StreamLoadPipe {
public:
    using StreamLoadPipe::StreamLoadPipe;
    bool is_discrete_message_pipe() const override { return true; }
};

static void run_arrow_routine_load_pipe_benchmark(benchmark::State& state, const std::string& arrow_file_path) {
    std::ifstream file(arrow_file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        state.SkipWithError("Failed to open Arrow bench stream file");
        return;
    }
    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string arrow_bytes(file_size, ' ');
    if (!file.read(&arrow_bytes[0], file_size)) {
        state.SkipWithError("Failed to read Arrow bench stream file");
        return;
    }

    LoadStreamMgr load_stream_mgr;

    for (auto _ : state) {
        ObjectPool pool;
        TQueryOptions query_options;
        TQueryGlobals query_globals;
        auto* runtime_state = pool.add(new RuntimeState(TUniqueId(), query_options, query_globals, nullptr));
        runtime_state->init_instance_mem_tracker();

        auto* desc_tbl = create_descriptor_table(runtime_state, &pool);
        runtime_state->set_desc_tbl(desc_tbl);

        const UniqueId load_id = UniqueId::gen_uid();
        const TUniqueId pipe_id = load_id.to_thrift();

        auto* scan_range = pool.add(new TBrokerScanRange());
        scan_range->params.src_tuple_id = 0;
        scan_range->params.src_slot_ids = {0, 1, 2};
        for (int i = 0; i < 3; ++i) {
            TTypeDesc type_desc;
            TTypeNode node;
            node.type = TTypeNodeType::SCALAR;
            TScalarType scalar_type;
            if (i == 0) {
                scalar_type.type = TPrimitiveType::INT;
            } else if (i == 1) {
                scalar_type.type = TPrimitiveType::VARCHAR;
                scalar_type.__set_len(100);
            } else {
                scalar_type.type = TPrimitiveType::DOUBLE;
            }
            node.__set_scalar_type(scalar_type);
            type_desc.types.push_back(node);

            TExpr expr;
            TExprNode expr_node;
            expr_node.node_type = TExprNodeType::SLOT_REF;
            expr_node.type = type_desc;
            expr_node.num_children = 0;
            TSlotRef slot_ref;
            slot_ref.slot_id = i;
            slot_ref.tuple_id = 0;
            expr_node.__set_slot_ref(slot_ref);
            expr.nodes.push_back(expr_node);

            scan_range->params.expr_of_dest_slot[i] = expr;
        }
        scan_range->params.dest_tuple_id = 0;
        scan_range->params.dest_sid_to_src_sid_without_trans = {{0, 0}, {1, 1}, {2, 2}};

        TBrokerRangeDesc range_desc;
        range_desc.path = "";
        range_desc.start_offset = 0;
        range_desc.size = -1;
        range_desc.format_type = TFileFormatType::FORMAT_ARROW;
        range_desc.file_type = TFileType::FILE_STREAM;
        range_desc.splittable = false;
        range_desc.__set_strip_outer_array(false);
        range_desc.__set_jsonpaths("");
        range_desc.__set_json_root("");
        range_desc.__set_load_id(pipe_id);
        scan_range->ranges.push_back(range_desc);

        std::shared_ptr<BenchDiscreteStreamLoadPipe> pipe =
                std::make_shared<BenchDiscreteStreamLoadPipe>(1024 * 1024, 64 * 1024 * 1024);
        Status st = load_stream_mgr.put(pipe_id, pipe);
        if (!st.ok()) {
            state.SkipWithError(st.to_string().c_str());
            break;
        }
        DeferOp remove_pipe([&]() { load_stream_mgr.remove(pipe_id); });

        auto buffer = ByteBuffer::allocate_with_tracker(arrow_bytes.size());
        if (!buffer.ok()) {
            state.SkipWithError(buffer.status().to_string().c_str());
            break;
        }
        auto bb = std::move(buffer).value();
        bb->put_bytes(arrow_bytes.data(), arrow_bytes.size());
        bb->flip_to_read();
        st = pipe->append(std::move(bb));
        if (st.ok()) {
            st = pipe->finish();
        }
        if (!st.ok()) {
            state.SkipWithError(st.to_string().c_str());
            break;
        }

        auto profile = pool.add(new RuntimeProfile("arrow_routine_load_bench_prof"));
        auto counter = pool.add(new ScannerCounter());
        auto scanner = std::make_unique<ArrowScanner>(runtime_state, profile, *scan_range, counter);

        st = scanner->open();
        if (!st.ok()) {
            state.SkipWithError(("Failed to open ArrowScanner for pipe: " + st.to_string()).c_str());
            break;
        }

        int64_t total_rows = 0;
        bool scanner_failed = false;
        while (true) {
            auto res = scanner->get_next();
            if (res.status().is_end_of_file()) {
                break;
            }
            if (!res.ok()) {
                state.SkipWithError(("Failed to scan chunk from pipe: " + res.status().to_string()).c_str());
                scanner_failed = true;
                break;
            }
            auto chunk = res.value();
            if (chunk) {
                total_rows += chunk->num_rows();
            }
        }
        scanner->close();
        if (scanner_failed) {
            break;
        }
        if (total_rows != kNumRows) {
            state.SkipWithError("Arrow pipe benchmark did not ingest every input row");
            break;
        }
        benchmark::DoNotOptimize(total_rows);
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kNumRows);
}

static void BM_ArrowRoutineLoadPipeScanner(benchmark::State& state) {
    run_arrow_routine_load_pipe_benchmark(state, g_arrow_file_path);
}

static void BM_ArrowRoutineLoadPipeScannerLz4(benchmark::State& state) {
    run_arrow_routine_load_pipe_benchmark(state, g_arrow_lz4_file_path);
}

static void run_json_scanner_benchmark(benchmark::State& state, const std::string& json_path,
                                       const char* requested_impl = nullptr) {
    const simdjson::implementation* original_impl = simdjson::get_active_implementation();
    DeferOp restore_impl([original_impl]() { simdjson::get_active_implementation() = original_impl; });
    if (requested_impl != nullptr) {
        const simdjson::implementation* target_impl = simdjson::get_available_implementations()[requested_impl];
        if (target_impl == nullptr || !target_impl->supported_by_runtime_system()) {
            state.SkipWithError("Requested simdjson implementation is not available on this host");
            return;
        }
        simdjson::get_active_implementation() = target_impl;
    }
    state.SetLabel(std::string(SIMDJSON_VERSION) + "/" + simdjson::get_active_implementation()->name());

    ObjectPool pool;
    TQueryOptions query_options;
    TQueryGlobals query_globals;
    auto* runtime_state = pool.add(new RuntimeState(TUniqueId(), query_options, query_globals, nullptr));
    runtime_state->init_instance_mem_tracker();

    auto* desc_tbl = create_descriptor_table(runtime_state, &pool);
    runtime_state->set_desc_tbl(desc_tbl);

    auto* scan_range = create_scan_range(&pool, json_path, TFileFormatType::FORMAT_JSON, desc_tbl);

    for (auto _ : state) {
        auto profile = pool.add(new RuntimeProfile("json_bench_prof"));
        auto counter = pool.add(new ScannerCounter());
        auto scanner = std::make_unique<JsonScanner>(runtime_state, profile, *scan_range, counter);

        Status st = scanner->open();
        if (!st.ok()) {
            state.SkipWithError(("Failed to open JsonScanner: " + st.to_string()).c_str());
            break;
        }

        int64_t total_rows = 0;
        while (true) {
            auto res = scanner->get_next();
            if (res.status().is_end_of_file()) {
                break;
            }
            if (!res.ok()) {
                state.SkipWithError(("Failed to scan chunk: " + res.status().to_string()).c_str());
                break;
            }
            auto chunk = res.value();
            if (chunk) {
                total_rows += chunk->num_rows();
            }
        }
        scanner->close();
        benchmark::DoNotOptimize(total_rows);
    }

    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kNumRows);
}

static void BM_JsonScanner(benchmark::State& state) {
    run_json_scanner_benchmark(state, g_json_file_path);
}

static void BM_JsonScanner_UnicodeEscapes(benchmark::State& state) {
    run_json_scanner_benchmark(state, g_json_unicode_file_path);
}

static void BM_JsonScanner_NumberHeavy(benchmark::State& state) {
    run_json_scanner_benchmark(state, g_json_number_file_path);
}

static void BM_JsonScanner_Icelake(benchmark::State& state) {
    run_json_scanner_benchmark(state, g_json_file_path, "icelake");
}

static void BM_JsonScanner_Haswell(benchmark::State& state) {
    run_json_scanner_benchmark(state, g_json_file_path, "haswell");
}

static void BM_JsonScanner_Westmere(benchmark::State& state) {
    run_json_scanner_benchmark(state, g_json_file_path, "westmere");
}

static void BM_JsonScanner_WideRecord_Icelake(benchmark::State& state) {
    run_json_scanner_benchmark(state, g_json_wide_file_path, "icelake");
}

static void BM_JsonScanner_WideRecord_Haswell(benchmark::State& state) {
    run_json_scanner_benchmark(state, g_json_wide_file_path, "haswell");
}

static void BM_JsonScanner_WideRecord_Westmere(benchmark::State& state) {
    run_json_scanner_benchmark(state, g_json_wide_file_path, "westmere");
}

#define STARROCKS_JSON_BENCH(name)     BENCHMARK(name)->Unit(benchmark::kMillisecond)->Iterations(10)->Repetitions(7)->ReportAggregatesOnly(true)
STARROCKS_JSON_BENCH(BM_JsonScanner);
STARROCKS_JSON_BENCH(BM_JsonScanner_UnicodeEscapes);
STARROCKS_JSON_BENCH(BM_JsonScanner_NumberHeavy);
STARROCKS_JSON_BENCH(BM_JsonScanner_Icelake);
STARROCKS_JSON_BENCH(BM_JsonScanner_Haswell);
STARROCKS_JSON_BENCH(BM_JsonScanner_Westmere);
STARROCKS_JSON_BENCH(BM_JsonScanner_WideRecord_Icelake);
STARROCKS_JSON_BENCH(BM_JsonScanner_WideRecord_Haswell);
STARROCKS_JSON_BENCH(BM_JsonScanner_WideRecord_Westmere);
#undef STARROCKS_JSON_BENCH
BENCHMARK(BM_ArrowScanner)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ArrowRoutineLoadPipeScanner)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ArrowRoutineLoadPipeScannerLz4)->Unit(benchmark::kMillisecond);

} // namespace starrocks

int main(int argc, char** argv) {
    starrocks::setup_global_files();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
        starrocks::cleanup_global_files();
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    starrocks::cleanup_global_files();
    return 0;
}
