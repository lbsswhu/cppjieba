# Bitmap raw main dictionary implementation plan

> Execute the supplied `dat_optimization/02_bitmap_raw/DESIGN.md` in this session; use subagent-driven development for the independent DAT core and independent reviews.

**Goal:** Replace the main dictionary pointer trie with the supplied raw-Rune bitmap-first-fit packed DAT and compare against master 103e1a2.

**Architecture:** Preserve C++11, DictUnit pointers, POS tags and dynamic user-word APIs. Build the packed static main/startup dictionary with exact C++ FP64 weights; store an auxiliary slot-to-input-id table for existing metadata APIs. A small mutable overlay handles later inserts and exact-word deletions. MP segmentation directly enumerates weighted candidates and uses a bounded score ring, preserving shortest-first strict-greater tie behavior. Keep the legacy Trie as fallback when a custom dictionary exceeds packed field limits.

**Scope:** In-memory construction from existing text dictionaries. The supplied archived model is a verification oracle, not a production runtime dependency. The packed 8-byte units and weights are measured separately from API compatibility metadata, HMM and process RSS. No version bump or release before evaluating compatibility and benchmark results.

## Tasks

- [x] Baseline: build Release from master; run CTest; preserve benchmark binary and input hashes.
- [x] Core (`include/cppjieba/BitmapDoubleArrayTrie.hpp`, `test/unittest/bitmap_dat_test.cpp`): write tests for raw Rune transitions, negative offsets, terminal prefixes, duplicate last-wins, exact weights, empty/root behavior, shifted bitmap word boundaries and packed limits. Verify failures, then implement global degree/span/id ordering, 65536-slot growth and lowest feasible bitmap anchor, packed units, exact bitwise FP64 deduplication, input-id metadata and stats. Confirm archived 802401 slots on the archived inputs.
- [x] Integration (`include/cppjieba/BitmapTrie.hpp`, `DictTrie.hpp`, `MPSegment.hpp`, tests): preserve exact Find/DAG candidates/POS with immutable DAT plus mutable overlay; implement direct reverse DP with bounded ring; preserve zero/one max-word-length semantics, tie ordering, user overrides and HMM boundaries. Add regression tests before edits. Existing Trie remains fallback for packed limits.
- [x] Verification: run all CTest cases, exhaustive dictionary weight/tag checks, compare legacy/new segmentation outputs on corpus and fixed-seed generated mixed UTF-8 text; ASan/UBSan focused tests; standalone header compilation.
- [x] Benchmark: alternate at least five fresh processes per implementation on one CPU using identical Release flags/input; preserve all raw rows and report medians, memory and construction cost, plus packed model versus auxiliary bytes.
- [x] Review: independent spec review then code quality review; fix material findings and revalidate affected paths.
- Delivery: write reproducible results and release assessment, commit complete changes and push feature branch as required by AGENTS.md. Leave master and the supplied untracked archive intact.
