# bitmap_raw performance comparison — 2026-09-17

The packed DAT plus direct reverse DP improves steady-state segmentation and
lookup throughput, and reduces dictionary-load RSS. Initialization becomes
3.62 times as slow: **188.785 ms → 682.851 ms**. This tradeoff is a release gate;
passing correctness checks does not establish that the new default is suitable
for applications that frequently construct a tokenizer.

## Measured results

Seven fresh processes per implementation, alternating their execution order by
round, pinned to guest CPU 0. Values below are medians. Baseline is master commit
`103e1a2c9f06d80705d261ab1cd66bc48c090abd`; optimized is this feature branch.

| Metric | Baseline pointer Trie | bitmap_raw + ring DP | Change |
|---|---:|---:|---:|
| Dictionary initialization | 188.785 ms | 682.851 ms | 3.62× time (+494.066 ms) |
| RSS after dictionary load | 125.418 MiB | 88.738 MiB | −29.2% |
| RSS increase during dictionary load | 119.734 MiB | 83.055 MiB | −30.6% |
| MP throughput | 36.383 MiB/s | 96.868 MiB/s | 2.66× |
| MP elapsed, 200 documents | 4277.526 ms | 1606.629 ms | −62.4% |
| Mix throughput | 29.316 MiB/s | 64.883 MiB/s | 2.21× |
| Mix elapsed, 200 documents | 5308.777 ms | 2398.635 ms | −54.8% |
| Dictionary lookup | 22.077 million/s | 34.145 million/s | 1.55× |
| HMM initialization | 5.872 ms | 5.483 ms | HMM algorithm unchanged |

The corpus is `test/testdata/synthetic_doc.utf8`, 815,952 bytes, segmented 200
times per mode. The lookup test queries the first 50,000 main-dictionary entries
20 times. Both versions produce 179,022 MP tokens, 165,690 Mix tokens, and
1,000,000 successful lookups in every sample. Values named `mb_per_sec`/`rss_mb`
in the program use 1,048,576 bytes per unit, hence MiB in this report.

The optimized model has 497,995 logical nodes, 802,401 slots and 5,087 unique
weights. Units occupy 6,419,208 bytes; weights and the unknown-word weight bring
the packed payload to **6,459,912 bytes (6.161 MiB)**. The separate metadata index
and DictUnit pointer table occupy 6,001,484 bytes (5.723 MiB). Original words/tags,
cache, allocator state and temporaries account for further memory; the 6.161 MiB
payload is not the process RSS.

## Conditions and scope

- CPU reported by the WSL2 guest: Intel Core i7-13700; Linux x86-64;
  GCC 11.4.0, CMake Release, C++11.
- Both binaries use `-O3 -DNDEBUG -O3 -g -std=c++11`, the repository's effective
  Release flags. They use byte-identical dictionaries, model and corpus.
- Each process starts with an empty in-process dictionary cache. Input files may
  already be cached by the OS. This is fresh-process initialization, not cold-disk I/O.
- The benchmark reads the corpus and prepares lookup strings before starting
  dictionary timing/RSS measurement. Segmentation time excludes dictionary/HMM loading.
- No benchmark competitors, builds or test workloads were launched during the
  seven paired rounds. Guest CPU affinity does not isolate the host scheduler.
- This measures the combined implementation: bitmap DAT, direct candidate/ring
  DP and removal of redundant load-time metadata copies. It is not an ablation
  attributing all gains exclusively to the DAT layout.
- One synthetic corpus and successful dictionary lookups do not characterize
  every workload. HMM-heavy inputs, miss-heavy lookup and repeated construction
  can have different tradeoffs.

[Raw samples](2026-09-17-bitmap-raw.json) include all 14 stdout/stderr records,
medians, compiler flags, input and binary SHA-256 hashes and environment details.

## Correctness evidence

- Baseline CTest: 2/2 targets pass. Optimized CTest: 2/2 targets pass, including
  48 unit tests. New tests cover raw Unicode transitions, negative bases, root
  alias exclusion, 64-bit bitmap shifts, duplicates, signed-zero weights,
  failed-build rollback, field-limit fallback, ring ties/limits, runtime user
  edits, retained pointers and DAG reuse.
- ASan + UBSan with leak detection: all 19 focused DAT/adapter/DictTrie tests pass.
- Four changed/new public headers compile independently under Clang C++11.
  The unchanged legacy `Trie.hpp` reports its existing unused-parameter warning.
- Exhaustive current-dictionary differential: 348,985 input records, exact
  weight bits/word Runes/POS tags, plus one negative lookup per record.
  Dictionary checksum is `8897672697622394244` in both executables.
- Token differential: 9,928 corpus/generated texts, five segmentation modes,
  byte/Unicode offsets and POS tags. Token checksum is
  `16338862599687085928` in both executables.
- The supplied archive uses a different snapshot, including the extra startup
  user word `韩玉鉴赏`. Rebuilding that snapshot gives 497,996 nodes and 802,401
  slots; all units and all C++ FP64 weights compare byte-for-byte with the
  archived Python model on this platform. The current benchmark uses the
  repository dictionaries, not the archived snapshot.
- Independent design-compliance and code-quality reviews found no blocking issues.

## Reproduction

Build the baseline at the stated commit in a separate worktree, then build this
branch with the same settings. Replace paths below with the two checkouts:

```sh
cmake -S /path/to/baseline -B /path/to/baseline/build -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/baseline/build --target benchmark_bin -j4
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target benchmark_bin -j4
python3 scripts/compare_benchmarks.py \
  /path/to/baseline/build/benchmark_bin build/benchmark_bin \
  --rounds 7 --cpu 0 --output /tmp/bitmap-comparison.json
```

See the [implementation note](../bitmap-raw.md) for the archive and differential
check commands. The externally supplied design archive is not a runtime dependency
and is not copied into this commit.

## Release assessment

No release/tag/version bump is made. The implementation is validated for the
measured segmentation behavior and provides a substantial steady-state gain,
but initialization regresses by about 494 ms per fresh dictionary object.
Before changing a published default, evaluate this cost against downstream
construction patterns or add an explicitly designed prebuilt-model loading path.
The current implementation builds the layout from text; it does not implement
the archive's production binary/manifest loader. Runtime metadata and mutable
user-word compatibility are preserved outside the compact model budget.
