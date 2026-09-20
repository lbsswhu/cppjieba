#!/usr/bin/env python3
"""Compare complete outputs of independent previous and DAT-only processes.

Build test/dat_only_oracle.cpp twice using identical compiler/FP flags. Link
the previous checkout's DatBuilder.cpp with -DCPPJIEBA_CPU_PREVIOUS for the
--previous binary; link the current library for --current. No old Trie is
compiled into the current executable. Successful transcripts are removed
unless --keep-transcripts is given; input fixtures, hashes and report remain.
"""
import argparse
import filecmp
import hashlib
import itertools
import json
import pathlib
import random
import subprocess
import sys


def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(data)
    return value.hexdigest()


def write_inputs(path, texts, lookups=()):
    with path.open("w", encoding="ascii") as output:
        for kind, values in (("T", texts), ("F", lookups)):
            for value in values:
                if isinstance(value, str):
                    value = value.encode("utf-8")
                output.write(kind + " " + value.hex() + "\n")


def require_equal(reference, candidate):
    if filecmp.cmp(reference, candidate, shallow=False):
        return
    with reference.open("rb") as a, candidate.open("rb") as b:
        for number, (expected, actual) in enumerate(itertools.zip_longest(a, b), 1):
            if expected != actual:
                raise RuntimeError(f"output mismatch at transcript line {number}:\n"
                                   f"{reference}: {repr(expected)[:300]}\n"
                                   f"{candidate}: {repr(actual)[:300]}")
    raise RuntimeError("transcripts differ")


def fixtures(args, root):
    rng = random.Random(0xC0FFEE)
    atoms = ["南", "京", "市", "长", "江", "大", "桥", "研究", "生命", "起源", "自然语言",
             "处理", "甲", "龘", "𠀀", "😀", "abc", "3.14", " ", ",", "。", "\n"]
    texts = ["", "小明在南京市长江大桥", "研究生命起源", "北京大学生物系主任", "龘𠀀😀",
             "abc123.45% 中国C++ test@example.com\t\r\n", b"a\x00" + "中国".encode(),
             b"\xff", "中".encode() + b"\xe4\xb8"]
    reviews = (args.repo / "test/testdata/review.100").read_bytes().splitlines()
    texts.extend(reviews[:100])
    texts.extend("".join(rng.choice(atoms) for _ in range(rng.randrange(90)))
                 for _ in range(args.random_texts))
    vocabulary = [line.split(b" ", 1)[0] for line in
                  (args.repo / "dict/jieba.dict.utf8").read_bytes().splitlines() if line]
    if args.all_dictionary_words:
        probes = vocabulary
    else:
        stride = max(1, len(vocabulary) // args.dictionary_probes)
        probes = vocabulary[::stride][:args.dictionary_probes]
    normal = root / "normal.inputs"
    write_inputs(normal, texts, probes + [b"", "不存在𠀀词".encode()])
    yield "normal", normal, [], len(texts), len(probes) + 2

    main = root / "fixture.dict"
    entries = ["南京 8 n", "南京市 12 ns", "长江 9 n", "大桥 6 n", "长江大桥 12 n", "前缀完整 5 n"]
    entries.extend("甲" * i + f" {i + 1} n" for i in range(1, 25))
    main.write_text("\n".join(entries) + "\n", encoding="utf-8")
    first, last = root / "first.user", root / "last.user"
    first.write_text("南京市 123 first\n南 nz\n𠀀 5 x\n启动用户词\n长用户词" + "乙" * 513 + " 99 long\n", encoding="utf-8")
    last.write_text("南京市 456 last\n英文abc eng\n", encoding="utf-8")
    user_texts = ["南京市长江大桥", "南𠀀启动用户词未知", "甲" * 52, "前缀?前缀完整", "英文abc",
                  "长用户词" + "乙" * 513, "南京市,长江大桥;南 𠀀"]
    user_inputs = root / "startup.inputs"
    write_inputs(user_inputs, user_texts, ["南京市", "南", "𠀀", "启动用户词", "英文abc", "前缀"])
    user_args = ["--dict", str(main), "--users", str(first) + "|" + str(last)]
    yield "startup", user_inputs, user_args, len(user_texts), 6
    # Chinese separators must visibly split known dictionary entries, so this
    # also proves ResetSeparators takes effect rather than just being callable.
    yield "separators", user_inputs, user_args + ["--separators", " ,;\n京甲"], len(user_texts), 6

    lengths = [513, 65535, 65536]
    long_words = [first + "乙" * (length - 1) for first, length in zip("甲丙丁", lengths)]
    long_dict = root / "long.dict"
    long_dict.write_text("普通 10 n\n" + "".join(word + " 100 n\n" for word in long_words), encoding="utf-8")
    empty_user = root / "empty.user"
    empty_user.write_text("", encoding="utf-8")
    long_inputs = root / "long.inputs"
    write_inputs(long_inputs, long_words)
    yield "long", long_inputs, ["--dict", str(long_dict), "--users", str(empty_user), "--only-mp"], 3, 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--previous", required=True, type=pathlib.Path)
    parser.add_argument("--current", required=True, type=pathlib.Path)
    parser.add_argument("--output-dir", type=pathlib.Path, default=pathlib.Path("/tmp/cppjieba-dat-only-comparison"))
    parser.add_argument("--random-texts", type=int, default=300)
    parser.add_argument("--dictionary-probes", type=int, default=2000)
    parser.add_argument("--all-dictionary-words", action="store_true")
    parser.add_argument("--keep-transcripts", action="store_true")
    args = parser.parse_args()
    if args.random_texts < 0 or args.dictionary_probes < 1:
        parser.error("random-texts must be nonnegative and dictionary-probes positive")
    args.repo, args.previous, args.current = args.repo.resolve(), args.previous.resolve(), args.current.resolve()
    root = args.output_dir.resolve()
    root.mkdir(parents=True, exist_ok=True)
    report = dict(previous_binary=str(args.previous), current_binary=str(args.current),
                  binary_sha256={"previous": digest(args.previous), "current": digest(args.current)},
                  oracle_source_sha256=digest(pathlib.Path(__file__).with_name("dat_only_oracle.cpp")),
                  comparison="complete byte-for-byte transcript comparison from independent processes", scenarios=[])
    variants = [("previous-A", args.previous, "A", []), ("previous-C", args.previous, "C", []),
                ("previous-D", args.previous, "D", []), ("current-C", args.current, "C", []),
                ("current-D", args.current, "D", []),
                ("current-D-sparse", args.current, "D", ["--hmm-budget", "1"])]
    try:
        for name, inputs, options, text_count, probes in fixtures(args, root):
            scenario = dict(name=name, text_count=text_count, find_probes=probes,
                            input_sha256=digest(inputs), variants=[])
            report["scenarios"].append(scenario)
            reference = root / (name + ".previous-A.out")
            transcripts = []
            for label, binary, mode, extras in variants:
                output = root / (name + "." + label + ".out")
                errors = root / (name + "." + label + ".stderr")
                command = [str(binary), str(args.repo), "--mode", mode] + options + extras
                print(f"{name}: {label}", file=sys.stderr, flush=True)
                with inputs.open("rb") as source, output.open("wb") as destination, errors.open("wb") as stderr:
                    completed = subprocess.run(command, stdin=source, stdout=destination, stderr=stderr)
                if completed.returncode:
                    raise RuntimeError(f"oracle exited {completed.returncode}: {command}; see {errors}")
                require_equal(reference, output)
                scenario["variants"].append(dict(name=label, command=command, output_bytes=output.stat().st_size,
                                                  output_sha256=digest(output), exact_match=True))
                transcripts.append(output)
            if not args.keep_transcripts:
                for output in transcripts:
                    output.unlink()
        report["passed"] = True
    finally:
        (root / "comparison.json").write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Independent process outputs match for all {len(report['scenarios'])} scenarios and {len(variants)} variants.")
    print(root / "comparison.json")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print("DAT-only comparison failed: " + str(error), file=sys.stderr)
        sys.exit(1)
