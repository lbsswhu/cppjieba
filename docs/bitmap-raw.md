# Raw-Rune bitmap DAT integration

This change implements the bitmap-first-fit main dictionary layout from the supplied
`dat_optimization/02_bitmap_raw/DESIGN.md`, replacing the default pointer trie in
`DictTrie`. It also replaces MP's materialized DAG and full score array with direct
candidate enumeration and a bounded score ring. HMM segmentation remains unchanged.

## Representation and compatibility

`BitmapDoubleArrayTrie.hpp` constructs an immutable dictionary from the existing
text dictionary inputs. Effective words are ordered lexicographically by Rune;
duplicates keep the last input entry. Logical rows are placed in descending child
count, descending label span, then ascending logical node ID. Construction intersects
64-bit windows of the free-slot bitmap and selects the lowest feasible anchor. The
bitmap grows in 65,536-slot blocks only when no existing anchor fits. Logical nodes
are resolved to their final states after all rows have been placed.

Each runtime Unit contains signed base:22, check:21, weightCode:13, and eight zero
reserved bits. Labels are original Unicode scalar values. A transition computes
`base[state] + rune` using signed wide arithmetic and requires `0 < target < M`
and `check[target] == state`. Terminal state and base sign are independent. FP64
weights are deduplicated by their exact C++ bit patterns and ordered as unsigned
64-bit integers. Failed builds leave the previous tree intact.

`BitmapTrie.hpp` adapts this packed dictionary to cppjieba's interfaces:

- A separate slot-to-input index and pointer table preserve `DictUnit` word/tag
  access, pointer lifetimes, POS tagging and the existing DAG interface.
- Runtime user words live in a small mutable trie. An override wins over the
  static terminal; a tombstone removes only the exact word. Longer words and
  siblings remain available. The immutable packed dictionary is never rebuilt
  for these edits.
- Custom dictionaries that exceed the packed state/base/weight fields fall back
  to the existing pointer Trie. No field is silently truncated.
- MP enumerates candidates shortest first, always supplies a singleton candidate,
  and updates only for a strictly larger score. Its score ring contains at most
  `min(input_length, max(1, min(requested_limit, longest_dictionary_word))) + 1`
  doubles. One chosen length per input Rune is retained for reconstruction.

The integration also corrects three existing adapter behaviors: reusing a DAG
output replaces its candidates, deletion preserves other words with the same
prefix, and runtime `LoadUserDict` actually installs the new words without
invalidating pointers to startup user words. Regression tests cover these cases;
equivalence claims for the legacy implementation exclude these bug fixes.

Dictionary loading sorts only a vector of weights when computing defaults and
moves the final word/tag storage into its cache, eliminating redundant full
`DictUnit` copies.

## Model size versus process memory

`BENCH DATModel` reports `model_bytes` as packed units + unique FP64 weights + the
8-byte unknown-word weight. `metadata_index_bytes` is the auxiliary slot index and
pointer table only. Retained `DictUnit` words/tags, the path-keyed dictionary cache,
user-word markers and overlay, allocator retention, build temporaries, and HMM are
not part of the packed model budget. `rss_mb` and `rss_delta_mb` are process RSS
measurements and must not be interpreted as the packed model size.

This implementation builds from text when `DictTrie` is constructed. It does not
add an on-disk cache or a production binary/manifest loader. The supplied archived
files are a verification oracle. Loading a prebuilt model would be a separate
change with input identity/version/validation and metadata format decisions.

## Reproduce correctness checks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
cmake --build build --target bitmap_differential bitmap_archive_check -j4
./build/bitmap_differential /path/to/input-repository
./build/bitmap_archive_check /path/to/dat_optimization/02_bitmap_raw
```

The optional archive check requires the externally supplied design archive. It
rebuilds using target C++ FP64 math and compares every packed Unit and every weight
bit with the archived little-endian binary files. A different platform's `log`
may differ in the last bit and is reported as a mismatch, never silently rounded.

For baseline comparison, compile the same differential source against the old
checkout and use the same input directory for both executables:

```sh
c++ -std=c++11 -O3 -DNDEBUG -I /path/to/baseline/include \
  test/bitmap_differential.cpp -o /tmp/cppjieba-baseline-check
/tmp/cppjieba-baseline-check /path/to/input-repository
./build/bitmap_differential /path/to/input-repository
```

It checks every dictionary record's existence, exact FP64 weight bits, word Runes
and POS tag, checks a negative lookup for every record, and compares checksum
outputs for corpus text plus 1,000 fixed-seed generated mixed UTF-8 strings.
Five segmentation modes (MP, Mix, full, search without/with HMM), byte/Unicode
offsets and POS results contribute to the token checksum.

See [measured results and release assessment](benchmarks/2026-09-17-bitmap-raw.md)
and [raw samples](benchmarks/2026-09-17-bitmap-raw.json).
