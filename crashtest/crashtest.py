#!/usr/bin/env python3
# Copyright (c) 2026 Ethan Kothavale. Distributed under the MIT License (see LICENSE.md).
"""
Crash-recovery test for StripeSQL's write-ahead log (macOS only).

For each scenario the harness:
  1. builds a baseline database and records every table's contents,
  2. runs the commit once to record the post-commit contents and count its file writes and syncs,
  3. for every one of those N events: restores the baseline, runs the commit with the process
     killed just before event N, then starts StripeSQL normally (which runs recovery) and checks that
       - every table holds exactly its pre-commit or its post-commit contents, never a mix, and a
         table that doesn't exist has neither a schema entry nor a file (CREATE and DROP TABLE),
       - a primary-key lookup of every id agrees with a full scan,
       - the log is empty afterwards.

The kill is done by syncspy.c, injected with DYLD_INSERT_LIBRARIES. It models a process crash, not
power loss: writes that already reached the OS survive the crash.

Usage, from the repository root:  make && python3 crashtest/crashtest.py
Set STRIPESQL_BIN to test a different binary (make coverage-full uses an instrumented one).
Everything runs in a temporary directory; the repository's tables/ directory is never touched.
"""
import os, re, shutil, subprocess, sys, tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.environ.get("STRIPESQL_BIN", os.path.join(REPO, "main"))  # overridable, e.g. by `make coverage-full`
SPY_SRC = os.path.join(REPO, "crashtest", "syncspy.c")
QUERY_TIMEOUT = 10  # seconds; a damaged tree can make a scan loop forever
IDS = list(range(1, 400))  # every primary key the scenarios can produce
SPY = None  # path of the built syncspy.dylib


class Inconsistent(Exception):
    pass


def run(sql, crash_at=None):
    """run a SQL script with StripeSQL in the current directory; crash_at kills it before that event"""
    with open("ct.sql", "w") as f:
        f.write(sql)
    env = dict(os.environ)
    if crash_at is not None:
        env.update(DYLD_INSERT_LIBRARIES=SPY, SYNCSPY_CRASH_AT=str(crash_at))
    try:
        return subprocess.run([BIN, "ct.sql"], capture_output=True, text=True, env=env, timeout=QUERY_TIMEOUT)
    except subprocess.TimeoutExpired:
        raise Inconsistent("a query hung (the table is damaged)")


def rows(out):
    """parse 'a | b' result lines into tuples of ints"""
    return [tuple(int(x) for x in l.split(" | ")) for l in out.splitlines() if re.fullmatch(r"-?\d+( \| -?\d+)*", l)]


def contents(table):
    """a table's rows from a full scan, after checking that primary-key lookups agree with the scan;
    None if the table doesn't exist"""
    scan = run(f"SELECT id, v FROM {table};\n")
    if scan.returncode == 65 and "does not exist" in scan.stdout:
        if os.path.exists(os.path.join("tables", f"{table}.tbl")):
            raise Inconsistent(f"{table} has a file but no schema entry")
        return None
    if scan.returncode != 0:
        raise Inconsistent(f"scanning {table} failed: {scan.stdout.strip()[-200:]}")
    found = sorted(rows(scan.stdout))
    lookups = run("".join(f"SELECT id, v FROM {table} WHERE id = {i};\n" for i in IDS))
    if lookups.returncode != 0:
        raise Inconsistent(f"looking up {table} failed: {lookups.stdout.strip()[-200:]}")
    if sorted(rows(lookups.stdout)) != found:
        raise Inconsistent(f"primary-key lookups on {table} disagree with a full scan")
    return found


def database(tables):
    return {t: contents(t) for t in tables}


def reset_dir(path, source=None):
    shutil.rmtree(path, ignore_errors=True)
    if source:
        shutil.copytree(source, path)
    else:
        os.mkdir(path)


def log_size():
    path = os.path.join("tables", "stripe.log")
    return os.path.getsize(path) if os.path.exists(path) else 0


def sweep(label, setup_sql, commit_sql, tables):
    """crash commit_sql before each of its writes and syncs; returns True if every crash recovered cleanly"""
    reset_dir("tables")
    assert run(setup_sql).returncode == 0, "setup failed"
    reset_dir("baseline", "tables")
    old = database(tables)

    counted = run(commit_sql, crash_at=0)  # SYNCSPY_CRASH_AT=0 counts events without crashing
    assert counted.returncode == 0, "commit failed"
    new = database(tables)
    assert old != new, "the commit didn't change anything"
    events = sum(int(x) for x in re.search(r"fsync=(\d+) F_FULLFSYNC=(\d+) writes=(\d+)", counted.stderr).groups())

    outcomes = {"old": 0, "new": 0}
    for n in range(1, events + 1):
        reset_dir("tables", "baseline")
        crashed = run(commit_sql, crash_at=n)
        try:
            if crashed.returncode != 137:
                raise Inconsistent(f"expected the process to be killed, but it exited with {crashed.returncode}")
            got = database(tables)  # the first statement run here performs recovery
            if got == old:
                outcomes["old"] += 1
            elif got == new:
                outcomes["new"] += 1
            else:
                raise Inconsistent("tables hold a mix of pre- and post-commit contents")
            if log_size() != 0:
                raise Inconsistent("the log is not empty after recovery")
        except Inconsistent as e:
            print(f"FAIL  {label}: crash before event {n} of {events}: {e}")
            return False
    print(f"ok    {label}: {events} crash points, all recovered "
          f"(pre-commit state {outcomes['old']}, post-commit state {outcomes['new']})", flush=True)
    return True


SETUP = ("CREATE TABLE t (id int PRIMARY KEY, v int);\nCREATE TABLE u (id int PRIMARY KEY, v int);\n"
         "BEGIN TRANSACTION;\n" + "".join(f"INSERT INTO t VALUES ({i}, {i});\n" for i in range(1, 151)) +
         "INSERT INTO u VALUES (1, 1);\nCOMMIT;\n")

SCENARIOS = [
    ("multi-table transaction (60 inserts with splits, an update and a delete, 2 tables)",
     "BEGIN TRANSACTION;\n" + "".join(f"INSERT INTO t VALUES ({i}, {i});\n" for i in range(151, 211)) +
     "UPDATE t SET v = -1 WHERE id = 7;\nDELETE FROM t WHERE id = 20;\nINSERT INTO u VALUES (2, 2);\nCOMMIT;\n"),
    ("autocommit INSERT", "INSERT INTO t VALUES (300, 300);\n"),
    ("autocommit DELETE", "DELETE FROM t WHERE id = 42;\n"),
    ("CREATE TABLE", "CREATE TABLE w (id int PRIMARY KEY, v int);\n"),
    ("DROP TABLE", "DROP TABLE u;\n"),
]


def main():
    global SPY
    if sys.platform != "darwin":
        sys.exit("crashtest relies on macOS dyld interposing and only runs on macOS")
    if not os.path.exists(BIN):
        sys.exit(f"{BIN} not found; run `make` first")
    workdir = tempfile.mkdtemp(prefix="stripesql-crashtest-")
    try:
        SPY = os.path.join(workdir, "syncspy.dylib")
        subprocess.run(["clang", "-dynamiclib", "-O2", SPY_SRC, "-o", SPY], check=True)
        os.chdir(workdir)
        passed = all([sweep(label, SETUP, sql, ["t", "u", "w"]) for label, sql in SCENARIOS])
    finally:
        os.chdir(REPO)
        shutil.rmtree(workdir, ignore_errors=True)
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
