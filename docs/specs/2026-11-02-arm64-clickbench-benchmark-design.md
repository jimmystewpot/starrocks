# Design Specification: StarRocks ARM64 ClickBench Performance Evaluation & Automated Harness

- **Date**: 2026-11-02
- **Author**: Jimmy Lamb (@jimmystewpot), AI Pair Programming Team
- **Target Platform**: ARM64 / AArch64 (Google Cloud C4A Axion / Arm Neoverse-V2, AWS Graviton4)
- **Status**: Ready for Approval

---

## 1. Intent & Executive Summary

Over recent months, extensive performance optimizations have been developed for StarRocks on ARM64. These changes span SIMD filtering, Parquet decoding, BitShuffle vectorization, string algorithms, hash aggregation, hardware CRC-32C/PMULL folding, SwissTable group hashing, and compression library upgrades.

The goal of this project is to measure and quantify the aggregate benefits of this entire suite of improvements on ARM64 using the industry-standard **ClickHouse ClickBench** suite (100 million rows, 43 queries) on Google Compute Engine **C4A** (Google Axion / Neoverse-V2) instances.

To evaluate future improvements cleanly and detect any query-specific regressions, the benchmarking matrix is anchored on upstream `main`:
1. **Upstream Baseline (`main-baseline`)**: Clean upstream `main` compiled with standard flags (`-march=armv8-a+crc`).
2. **Release Reference (`4.1.7-official`)**: Official StarRocks 4.1.7 release binary tarball from the StarRocks release CDN.
3. **Consolidated Candidate (`main-custom-default`)**: `main` merged with all pending/open performance PR branches, built with matching baseline compiler flags (`-march=armv8-a+crc`).
4. **Consolidated Candidate + LSE (`main-custom-lse`)**: The same consolidated candidate compiled with ARM Large System Extensions (`-march=armv8-a+crc+lse` / `-mcpu=neoverse-v2`) to quantify hardware atomics vs. LL/SC loops.

To ensure comprehensive workload coverage, the suite executes both:
- **Standard ClickBench**: Native StarRocks `DUPLICATE KEY` table loaded via TSV (evaluating scanning, SIMD filtering, string lengths, regex replacement, hash aggregation, and group hashing).
- **StarRocks Parquet ClickBench**: Direct querying of Parquet files via `FILES()` (actively evaluating Parquet Bloom filter, dictionary null scatter, and FLBA plain decoding).

---

## 2. Goals, Non-Goals, and Anti-Goals

### Goals
- **Empirical Quantification**: Measure cold and hot query latencies across all 43 ClickBench queries and data ingest throughput.
- **Scientific Control**:
  - Isolate the net impact of the 20+ performance PRs against clean upstream `main`.
  - Quantify the macro gain of the candidate build against the current production release (4.1.7).
  - Isolate the specific benefit of compiler-emitted LSE atomics (`ldadd`, `cas`) on Axion's Neoverse-V2 multi-core interconnect.
- **Regression Detection**: Highlight any query exhibiting a regression (> +10%) so it can be isolated and added to the backlog.
- **Automated & Repeatable Harness**: Extend [`ClickBench/starrocks-versions`](file:///home/jalamb/go/src/github.com/jimmystewpot/ClickBench/starrocks-versions) so future branches can be benchmarked with a single command.

### Non-Goals
- **Replacing CI/CD**: Not intended for every commit; aimed at release benchmarking and architectural validation.
- **Distributed Cluster Benchmarking**: Initial phase focuses on single-node ClickBench to align with published cloud leaderboard profiles.

### Anti-Goals
- **Container Network Overhead**: The benchmark will not run inside Docker; tests run on bare Linux host OS with direct memory and I/O access to eliminate virtualization noise.

---

## 3. Hardware & Instance Specification

### Google Cloud C4A (Google Axion)
- **CPU**: Google Axion (Arm Neoverse-V2 cores, Armv9.0-A architecture).
- **Physical Cores**: 1 vCPU = 1 physical core (no SMT/hyperthreading resource sharing).
- **Vector Units**: 4x128-bit vector pipelines per core supporting Advanced SIMD (NEON) and SVE2.
- **Hardware Atomics**: Native support for Large System Extensions (LSE).

### Sizing Strategy
- **Iteration / Smoke Runs**: **`c4a-standard-8` (8 vCPU, 32 GB RAM)** for fast iteration and harness debugging.
- **Official Publishable Runs**: **`c4a-standard-16` (16 vCPU, 64 GB RAM)** for direct 1:1 hardware comparability against AWS Graviton4 `c8g.4xlarge` and AMD `c6a.4xlarge`.

---

## 4. Performance PR & Patch Inventory (Consolidation Target)

The consolidated candidate branch `perf/arm64-aggregate-rc` integrates the following performance branches and third-party optimization patches:

### A. Core SIMD & Expression Vectorization
1. **PR #81467** (`feat/arm-neon-filter-selector`): BinaryColumn::filter_range and 8-byte SIMD_muti_selector ARM NEON fast paths.
2. **PR #82085** (`feat/arm-filter-neon-sve2`): ARM64 SIMD::Filter::filter_range 4B/8B stream compaction with NEON `vqtbl1q_u8` table lookup.
3. **PR #82539** (`feat/x86-filter-avx2-permute`): Vectorized SIMD stream compaction with AVX2 table-lookup permute and safe boundary/fallback gates.
4. **PR #81482** (`fix/x86-avx2-mulselector-filter-bugs`): Fixes AVX2 SIMD_muti_selector multi-byte regression, uninitialised reads, and signed filter mask bugs.
5. **PR #82018** (`perf/simd-count-zero-mask-strlen`): Multi-architecture SIMD optimisations for `count_zero`, `AppendWithMask`, and vectorized string length filtering.
6. **PR #82006** (`perf/simd-substring-search`): Multi-arch SIMD substring search for `LIKE '%pattern%'` and `locate()` with hoisted state.
7. **PR #82029** (`perf/regexp-replace`): Zero-copy BinaryColumn materialisation and template pre-compilation in `StringFunctions::regexp_replace` (ClickBench Q28).

### B. Storage, Column & Page Decoding
8. **PR #82012** (`perf/parquet-page-decode-simd-parity`): ARM64 NEON SIMD implementations for Parquet and page decoding (Block Split Bloom Filter, dictionary null scatter, FLBA plain decode).
9. **PR #82015** (`fix/parquet-page-decode-simd-bugs`): Fixes signed overflow UB in delta decode, BatchRunCounter signed comparison bug, and Parquet bloom filter mask fusion.
10. **PR #75981** (`bugfix/parquet-missing-column-null-append`): Fixes null-padding size for missing columns in ParquetScanner.
11. **PR #80190** (`perf/pb1-bitshuffle-fast-path`): Direct write optimization into destination column in `BitShufflePageDecoder::read_by_rowids`.
12. **PR #82543** (`perf/pg2-gather-prefetch`): Prefetch large-table lookups in `SIMDGather` for hash join probes (unrolled default loop, architecture-gated prefetch, caller pool-size hints).

### C. Hash Tables, Aggregations, Sorting & Group Hashing
13. **PR #78757** (`feat/arm64-hashutil-crc-dispatch`): Hardware CRC-32C dispatch for ARM64 using ACLE intrinsics.
14. **PR #79030** (`pr/arm64-crc32c-scalar-and-cpuinfo`): Optimized ARM64 Fast_CRC32 scalar throughput using `__crc32cd`.
15. **PR #79031** (`pr/arm64-crc32c-pmull-parallel-fold`): 64-byte PMULL parallel folding for CRC32C on ARM64 with runtime CPU capability detection.
16. **PR #82420** (`perf/track7-xxh3-inline-arm64-crc`): Inlined columnar XXH3 and ARM64 hardware IEEE CRC-32 acceleration follow-ups.
17. **PR #81384** (`neon-phmap-group`): ARM NEON `GroupAArch64Impl` for phmap SwissTable group probing.
18. **PR #82007** (`perf/string-hash-agg-memcmp`): String CRC hashing, SliceAggHashSet slot layout, and SIMD/big-endian string comparisons.
19. **PR #81383** (`neon-memequal-padded`): Vectorized `memequal_padded` for ARM NEON.
20. **PR #82119** (`fix/agg-int32-two-level-and-fixed-size-stride`): Fixes missing int32 two-level conversion and FixedSizeSliceAggHashMap noprefetch stride corruption.
21. **PR #82131** (`perf/topn-chunks-sorter-fastpath`): Top-N batch filtering optimization and widened ARM64 64-bit column comparisons.
22. **PR #81472** (`feat/simd-radix-sort`): Accelerated fixed-length column sorting with SIMD 8-bit LSD radix sort.
23. **PR #78737** (`enhancement/arm64-primary-key-encoder`): ARM64 NEON acceleration for `PrimaryKeyEncoder`.

### D. Third-Party Library Modernization
24. **PR #81294** (`enhancement/upgrade_croaring_5_3_2`): Upgrades CRoaring to v5.3.2 with runtime AVX-512/AVX2/SVE2 dispatch.
25. **PR #82542** (`upgrade_simdjson_avx512`): Upgrades simdjson to v5.1.1 with runtime AVX-512 dispatch.
26. **PR #79658** (`feature/vectorscan-5.4.13-x86-migration`): Unified regex backend with Vectorscan 5.4.13 across all architectures.
27. **PR #78457** / **PR #78462** (`enhancement/arm_libdeflate` / `chore/update_libdeflate`): Libdeflate 1.26 upgrade and enablement for GZIP/ORC decompression on AArch64.
28. **PR #78566** (`chore/upgrade_snappy_1.2.1`): Snappy 1.2.1 upgrade for ARM and x86-64 SIMD performance.
29. **PR #78568** (`chore/upgrade_simdutf_9.1.0`): Simdutf 9.1.0 upgrade.
30. **PR #79104** (`enhancement/upgrade_fast_float_8_2_10`): Fast_float 8.2.10 upgrade.
31. **PR #79033** (`enhancement/arm64-thirdparty-march-crc`): Global `-march=armv8-a+crc` thirdparty build flags.
32. **PR #79278** (`feat/arm64-cpuinfo-detection`): Comprehensive ARM64 CPU feature detection in `CpuInfo`.

### E. Frontend, Query Pipeline & Concurrency Tooling
33. **PR #77471** (`feature/rg-copy-on-write-map`): Zero-Allocation Resource Group Classification (eliminates `ReentrantReadWriteLock` contention on query admission).
34. **PR #76656** (`starrocks_arrow_ipc_routine_load`): Apache Arrow IPC format in Routine Load.
35. **PR #80310** (`tool/uncap-be-ut-parallelism`): RAM- and CFS quota-aware dynamic build parallelism.

### F. Upstream Bitshuffle Acceleration Patches (PR #186 & #187)
Integrated via `thirdparty/patches/bitshuffle-0.5.1-arm64-neon-opt.patch` and applied during `thirdparty/download-thirdparty.sh`:
36. **Bitshuffle PR #186 (`kiyo-masui/bitshuffle#186`)**:
   - Eliminates 128-byte stack spills in `bshuf_trans_byte_bitrow_NEON` using `vst1q_lane_s64` instead of array store pointer casting.
   - Adds dedicated little-endian 1-byte SIMD delta transpose (`bshuf_shuffle_bit_eightelem_NEON`) using `uint64x2_t` SIMD instructions, accelerating tinyint and boolean column un-transposition.
37. **Bitshuffle PR #187 (`kiyo-masui/bitshuffle#187`)**:
   - Introduces `bshuf_untrans_bit_elem_with_buf` and per-ISA entrypoints (`scal`, `NEON`, `SSE`, `AVX2`, `AVX512`).
   - Directly reuses output buffers during LZ4 and ZSTD block decompression, eliminating intermediate buffer allocation and memcpy steps.
   - Introduces an 8KB stack buffer fallback (`uint64_t stack_buf[8192 / 8]`) for page untransposition.
   - **ClickBench Architectural Impact**: StarRocks native storage uses BitShuffle + LZ4 encoding by default for all integer, timestamp, and boolean columns. For standard <=8KB data pages, this patch eliminates **2 `malloc` and 2 `free` calls per decompressed page** (from 2 down to 0). On Google Axion / Neoverse-V2 with 8-16 concurrent scan threads, this completely removes memory allocator lock contention (`ptmalloc`/`jemalloc`) across scan workers during ClickBench Q0–Q42.

---

## 5. Benchmarking Evaluation Matrix

| ID | Label | Source Tree | Compiler Flags | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **M1** | `main-baseline` | Upstream `main` (merge base) | `-march=armv8-a+crc` | Scientific control baseline |
| **M2** | `4.1.7-official` | Official release CDN | Release default | Production release reference |
| **M3** | `main-custom-default` | `perf/arm64-aggregate-rc` | `-march=armv8-a+crc` | Isolates aggregate PRs + Bitshuffle NEON/8KB buffer patches |
| **M4** | `main-custom-lse` | `perf/arm64-aggregate-rc` | `-march=armv8-a+crc+lse` | Isolates ARM Large System Extensions over M3 |
| **M5** | `main-custom-neoverse` | `perf/arm64-aggregate-rc` | `-mcpu=neoverse-v2` | (Optional) Evaluates SVE2 and V2 pipeline scheduling |

> **Note on Third-Party Patches**: Candidate builds M3 and M4 include the external Bitshuffle PR #186 & PR #187 patch (`thirdparty/patches/bitshuffle-0.5.1-arm64-neon-opt.patch`). If isolating the exact speedup from the Bitshuffle patches alone is required (ablation analysis), a variant `main-custom-no-bitshuffle-opt` can be built by omitting the patch in `thirdparty/download-thirdparty.sh`.

---

## 6. Pipeline Architecture & Components

Located in `ClickBench/starrocks-versions/`:

```
ClickBench/starrocks-versions/
├── package-artifact.sh       # Consolidates branches, compiles in dev-env, packages tarballs
├── run-version.sh           # Installs tarball, configures FE/BE, executes test matrix
├── compare.py               # Compares baseline vs target JSON results (percentages, geometric mean)
└── results/                 # Stores output JSON metrics per run
```

### Component 1: `package-artifact.sh`
- Creates integration branch `perf/arm64-aggregate-rc` from upstream `main` and merges all performance branches.
- Launches official StarRocks Docker builder container (`ghcr.io/starrocks/starrocks/dev-env-ubuntu:main`) to compile FE and BE with pre-built thirdparty libraries:
  - Build Variant A: standard flags (`-march=armv8-a+crc`).
  - Build Variant B: LSE flags (`-march=armv8-a+crc+lse`).
- Packages output into `StarRocks-${VERSION}-ubuntu-arm64.tar.gz` and registers in `work/${VERSION}/`.

### Component 2: `run-version.sh`
- For each version in the matrix:
  1. Sets up dedicated `meta/` and `storage/` directories.
  2. Injects configuration:
     - `disable_storage_page_cache = true`
     - `datacache_enable = false`
     - `priority_networks = 127.0.0.1/32`
  3. Starts FE and BE, waits for backend initialization.
  4. Runs **Suite A (Native ClickBench)**: creates `hits` table, loads TSV via `starrocks/load`, records load time and disk size, runs Q0–Q42 (1 cold run with OS cache drop + 2 hot runs).
  5. Runs **Suite B (Parquet ClickBench)**: creates view over `hits.parquet`, runs queries to benchmark Parquet SIMD decoding.
  6. Gracefully stops daemons and generates `results/${VERSION}.json`.

### Component 3: `compare.py`
- Parses baseline and candidate JSON files.
- Computes per-query delta percentages for Cold and Hot runs.
- Computes overall Geometric Mean speedup ratios for Cold and Hot runs.
- Highlights improvements (< -10%) and regressions (> +10%) in Markdown format.

---

## 7. System & Runtime Configuration

- **Memory Allocation**:
  - `c4a-standard-8`: `query_mem_limit = 21474836480` (20 GB BE, 8 GB FE JVM, 4 GB OS).
  - `c4a-standard-16`: `query_mem_limit = 42949672960` (40 GB BE, 16 GB FE JVM, 8 GB OS).
- **Execution Concurrency**: `pipeline_dop = 0` (auto-tuned to physical core count).
- **Cache Policy**: Explicit OS cache dropping (`sync; echo 3 | sudo tee /proc/sys/vm/drop_caches`) prior to cold run for every query.

---

## 8. What's Left to the Builder

1. Implementation of `package-artifact.sh` with automated branch merging, Docker dev-env invocation, and tarball packaging.
2. Modification of `run-version.sh` to support executing both native and Parquet suites.
3. Addition of Geometric Mean calculation to `compare.py`.
4. Execution of the preliminary run on `c4a-standard-8` and generation of the initial comparative report.
