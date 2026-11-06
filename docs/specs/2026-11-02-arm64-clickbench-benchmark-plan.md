# StarRocks ARM64 ClickBench Benchmark & Evaluation Harness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build, automate, and execute an empirical ARM64 ClickBench benchmarking pipeline evaluating StarRocks upstream `main` baseline, 4.1.7 official release, consolidated custom candidate (35 PRs + Bitshuffle NEON/8KB buffer patches), and custom candidate with LSE on Google Cloud C4A Axion instances.

**Architecture:** A native VM execution pipeline based on `ClickBench/starrocks-versions`. Uses StarRocks `dev-env-ubuntu:main` container on the C4A VM strictly for native compiling and tarball generation, then executes benchmarks directly on the bare Linux host OS with OS-level cache drops, capturing timing metrics across native and Parquet suites into structured JSON diffs via `compare.py`.

**Tech Stack:** Bash, Python 3, StarRocks C++ Backend / Java Frontend, CMake, MySQL client, Google Cloud SDK (`gcloud`), ARM64 (Google Axion / Neoverse-V2).

**Spec:** [`docs/specs/2026-11-02-arm64-clickbench-benchmark-design.md`](file:///home/jalamb/go/src/github.com/jimmystewpot/starrocks/docs/specs/2026-11-02-arm64-clickbench-benchmark-design.md)

---

## Global Constraints

- Platform: Linux AArch64 (ARM64), specifically Google Compute Engine C4A (Google Axion).
- Baseline reference: Upstream `main` at common merge-base and StarRocks 4.1.7 official CDN tarball.
- Target branch: `perf/arm64-aggregate-rc` integrating all 35 performance branches + Bitshuffle patches.
- Compiler flags: Baseline (`-march=armv8-a+crc`); LSE candidate (`-march=armv8-a+crc+lse`).
- ClickBench methodology: 100M rows, 43 queries (Q0–Q42), 1 cold run with OS cache drop + 2 hot runs.
- Execution environment: Bare host OS (no Docker runtime overhead during benchmark execution).

---

## Review Focus

1. **Git Merge Conflicts during Consolidation**: Merging all 35 branches onto `main` must resolve cleanly without dropping localized SIMD dispatch paths or introducing compilation regressions.
2. **BE Startup on C4A**: Verify BE passes `CpuInfo` checks and registers successfully with FE (`backend_alive`) before running queries.
3. **OS Page Cache Drop Rights**: Ensure `sync; echo 3 | sudo tee /proc/sys/vm/drop_caches` functions properly on C4A VM for true cold-query timing.
4. **Memory Sizing**: Avoid OOM on 100M-row TSV load by enforcing appropriate `query_mem_limit` and JVM heap bounds.
5. **Parquet Suite Execution**: Ensure `FILES()` queries in `starrocks-parquet` resolve local file paths correctly and produce non-empty result sets.

---

## Implementation Tasks

### Task 1: Integration Branch Consolidation (`perf/arm64-aggregate-rc`)

**Files:**
- Create/Branch: `perf/arm64-aggregate-rc` from `upstream/main`
- Patch: `thirdparty/patches/bitshuffle-0.5.1-arm64-neon-opt.patch`
- Script: `thirdparty/download-thirdparty.sh`
- Verify: BE compilation and unit tests on changed files

- [x] **Step 1.1**: Fetch latest `upstream/main` and create clean integration branch `perf/arm64-aggregate-rc`.
- [x] **Step 1.2**: Incrementally merge core SIMD and filter branches:
  - `feat/arm-neon-filter-selector` (PR #81467)
  - `feat/arm-filter-neon-sve2` (PR #82085)
  - `feat/x86-filter-avx2-permute` (PR #82539)
  - `fix/x86-avx2-mulselector-filter-bugs` (PR #81482)
  - `perf/simd-count-zero-mask-strlen` (PR #82018)
  - `perf/simd-substring-search` (PR #82006)
  - `perf/regexp-replace` (PR #82029)
- [x] **Step 1.3**: Merge storage and page decode branches:
  - `perf/parquet-page-decode-simd-parity` (PR #82012)
  - `fix/parquet-page-decode-simd-bugs` (PR #82015)
  - `bugfix/parquet-missing-column-null-append` (PR #75981)
  - `perf/pb1-bitshuffle-fast-path` (PR #80190)
  - `perf/pg2-gather-prefetch` (PR #82543)
- [x] **Step 1.4**: Merge hash tables, aggregations, and hardware CRC branches:
  - `feat/arm64-hashutil-crc-dispatch` (PR #78757)
  - `pr/arm64-crc32c-scalar-and-cpuinfo` (PR #79030)
  - `pr/arm64-crc32c-pmull-parallel-fold` (PR #79031)
  - `perf/track7-xxh3-inline-arm64-crc` (PR #82420)
  - `neon-phmap-group` (PR #81384)
  - `perf/string-hash-agg-memcmp` (PR #82007)
  - `neon-memequal-padded` (PR #81383)
  - `fix/agg-int32-two-level-and-fixed-size-stride` (PR #82119)
  - `perf/topn-chunks-sorter-fastpath` (PR #82131)
  - `feat/simd-radix-sort` (PR #81472)
  - `enhancement/arm64-primary-key-encoder` (PR #78737)
- [x] **Step 1.5**: Merge third-party updates, query pipeline enhancements, and integrate Bitshuffle patches:
  - `enhancement/upgrade_croaring_5_3_2` (PR #81294)
  - `upgrade_simdjson_avx512` (PR #82542)
  - `feature/vectorscan-5.4.13-x86-migration` (PR #79658)
  - `chore/update_libdeflate` & `enhancement/arm_libdeflate` (PR #78462 / #78457)
  - `chore/upgrade_snappy_1.2.1` (PR #78566)
  - `origin/chore/upgrade_simdutf_9.1.0` (PR #78568)
  - `origin/enhancement/upgrade_fast_float_8_2_10` (PR #79104)
  - `origin/enhancement/arm64-thirdparty-march-crc` (PR #79033)
  - `feature/rg-copy-on-write-map` (PR #77471)
  - `starrocks_arrow_ipc_routine_load` (PR #76656)
  - `tool/uncap-be-ut-parallelism` (PR #80310)
  - Bitshuffle PR #186 & PR #187 (`thirdparty/patches/bitshuffle-0.5.1-arm64-neon-opt.patch`)
- [x] **Step 1.6**: Run `git status` and verify git log tree and clean multi-ISA builds.

---

### Task 2: Artifact Packaging Script (`package-artifact.sh`)

**Files:**
- Create: `/home/jalamb/go/src/github.com/jimmystewpot/ClickBench/starrocks-versions/package-artifact.sh`

- [x] **Step 2.1**: Implement `package-artifact.sh` supporting:
  - `--branch <git-ref>`
  - `--version-tag <name>` (e.g. `main-baseline`, `main-custom-default`, `main-custom-lse`)
  - `--cflags <flags>` (e.g. `-march=armv8-a+crc` vs `-march=armv8-a+crc+lse`)
  - `--use-docker` to wrap compilation inside `ghcr.io/starrocks/starrocks/dev-env-ubuntu:main`.
- [x] **Step 2.2**: Include packaging logic that tars `fe/`, `be/`, and dependencies into `StarRocks-${VERSION}-ubuntu-arm64.tar.gz`.
- [x] **Step 2.3**: Verify script in `--dry-run` mode.

---

### Task 3: Dual-Suite Support in Runner (`run-version.sh`)

**Files:**
- Modify: `/home/jalamb/go/src/github.com/jimmystewpot/ClickBench/starrocks-versions/run-version.sh`

- [ ] **Step 3.1**: Add support for running both `starrocks` (native TSV table) and `starrocks-parquet` (Parquet queries).
- [ ] **Step 3.2**: Configure port safety checks (killing stale processes on 9030, 9050, 8040, 8030).
- [ ] **Step 3.3**: Ensure cold run cache drops (`sync; echo 3 | sudo tee /proc/sys/vm/drop_caches`) execute gracefully.
- [x] **Step 3.4**: Test execution with `./run-version.sh --dry-run 4.1.7` to verify synthetic output generation.

---

### Task 4: Enhance Comparison Utility (`compare.py`)

**Files:**
- Modify: `/home/jalamb/go/src/github.com/jimmystewpot/ClickBench/starrocks-versions/compare.py`
- Test: `/home/jalamb/go/src/github.com/jimmystewpot/ClickBench/starrocks-versions/tests/test_compare.py`

- [x] **Step 4.1**: Add calculation of overall **Cold Geometric Mean** and **Hot Geometric Mean** speedups across all 43 queries.
- [x] **Step 4.2**: Format output in GitHub-flavored Markdown tables with `+` and `-` percentage changes and visual status indicators (Improvement / Regression).
- [x] **Step 4.3**: Run `pytest /home/jalamb/go/src/github.com/jimmystewpot/ClickBench/starrocks-versions/tests/test_compare.py` to ensure unit test pass.

---

### Task 5: Provisioning Runbook & Smoke Test on C4A (`c4a-standard-8`)

**Files:**
- Create: `ClickBench/starrocks-versions/C4A-RUNBOOK.md`

- [ ] **Step 5.1**: Document exact `gcloud compute instances create` command for `c4a-standard-8` (Ubuntu 24.04 ARM64, 100GB Hyperdisk Balanced).
- [ ] **Step 5.2**: Build and execute baseline `main-baseline` and candidate `main-custom-default` smoke runs on 10M / 100M rows.
- [ ] **Step 5.3**: Run `compare.py` on smoke outputs to verify automated metrics pipeline.

---

### Task 6: Execution of Full Evaluation Matrix on `c4a-standard-16`

- [ ] **Step 6.1**: Execute Matrix-1: `4.1.7-official` (downloaded CDN release).
- [ ] **Step 6.2**: Execute Matrix-2: `main-baseline` (clean upstream merge-base).
- [ ] **Step 6.3**: Execute Matrix-3: `main-custom-default` (`perf/arm64-aggregate-rc`, baseline flags).
- [ ] **Step 6.4**: Execute Matrix-4: `main-custom-lse` (`perf/arm64-aggregate-rc`, `+lse` flags).
- [ ] **Step 6.5**: Run `compare.py` comparing:
  - `main-baseline` vs `main-custom-default` (isolating PRs).
  - `main-custom-default` vs `main-custom-lse` (isolating LSE).
  - `4.1.7-official` vs `main-custom-lse` (total release delta).
- [ ] **Step 6.6**: Publish final benchmark analysis report into `docs/specs/2026-11-02-arm64-clickbench-benchmark-results.md`.
