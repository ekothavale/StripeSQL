#!/usr/bin/env python3
# Copyright (c) 2026 Ethan Kothavale. Distributed under the MIT License (see LICENSE.md).
"""
Profiles StripeSQL on the workloads behind the README benchmarks (macOS only).

For each workload it attaches macOS `sample` to a running StripeSQL for the whole run (one sample
per millisecond) and prints, for every function above 1%, the share of samples spent inside it,
including everything it calls. It also reports the run's peak memory.

  lookup      20,000 primary-key lookups on a 10k-row table
  scan        20 full scans of a 10k-row table
  insert      10,000 autocommit inserts
  insert-txn  100,000 inserts in one transaction
  delete      DELETE FROM on a 50k-row table

With --io it instead counts the file calls StripeSQL makes per statement (seeks, reads, writes,
opens, fsyncs and bytes) for a lookup, a scanned row and an autocommit insert, using bench/iocount.c.

Usage, from the repository root:  make profile                       (every workload)
                                  make profile PROFILE="lookup insert"
                                  make profile PROFILE=--io
Set STRIPESQL_BIN to profile a different binary.
"""
import os, random, re, shutil, subprocess, sys, tempfile
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.abspath(os.environ.get("STRIPESQL_BIN", os.path.join(REPO, "main")))
IOCOUNT_SRC = os.path.join(REPO, "bench", "iocount.c")
FRAME = re.compile(r"^(?P<prefix>[\s+!:|]*)(?P<count>\d+)\s+(?P<frame>.+?)\s+\(in (?P<image>[^)]+)\)")
SKIP = {"start", "???", "main", "runFile"}


def write(name, sql):
    with open(name, "w") as f:
        f.write(sql)


def run(sql_file, env=None):
    r = subprocess.run([BIN, sql_file], capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.exit(f"{sql_file}: StripeSQL exited {r.returncode}\n{r.stdout[-400:]}")
    return r


def fresh():
    shutil.rmtree("tables", ignore_errors=True)
    os.mkdir("tables")


def inserts(table, lo, hi, columns=1):
    values = (lambda i: f"{i}") if columns == 1 else (lambda i: f"{i}, {i}")
    return "".join(f"INSERT INTO {table} VALUES ({values(i)});\n" for i in range(lo, hi + 1))


def load_10k():
    fresh()
    write("load.sql", "CREATE TABLE p (id int PRIMARY KEY, v int);\nBEGIN TRANSACTION;\n" + inserts("p", 1, 10000, 2) + "COMMIT;\n")
    run("load.sql")


# ---------------------------------------------------------------------------------------------------
# workloads: each prepares tables/ and returns the SQL file to profile

def prep_lookup():
    load_10k()
    rng = random.Random(1)
    write("w.sql", "".join(f"SELECT v FROM p WHERE id = {rng.randint(1, 10000)};\n" for _ in range(20000)))
    return "w.sql"


def prep_scan():
    load_10k()
    rng = random.Random(1)
    write("w.sql", "".join(f"SELECT v FROM p WHERE v = {rng.randint(1, 10000)};\n" for _ in range(20)))
    return "w.sql"


def prep_insert():
    fresh()
    write("w.sql", "CREATE TABLE s (id int PRIMARY KEY);\n" + inserts("s", 1, 10000))
    return "w.sql"


def prep_insert_txn():
    fresh()
    write("w.sql", "CREATE TABLE s (id int PRIMARY KEY);\nBEGIN TRANSACTION;\n" + inserts("s", 1, 100000) + "COMMIT;\n")
    return "w.sql"


def prep_delete():
    fresh()
    write("load.sql", "CREATE TABLE d (id int PRIMARY KEY);\nBEGIN TRANSACTION;\n" + inserts("d", 1, 50000) + "COMMIT;\n")
    run("load.sql")
    write("w.sql", "DELETE FROM d;\n")
    return "w.sql"


WORKLOADS = {"lookup": prep_lookup, "scan": prep_scan, "insert": prep_insert, "insert-txn": prep_insert_txn, "delete": prep_delete}


# ---------------------------------------------------------------------------------------------------
# sampling

def inclusive_times(sample_file):
    """function -> samples spent inside it (including callees), counting recursion once per stack"""
    text = open(sample_file).read()
    graph = text[text.index("Call graph:"):text.index("Total number in stack")]
    inclusive = defaultdict(int)
    stack = []  # (depth, function) of the current call path
    for line in graph.splitlines()[1:]:
        m = FRAME.match(line)
        if not m:
            continue
        depth, count = len(m.group("prefix")), int(m.group("count"))
        function = re.sub(r"\s*\+\s*\d+$", "", m.group("frame")).strip()
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if function not in {f for _, f in stack}:
            inclusive[function] += count
        stack.append((depth, function))
    return inclusive


def profile(name):
    sql = WORKLOADS[name]()
    proc = subprocess.Popen([BIN, sql], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["sample", str(proc.pid), "600", "1", "-mayDie", "-file", "sample.txt"], capture_output=True)
    _, _, usage = os.wait4(proc.pid, 0)
    inclusive = inclusive_times("sample.txt")
    total = max(inclusive.values())
    print(f"### {name}: {total:,} samples (about {total / 1000:.1f} s), peak memory {usage.ru_maxrss / 1e6:,.0f} MB\n")
    print("| Time | Samples | Function |")
    print("|-----:|--------:|----------|")
    for function, count in sorted(inclusive.items(), key=lambda kv: -kv[1]):
        if count >= total * 0.01 and function not in SKIP:
            print(f"| {100 * count / total:.1f}% | {count:,} | `{function}` |")
    print(flush=True)


# ---------------------------------------------------------------------------------------------------
# per-statement file calls

def io_counts():
    lib = os.path.abspath("iocount.dylib")
    subprocess.run(["clang", "-dynamiclib", "-O2", IOCOUNT_SRC, "-o", lib], check=True)
    env = dict(os.environ, DYLD_INSERT_LIBRARIES=lib)

    def count(sql, per):
        line = [l for l in run(sql, env).stderr.splitlines() if l.startswith("[iocount]")][-1]
        return {k: int(v) / per for k, v in re.findall(r"(\w+)=(\d+)", line)}

    load_10k()
    write("pk.sql", "".join(f"SELECT v FROM p WHERE id = {i * 97 % 10000 + 1};\n" for i in range(100)))
    write("scan.sql", "SELECT v FROM p WHERE v = 5;\n")
    write("ins.sql", "".join(f"INSERT INTO p VALUES ({20000 + i}, {i});\n" for i in range(100)))
    rows = [("Primary-key lookup (10k-row table)", count("pk.sql", 100)),
            ("Row visited by a full scan", count("scan.sql", 10000)),
            ("Autocommit INSERT (10k-row table)", count("ins.sql", 100))]
    keys = ["fseek", "fread", "fwrite", "fopen", "fclose", "fsync", "bytesRead", "bytesWritten"]
    print("### File calls per statement\n")
    print("| Operation | " + " | ".join(f"`{k}`" for k in keys) + " |")
    print("|---|" + "|".join("---:" for _ in keys) + "|")
    for label, c in rows:
        print(f"| {label} | " + " | ".join(f"{c[k]:,.1f}" for k in keys) + " |")
    print(flush=True)


def main():
    if sys.platform != "darwin":
        sys.exit("profile.py relies on macOS `sample` and dyld interposing, so it only runs on macOS")
    if not os.path.exists(BIN):
        sys.exit(f"{BIN} not found; run `make` first")
    args = sys.argv[1:]
    io = "--io" in args
    chosen = [a for a in args if a != "--io"] or ([] if io else list(WORKLOADS))
    unknown = [c for c in chosen if c not in WORKLOADS]
    if unknown:
        sys.exit(f"unknown workload(s): {', '.join(unknown)}; choose from {', '.join(WORKLOADS)} or --io")
    workdir = tempfile.mkdtemp(prefix="stripesql-profile-")
    try:
        os.chdir(workdir)
        if io:
            io_counts()
        for name in chosen:
            profile(name)
    finally:
        os.chdir(REPO)
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
