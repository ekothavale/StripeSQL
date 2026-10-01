#!/usr/bin/env python3
# Copyright (c) 2026 Ethan Kothavale. Distributed under the MIT License (see LICENSE.md).
"""
Runs the benchmarks reported in the README's Benchmarks section and prints each result as a
markdown table.

  lookup       primary-key lookup vs. full scan at 1k, 10k and 100k rows
  batch        10k inserts in autocommit mode vs. one transaction, int and text keys
  inserts100k  building a 100k-row table, and adding 10k rows to one
  sqlite       StripeSQL vs. the sqlite3 CLI on four 10k-row insert/delete workloads

Usage, from the repository root:  make bench            (all of them, about 5 minutes)
                                  make bench BENCH=lookup  (one or more, space-separated)
Set STRIPESQL_BIN to benchmark a different binary.

StripeSQL figures come from its own CLOCK_MONOTONIC timer (printed after a file-mode run), which
excludes process startup; the SQLite comparison times both engines as whole processes instead. Each
figure is the median of several trials, after one discarded warm-up run where noted. Everything runs
in a temporary directory, so the repository's tables/ directory is never touched.
"""
import os, platform, random, re, shutil, statistics, subprocess, sys, tempfile, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.abspath(os.environ.get("STRIPESQL_BIN", os.path.join(REPO, "main")))
TRIALS = 5


# ---------------------------------------------------------------------------------------------------
# helpers

def write(name, sql):
    with open(name, "w") as f:
        f.write(sql)


def run(sql_file):
    """runs a SQL file with StripeSQL; returns (seconds by StripeSQL's own timer, stdout)"""
    r = subprocess.run([BIN, sql_file], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{sql_file}: StripeSQL exited {r.returncode}\n{r.stdout[-400:]}")
    return float(re.findall(r"^ ([\d.]+) ms$", r.stdout, re.M)[-1]) / 1000, r.stdout


def fresh(snapshot=None):
    """empties tables/ (or restores it from a snapshot directory)"""
    shutil.rmtree("tables", ignore_errors=True)
    if snapshot:
        shutil.copytree(snapshot, "tables")
    else:
        os.mkdir("tables")


def median(xs):
    return statistics.median(xs)


def table(header, rows):
    print("| " + " | ".join(header) + " |")
    print("|" + "|".join("---" for _ in header) + "|")
    for row in rows:
        print("| " + " | ".join(row) + " |")
    print(flush=True)


def ms(seconds):
    return f"{seconds * 1000:,.3f} ms" if seconds < 0.0001 else f"{seconds * 1000:,.2f} ms" if seconds < 0.01 else f"{seconds * 1000:,.1f} ms" if seconds < 1 else f"{seconds * 1000:,.0f} ms"


def inserts(lo, hi, text=False, table_name="s"):
    lit = (lambda i: f"'key{i:05d}'") if text else str
    return "".join(f"INSERT INTO {table_name} VALUES ({lit(i)});\n" for i in range(lo, hi + 1))


# ---------------------------------------------------------------------------------------------------
# primary-key lookup vs. full scan

def bench_lookup():
    print("### Primary-key lookup vs. full scan\n")
    rows = []
    for n, scans in ((1000, 200), (10000, 40), (100000, 4)):
        fresh()
        write("load.sql", "CREATE TABLE p (id int PRIMARY KEY, v int);\nBEGIN TRANSACTION;\n" +
              "".join(f"INSERT INTO p VALUES ({i}, {i});\n" for i in range(1, n + 1)) + "COMMIT;\n")
        run("load.sql")
        rng = random.Random(n)
        pk_keys = [rng.randint(1, n) for _ in range(1000)]
        scan_keys = [rng.randint(1, n) for _ in range(scans)]
        write("pk.sql", "".join(f"SELECT v FROM p WHERE id = {k};\n" for k in pk_keys))
        write("scan.sql", "".join(f"SELECT v FROM p WHERE v = {k};\n" for k in scan_keys))

        # warm-up, which also checks that every query returns exactly its row
        for sql, keys in (("pk.sql", pk_keys), ("scan.sql", scan_keys)):
            out = run(sql)[1]
            values = [int(l) for l in out.splitlines() if l.strip().lstrip("-").isdigit()]
            if out.count("(1 row)") != len(keys) or values != keys:
                sys.exit(f"{sql}: wrong lookup results at {n} rows")

        pk = median([run("pk.sql")[0] / len(pk_keys) for _ in range(TRIALS)])
        scan = median([run("scan.sql")[0] / scans for _ in range(TRIALS)])
        size = os.path.getsize("tables/p.tbl")
        rows.append([f"{n:,}", ms(pk), ms(scan), f"{scan / pk:,.0f}×", f"{size / 1e6:,.1f} MB"])
        print(f"  ({n:,} rows done)", file=sys.stderr, flush=True)
    table(["Rows", "PK lookup", "Full scan", "Speedup", "Table file"], rows)


# ---------------------------------------------------------------------------------------------------
# transaction batching

def bench_batch():
    print("### Transaction batching (10,000 inserts)\n")
    rows = []
    for key, text in (("`int`", False), ("`text`", True)):
        create = f"CREATE TABLE s (id {'text' if text else 'int'} PRIMARY KEY);\n"
        write("auto.sql", create + inserts(1, 10000, text))
        write("txn.sql", create + "BEGIN TRANSACTION;\n" + inserts(1, 10000, text) + "COMMIT;\n")
        times = {}
        for mode in ("auto", "txn"):
            xs = []
            for trial in range(TRIALS + 1):  # the first run is a discarded warm-up
                fresh()
                t = run(f"{mode}.sql")[0]
                if trial:
                    xs.append(t)
            times[mode] = median(xs)
        rows.append([key, f"{times['auto']:.2f} s", f"{times['txn']:.3f} s",
                     f"{times['auto'] / times['txn']:.1f}×", f"~{10000 / times['txn']:,.0f}"])
    table(["Primary key", "Autocommit", "Single transaction", "Speedup", "Inserts/sec (transaction)"], rows)


# ---------------------------------------------------------------------------------------------------
# inserts at 100,000 rows

def bench_inserts100k():
    print("### Inserts at 100,000 rows\n")
    create = "CREATE TABLE s (id int PRIMARY KEY);\n"
    write("build_txn.sql", create + "BEGIN TRANSACTION;\n" + inserts(1, 100000) + "COMMIT;\n")
    write("build_auto.sql", create + inserts(1, 100000))
    write("add_txn.sql", "BEGIN TRANSACTION;\n" + inserts(100001, 110000) + "COMMIT;\n")
    write("add_auto.sql", inserts(100001, 110000))

    def trials(sql, n, snapshot=None):
        xs = []
        for _ in range(n):
            fresh(snapshot)
            xs.append(run(sql)[0])
        return median(xs)

    build_txn = trials("build_txn.sql", TRIALS)
    print("  (build in a transaction done)", file=sys.stderr, flush=True)
    fresh()
    run("build_txn.sql")
    shutil.rmtree("snapshot", ignore_errors=True)
    shutil.copytree("tables", "snapshot")
    add_txn = trials("add_txn.sql", TRIALS, "snapshot")
    add_auto = trials("add_auto.sql", TRIALS, "snapshot")
    print("  (adding rows done; the autocommit build takes about a minute and a half)", file=sys.stderr, flush=True)
    build_auto = trials("build_auto.sql", 3)  # about 30 s per run
    shutil.rmtree("snapshot", ignore_errors=True)

    cell = lambda t, n: f"{t:,.2f} s (~{n / t:,.0f} inserts/s)" if t < 10 else f"{t:,.1f} s (~{n / t:,.0f} inserts/s)"
    table(["Scenario", "Autocommit", "Single transaction"], [
        ["Build a 100,000-row table (keys 1–100,000)", cell(build_auto, 100000), cell(build_txn, 100000)],
        ["Add 10,000 rows to a 100,000-row table", cell(add_auto, 10000), cell(add_txn, 10000)],
    ])
    print("(medians of 5 trials, 3 for the autocommit build; no warm-up run)\n")


# ---------------------------------------------------------------------------------------------------
# comparison with SQLite

SQLITE_WORKLOADS = ["10k.sql", "10k_txn.sql", "10k_str.sql", "10k_str_txn.sql"]


def sqlite_workload(name):
    """the four workloads: create a table, insert 10k rows (int or text key, bare or in one transaction), delete, drop"""
    text = "_str" in name
    txn = "_txn" in name
    sql = f"CREATE TABLE stress (id {'TEXT' if text else 'int'} PRIMARY KEY);\n"
    if txn:
        sql += "BEGIN TRANSACTION;\n"
    sql += inserts(1, 10000, text, "stress")
    if txn:
        sql += "COMMIT;\n"
    return sql + "DELETE FROM stress;\nDROP TABLE stress;\n"


def bench_sqlite():
    sqlite = shutil.which("sqlite3")
    if not sqlite:
        print("### Comparison with SQLite\n\nskipped: sqlite3 isn't installed\n")
        return
    version = subprocess.run([sqlite, "--version"], capture_output=True, text=True).stdout.split()[0]
    print(f"### Comparison with SQLite (sqlite3 {version})\n")

    def stripesql(path):
        fresh()
        t0 = time.perf_counter()
        r = subprocess.run([BIN, path], capture_output=True, text=True)
        elapsed = time.perf_counter() - t0
        if r.returncode != 0:
            sys.exit(f"{path}: StripeSQL exited {r.returncode}")
        return elapsed

    def sqlite3(path):
        for f in ("cmp.db", "cmp.db-journal"):
            if os.path.exists(f):
                os.remove(f)
        with open(path) as src:
            t0 = time.perf_counter()
            r = subprocess.run([sqlite, "cmp.db"], stdin=src, capture_output=True, text=True)
            elapsed = time.perf_counter() - t0
        if r.returncode != 0 or r.stderr:
            sys.exit(f"{path}: sqlite3 failed: {r.stderr[-300:]}")
        return elapsed

    def timed(fn, path):
        fn(path)  # warm-up
        return median([fn(path) for _ in range(TRIALS)])

    labels = {"10k.sql": "int PK, no transaction", "10k_txn.sql": "int PK, single transaction",
              "10k_str.sql": "text PK, no transaction", "10k_str_txn.sql": "text PK, single transaction"}
    rows = []
    for name in SQLITE_WORKLOADS:
        write(name, sqlite_workload(name))
        ours, theirs = timed(stripesql, name), timed(sqlite3, name)
        fmt = lambda t: f"{t:.2f} s" if t >= 1 else f"{t:.3f} s"
        rows.append([f"`{name}` ({labels[name]})", fmt(ours), fmt(theirs), f"{ours / theirs:.1f}×"])
    table(["Benchmark", "StripeSQL", "SQLite", "Ratio"], rows)


# ---------------------------------------------------------------------------------------------------

BENCHMARKS = {"lookup": bench_lookup, "batch": bench_batch, "inserts100k": bench_inserts100k, "sqlite": bench_sqlite}


def main():
    chosen = sys.argv[1:] or list(BENCHMARKS)
    unknown = [c for c in chosen if c not in BENCHMARKS]
    if unknown:
        sys.exit(f"unknown benchmark(s): {', '.join(unknown)}; choose from {', '.join(BENCHMARKS)}")
    if not os.path.exists(BIN):
        sys.exit(f"{BIN} not found; run `make` first")
    cpu = platform.processor()
    if sys.platform == "darwin":
        cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
    print(f"StripeSQL benchmarks: {cpu}, {platform.system()} {platform.release()}, binary {os.path.relpath(BIN, REPO)}\n")
    workdir = tempfile.mkdtemp(prefix="stripesql-bench-")
    try:
        os.chdir(workdir)
        for name in chosen:
            BENCHMARKS[name]()
    finally:
        os.chdir(REPO)
        shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    main()
