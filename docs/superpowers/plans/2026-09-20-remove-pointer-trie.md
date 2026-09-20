# Remove Pointer Trie implementation plan

> **For agentic workers:** Use superpowers:subagent-driven-development for bounded independent tasks, then review the integrated implementation and measurements.

**Goal:** Remove the original Pointer Trie entirely and compare correctness, memory and performance against dat_merged commit 8b8a3f7.

**Architecture:** All dictionary traversal uses packed raw DAT. A uint32 source-index array indexed by DAT terminal state preserves stable DictUnit lookup and tags using existing dictionary payload ownership. Fused DP supports uint16 and size_t recovery lengths; no PointerWalker or legacy MP fallback remains. HMM implementation is unchanged to isolate the effect of Trie removal.

**Tech stack:** C++11, CMake, Bazel, GoogleTest, independent-process differential and benchmark tools.

- [x] Export 8b8a3f7 into /tmp/cppjieba-dat-merged-before, build Release and run CTest before changes.
- [x] Add terminal source-index metadata to DatModel/DatBuilder and its byte/budget accounting; test duplicate identity independent of shared weights and validation of all effective entries.
- [x] Move DictUnit/Dag/MAX_WORD_LENGTH into DictTypes.hpp; remove Trie.hpp and PointerWalker. Preserve read-compatible Find and DAG enumeration through DAT, add Contains for search. Keep loaded DictUnit payloads for word/tag pointer lifetime.
- [x] Remove LegacyDag/PointerFused modes, make DatRawFused the default. DAT construction failures throw with structured status instead of publishing a partial model or a removed backend. User confirmed unified startup-only read-only dictionaries.
- [x] Replace oversized-L legacy fallback with size_t best lengths in the same fused kernel; retain uint16 for ordinary calls. Test 65535/65536 and SIZE_MAX limits against independent prior binaries.
- [x] Replace old Trie test oracles with direct word-list candidate/DP oracles; adapt fixture tests to the selected mutation contract. Verify every public segmentation, lookup, POS and keyword API against the 8b8a3f7 executable with separate-process exact outputs, not two instances of the same new implementation.
- [x] Benchmark previous default A, previous D (DAT+old Trie+dense HMM), new DAT with identical HMM, using matching inputs/compiler/affinity; preserve raw repeated RSS, cold and warm measurements, include allocator trim diagnostic separately.
- [x] Run Release CTest, ASan/UBSan, Clang C++11, Bazel/install checks appropriate to removed header and linked model. Independently review production diff and measurements.
- [x] Update design/usage/report with explicit compatibility changes, fallback changes and release assessment. Commit and push completed work on dat_merged after validation, honoring existing publication instructions.
