StripeSQL is a relational database management system written from scratch in C, with no dependencies beyond the C standard library and POSIX. It supports a SQL front end backed by a custom B+ tree storage engine with slotted-page file format.

---

## I. Architecture

A SQL query moves through five stages before touching the disk:

```
  ┌──────────────────────────────────────────────────────────┐
  │                      SQL Front End                       │
  │                                                          │
  │   SQL Query                                              │
  │      │                                                   │
  │      ▼                                                   │
  │  ┌────────┐    token    ┌────────┐    AST                │
  │  │ Lexer  │ ──────────► │ Parser │ ──────────────┐       │
  │  │lexer.c │             │parser.c│               │       │
  │  └────────┘             └────────┘               ▼       │
  │                                         ┌─────────────┐  │
  │                                         │  Generator  │  │
  │                                         │generator.c  │  │
  │                                         └──────┬──────┘  │
  │                                                │ bytecode│
  │                                                ▼         │
  │                                         ┌─────────────┐  │
  │                                         │     VM      │  │
  │                                         │    vm.c     │  │
  │                                         └──────┬──────┘  │
  └────────────────────────────────────────────────┼─────────┘
                                                   │
  ┌────────────────────────────────────────────────┼─────────┐
  │                   Storage Engine               │         │
  │                                                ▼         │
  │                                        ┌─────────────┐   │
  │                                        │   B+ Tree   │   │
  │                                        │   bplus.c   │   │
  │                                        └──────┬──────┘   │
  │                                               │          │
  │               ┌───────────────────────────────┤          │
  │               │               │               │          │
  │               ▼               ▼               ▼          │
  │        ┌───────────┐  ┌────────────┐  ┌────────────┐     │
  │        │   Page    │  │    Node    │  │  Ordering  │     │
  │        │  page.c   │  │   node.h   │  │ordering.c  │     │
  │        └─────┬─────┘  └────────────┘  └────────────┘     │
  │              │                                           │
  │              ▼                                           │
  │        ┌───────────┐                                     │
  │        │ Table I/O │   (.tbl files in tables/)           │
  │        │ tableIO.c │                                     │
  │        └─────┬─────┘                                     │
  │              │ each commit is logged before the          │
  │              ▼ table files are written                   │
  │        ┌───────────┐                                     │
  │        │    WAL    │   (redo log: tables/stripe.log)     │
  │        │   wal.c   │                                     │
  │        └───────────┘                                     │
  └──────────────────────────────────────────────────────────┘

  Schema (.scma) loaded by schema.c is shared between the Generator
  and VM to resolve column names and primary key positions.
```

**Lexer** (`src/SQL_interpreter/lexer.c`) — scans the raw query string into a flat token array, skipping whitespace and comments. Multi-word clauses such as `INSERT INTO` are matched at the parse level, not here.

**Parser** (`src/SQL_interpreter/parser.c`) — consumes the token array and produces an AST. Each node carries a type tag, a keyword token, a flag for sub-variants (e.g. `DISTINCT`, `PRIMARY KEY`), and up to seven children.

**Generator** (`src/SQL_interpreter/generator.c`) — walks the AST and emits a bytecode `chunk` against the schema. Detects primary-key equality in `WHERE` clauses and emits `OP_KEY_SEARCH` instead of a scan loop.

**VM** (`src/SQL_interpreter/vm.c`) — stack-based interpreter that executes the bytecode chunk. Maintains up to four concurrent scanners, each holding a cursor into a B+ tree (`src/storage_engine/scanner.h`). A session-scoped transaction registry, kept outside the per-statement VM state, lets `BEGIN TRANSACTION` hold a table open — dirty writes uncommitted — across multiple statements until `COMMIT` or `DISCARD`. The schema is loaded once per session and kept in memory; it's reloaded from disk after every `CREATE TABLE` or `DROP TABLE`, whether or not the statement succeeded. Table files stay open between statements too, in a cache of 16: a table is closed and loaded again only when it's dropped, when changes to it are discarded, or when the cache needs its slot.

**B+ tree** (`src/storage_engine/bplus.c`) — the index that finds the page holding any primary key. Rows are kept in primary-key order: each leaf node files its pages under an upper bound for the keys in them and is searched by binary search, and the leaves link bidirectionally for sequential scans. A page that fills up is split and the new page is filed beside it; a page that empties is removed, and nodes split, borrow, and merge to stay at least half full.

**Slotted page** (`src/storage_engine/page.c`) — variable-length records are stored in 4 KB slotted pages, as many to a page as fit. Each slot holds its record's ordering key, a pointer into the entry array, and a byte length, and the slots are kept sorted by key. The page header records the smallest and largest key in the page.

**Ordering** (`src/storage_engine/ordering.c`) — converts a primary key into the ordering key that nodes and pages compare, keeping the key's own order: an integer has its sign bit flipped so that it sorts as an unsigned number, and text is compared byte by byte. A text key can be at most 24 bytes long. Every key in a table has the same type, so the type is stored once, in the table file's header, and not with each key, and the table's slots, page headers and nodes are sized for its keys: an integer key takes 8 bytes on disk and a text key 24.

**Table I/O** (`src/storage_engine/tableIO.c`) — serialises pages and nodes to `.tbl` files in the `tables/` directory. Each page or node is read with one seek and one read of its fixed on-disk size, then decoded in memory. Writes are buffered in dirty hashmaps until a commit, which sends them through the write-ahead log before writing and syncing (`fsync`) the table files. A new table exists only in memory until its first commit creates its file. While a statement runs, the first change it makes to each page, node, or delete marker saves a copy of that object's prior pending version, so a statement that fails can be rolled back without disturbing earlier statements in the same transaction.

**Write-ahead log** (`src/storage_engine/wal.c`) — a redo log in `tables/stripe.log` that makes commits atomic across crashes. At commit, every change is appended as a fixed-size entry with a CRC-32C checksum: the exact bytes of each dirty page, node, delete marker, and table header, plus whole-file operations — creating a new table's file, rewriting the schema file, or removing a dropped table's file. The log is synced, a checksummed commit marker is appended and synced (the commit point), the changes are made to the files, which are synced along with their directory, and the log is truncated. At startup, recovery replays a committed log — every entry is safe to repeat, so it doesn't need to know which changes finished before the crash — or discards an uncommitted one. Uncommitted changes never leave memory, so there is never anything on disk to undo. The log names files within `tables/` and knows nothing about what's in them.

**File helpers** (`src/storage_engine/file.c`) — format-independent utilities shared by Table I/O and the write-ahead log: syncing files and directories, and big-endian integer encoding.

---

## II. Supported Features

**Data Definition**
- `CREATE TABLE name (col type [PRIMARY KEY], ...)` — creates a new table and registers it in the schema, committed together so a crash can't leave one without the other
- `DROP TABLE name` — removes the schema entry and deletes the table file, committed together
- Column types: `int`, `text`
- `PRIMARY KEY` constraint on a single column per table

**Data Manipulation**
- `INSERT INTO table VALUES (v1, v2, ...)` — inserts a row into the page that covers its primary key, splitting the page if it's full. A row has to fit in one 4 KB page, and a text primary key can be at most 24 bytes long. String literals must be single-quoted (`'...'`); double-quoted or unquoted string values are reported as an error at compile time instead of being silently misparsed.
- `SELECT * FROM table` and `SELECT col1, col2, ... FROM table` — rows come back in primary-key order: integers ascending, negative ones first, and text byte by byte
- `SELECT DISTINCT ...` — parsed, but rows aren't deduplicated yet (see Known Issues)
- `UPDATE table SET col = expr [WHERE ...]`
- `DELETE FROM table [WHERE ...]`
- Referencing a table with no registered schema (e.g. a typo'd name) reports a compile error instead of crashing the interpreter, across `SELECT`, `INSERT`, `UPDATE`, and `DELETE`

**Transactions**
- Transactions are ACID for a single process: commits are atomic and durable across crashes, statements are atomic, and only one process can use a database at a time (see Execution Modes). With the default `FULL_FSYNC 0`, durability covers process and OS crashes; set `FULL_FSYNC 1` in `const.h` to also survive power loss on macOS.
- `BEGIN TRANSACTION` — starts a transaction; every table touched by a subsequent statement stays open, with its writes buffered but not flushed to disk
- `COMMIT` — commits all changes made since `BEGIN TRANSACTION` to disk and closes the tables; the changes to every table touched are committed together, so after a crash either all of them survive or none do
- `DISCARD` — drops all changes made since `BEGIN TRANSACTION` without writing anything to disk
- A single transaction may span multiple tables
- `BEGIN TRANSACTION` while already in a transaction, or `COMMIT`/`DISCARD` with none active, is a runtime error
- `CREATE TABLE` and `DROP TABLE` can't run inside a transaction: they always commit on their own, so `DISCARD` couldn't undo them
- Every commit — `COMMIT`, or the implicit commit at the end of each statement outside a transaction — goes through the write-ahead log and is durable before the statement returns. So do `CREATE TABLE`, `DROP TABLE`, and every schema change. If the process crashes mid-commit, the next startup finishes or discards the commit, so a table never holds part of one. Syncs use `fsync` (set `FULL_FSYNC` in `const.h` to use macOS's stronger `F_FULLFSYNC` instead). Read-only statements write and sync nothing.
- Statements are atomic. Any error while a statement runs — a duplicate primary key, a row too large for a page, a type error, division by zero — halts it as a runtime error, and everything it changed is rolled back. Inside a transaction, earlier statements' changes stay pending and the transaction carries on; outside one, nothing was committed.
- A write failure before the commit point aborts the transaction with a runtime error. A failure after it, when the commit is already in the log but a table may be half-written, exits with code `74`, and recovery completes the commit on the next startup. Failures are never retried.

**Filtering and Expressions**
- `WHERE` clause with `=`, `!=`, `<`, `<=`, `>`, `>=`
- `AND`, `OR`, `NOT` logical operators with correct precedence
- `LIKE` pattern matching
- `IS NULL` and `IS NOT NULL`
- Arithmetic: `+`, `-`, `*`, `/`, unary `-`

**Result Set Operations**
- `ORDER BY col [ASC | DESC]` and `LIMIT n` are parsed but not yet applied to the result (see Known Issues)

**Query Optimization**
- Primary key equality (`WHERE pk_col = literal`) uses `OP_KEY_SEARCH` — a direct B+ tree lookup — instead of a full table scan, in `SELECT`, `UPDATE`, and `DELETE` statements alike

**Execution Modes**
- Interactive REPL (`./main`)
- Batch file execution (`./main file.sql`) supporting multiple semicolon-delimited statements
- Single-line comments (`-- ...`) and block comments (`/* ... */`), in SQL files and in the REPL. Block comments don't nest, and one that is never closed runs to the end of the input. The REPL runs each line as it's entered, so a block comment there has to close on the line it opens on
- One process at a time: StripeSQL holds an exclusive lock on `tables/stripe.lock` for as long as it runs; a second process exits with code `75`. The OS releases the lock when the process exits, even after a crash

---

## III. Roadmap

The following features are next on the todo list, roughly in priority order:

1. **Column lists in `INSERT`** — `INSERT INTO t (b, a) VALUES (2, 1)` is parsed, but the column list is ignored and the values are assigned in declared order. (`SELECT b, a FROM t` already returns columns in the order listed.)
2. **Apply `ORDER BY`, `LIMIT`, and `DISTINCT`** — all three are parsed but have no effect on the result yet.
3. **File-level garbage collection** — deleted pages and nodes are only marked as garbage, so table files never shrink. `condenseStripe` and `condenseAll` are declared in `tableIO.h` but not implemented yet.
4. **Propagate I/O errors** — `readNode` and `readPage` now return a failure for a failed read or a damaged object, but most of their callers in `bplus.c` don't check it yet.
5. **File structure analysis mode** - a new mode which creates a new database populated with the attributes of a given directory's files and subdirectories.

---

## IV. Known Issues

| # | Description |
|---|-------------|
| 1 | `ORDER BY`, `LIMIT`, and `SELECT DISTINCT` are parsed but have no effect on the result. |
| 2 | `INSERT` ignores its column list: `INSERT INTO t (b, a) VALUES (2, 1)` assigns the values in the order the columns were declared in `CREATE TABLE`. `SELECT` returns columns in the order they're listed. |
| 3 | `readNode` and `readPage` report a failed read or a damaged page or node, but most of their callers — the B+ tree's descents and rebalancing in particular — ignore the result, so an I/O error partway through a statement doesn't fail the statement. |
| 4 | Most syntax errors call `exit()`, which also ends a REPL session. A transaction open at that point is lost without a `DISCARD`; nothing it changed was written to disk, so the tables are unharmed. The REPL reads at most 1,023 characters of a line at a time, so a longer statement arrives in pieces and usually ends the session this way; run long statements from a file. |
| 5 | Values aren't checked against their column's type. Text in an `int` column is stored as text, and an `int` column keeps only the low 32 bits of a larger number, although a primary key is still ordered by the whole number. A primary key of the wrong type is refused, because the B+ tree can't compare it with the table's other keys, but only with a generic "failed to insert" error, and a lookup by one finds no rows. |
| 6 | Power loss on macOS: with the default `FULL_FSYNC 0`, a plain `fsync` leaves data in the drive's cache, which the drive may also reorder, so recently committed transactions are only guaranteed to survive a process or OS crash. Set `FULL_FSYNC 1` in `const.h` to survive power loss (at about 3 ms per sync). |

---

## V. Usage

### Build

```sh
make
```

This compiles all source files with `clang` at `-O3` and produces the `main` binary in the project root. Requires `clang` and `make`. Run the tests with `make test` (see Testing and Coverage).

### Run the REPL

```sh
./main
```

Type SQL statements ending with `;` and press Enter. Type `quit` or press `Ctrl-D` to exit. Blank lines are ignored.

### Run a SQL file

```sh
./main file.sql
```

All statements in the file are executed in order. The total wall-clock time is printed after the last statement. Exit codes mirror the POSIX convention: `65` for a compile error, `70` for a runtime error, `60` for a load error, and `74` for an I/O error (a script that can't be read, a commit that fails after its commit point, or crash recovery that can't finish), and `75` when another StripeSQL process is using the database.

### Crash recovery

Every run starts by checking `tables/stripe.log`. If a previous run crashed partway through a commit, it prints one of:

```
Recovery: finished an interrupted commit (N log entries)
Recovery: discarded an uncommitted transaction
```

If a committed log is damaged and can't be replayed, StripeSQL exits with code `74` and leaves the log in place for inspection.

### Crash-recovery test (macOS only)

```sh
make crashtest
```

Runs `crashtest/crashtest.py`, which kills StripeSQL just before every file write and `fsync` of a commit, then restarts it and checks that recovery left each table holding exactly its pre-commit or post-commit contents, with primary-key lookups agreeing with full scans and the log empty afterwards. It covers a two-table transaction whose 6,000 inserts split pages and B+ tree nodes, an autocommit `INSERT`, an autocommit `DELETE`, `CREATE TABLE`, and `DROP TABLE` (211 crash points in all); for `CREATE` and `DROP` it also checks that a table never has a file without a schema entry or the reverse. The kill is done by `crashtest/syncspy.c`, a library injected with `DYLD_INSERT_LIBRARIES` that intercepts `write` and `fsync`, so it models a process crash rather than power loss. It runs in a temporary directory and never touches `tables/`. Requires Python 3.

### Debug mode

```sh
./main -d          # REPL with debug tracing
./main file.sql -d # file mode with debug tracing
```

Debug mode prints the disassembled bytecode chunk before execution.

### Example session

```sql
CREATE TABLE users (id int PRIMARY KEY, name text);
INSERT INTO users VALUES (1, 'alice');
INSERT INTO users VALUES (4, 'donald');
SELECT * FROM users WHERE id = 4;
```

Expected output:
```
4 | donald
(1 row)
```

### Example session with a transaction

```sql
BEGIN TRANSACTION;
INSERT INTO users VALUES (7, 'grace');
DISCARD;
SELECT * FROM users WHERE id = 7;
```

Expected output: `grace` was never committed, so the lookup returns nothing.
```
(0 rows)
```

---

## VI. Extending the Engine

The pipeline is layered, so adding a new SQL feature follows a fixed sequence of steps.

**1. Add a keyword token** (`src/SQL_interpreter/lexer.c` / `lexer.h`)
Add the new keyword to the `token_type` enum and register it in the keyword-matching table inside `lexer.c`.

**2. Add an AST node type** (`src/SQL_interpreter/parser.h`)
If the new feature introduces a new clause or statement, add a variant to `ast_type` and, if needed, a flag to `ast_flag`.

**3. Parse the new syntax** (`src/SQL_interpreter/parser.c`)
Write a `parse*` function that consumes the relevant tokens and returns an `ast_node`. Hook it into the top-level `query()` dispatcher.

**4. Add opcodes** (`src/SQL_interpreter/chunk.h`)
If new VM behaviour is needed, extend the `opcode` enum. Single-byte opcodes with optional one- or two-byte operands follow the existing pattern.

**5. Generate bytecode** (`src/SQL_interpreter/generator.c`)
Extend `munchStmt` (or `munchExpr`) to handle the new AST node and emit the corresponding opcodes.

**6. Implement the opcode** (`src/SQL_interpreter/vm.c`)
Add a `case` to the dispatch loop in `run()`. Operations that touch the disk go through the scanner and the B+ tree API in `bplus.c`. A string popped off the stack never has to be freed: it belongs either to the scanner whose row it was read from, which frees it when it moves to another row, or to the statement's bytecode. An opcode that keeps a string for longer than the current row must take a copy, as `OP_EMIT_ROW` does for result rows.

**7. Update the schema if needed** (`src/SQL_interpreter/schema.c`)
If the feature introduces a new per-table or per-column attribute, extend the `schema` struct and update the binary serialisation in `schema.c`.

---

## VII. Benchmarks

Execution time is reported automatically after every file-mode run (wall-clock, measured with `CLOCK_MONOTONIC`):

```
./main file.sql
...
 1.243 ms
```

### Running the benchmarks

```sh
make bench                        # every benchmark below (about 5 minutes)
make bench BENCH="lookup batch"   # a subset: lookup, batch, inserts100k, sqlite
make profile                      # where the time goes, function by function (macOS only)
make profile PROFILE=--io         # file calls per statement (macOS only)
```

`make bench` builds `main` and runs `bench/bench.py`, which generates every workload, runs each trial against a fresh database in a temporary directory, and prints each table below as markdown. `make profile` runs `bench/profile.py`, which attaches macOS's `sample` to StripeSQL on five workloads (lookups, scans, autocommit inserts, a 100,000-row transaction, and `DELETE FROM`) and reports each function's share of the time, including what it calls, along with peak resident memory. With `--io` it instead counts the file calls a lookup, a scanned row, and an autocommit insert make, using `bench/iocount.c`. Both use the binary in `STRIPESQL_BIN` instead of `./main` when it's set.

All figures below were measured on an Apple M2 with an `-O3` build (the Makefile's flags, no sanitizers; default `FULL_FSYNC 0`, so every commit goes through the write-ahead log with plain `fsync`) and a warm OS page cache. Each is the median of 5 trials, taken after one discarded warm-up run. They will vary with page fill factor, tree depth, `M_GLOBAL`, and disk speed.

### Primary-key lookup vs. full scan

A table `(id int PRIMARY KEY, v int)` is loaded with N rows where `v = id`, so both columns hold identical values. Random keys are then looked up two ways: `WHERE id = k`, which compiles to `OP_KEY_SEARCH` (a B+ tree descent), and `WHERE v = k`, which has no index and falls back to a full table scan. Each lookup runs as its own statement, and the built-in timer's total is divided by the number of queries (1,000 index lookups per trial; fewer scans at larger N, since each one reads the whole table). Every query is checked to return exactly the expected row.

| Rows | PK lookup | Full scan | Speedup | Table file |
|-----:|----------:|----------:|--------:|-----------:|
| 1,000 | 8.0 µs | 82.2 µs | 10× | 0.1 MB |
| 10,000 | 9.0 µs | 0.68 ms | 75× | 0.5 MB |
| 100,000 | 9.2 µs | 6.55 ms | 711× | 4.8 MB |

- **Lookup latency grows slowly.** It rises about 15% across a 100× increase in rows, as the tree gets deeper, while scan time grows roughly linearly — the O(log n) vs. O(n) difference the index exists for. A lookup is read-only, so it never touches the log or syncs, and with the schema and the table already open it opens no files: at 10,000 rows, and still at 100,000, it is three reads, two nodes and the page (`make profile PROFILE=--io`).
- **A lookup decodes the whole page its row is in.** About 100 of these rows fit in a page, and a page is read and decoded as a unit, so finding one row costs a decode of all of them: that is nearly half of a lookup's time (`make profile PROFILE=lookup`). Nothing is kept between statements, so the next lookup reads and decodes its nodes and page again.
- **A scan reads little more than the rows themselves.** The table file takes about 50 bytes per row (4.8 MB at 100,000 rows) and a scan reads about 40 bytes for each row it visits, because rows inserted in key order fill each page before the next one is started. Rows inserted in a scattered order split pages in the middle and leave them between half and completely full.

### Transaction batching

10,000 sequential `INSERT`s are run twice: once in autocommit mode, where every statement commits its own changes; and once wrapped in a single `BEGIN TRANSACTION` / `COMMIT`, where dirty pages and nodes stay buffered until the commit. These scripts contain only the `CREATE TABLE` and the inserts, timed with the built-in timer.

| Primary key | Autocommit | Single transaction | Speedup | Inserts/sec (transaction) |
|-------------|-----------:|-------------------:|--------:|--------------------------:|
| `int` | 2.35 s | 0.062 s | 37.6× | ~160,000 |
| `text` | 2.40 s | 0.054 s | 44.6× | ~185,000 |

Every commit writes each changed object twice — once to the write-ahead log as a 4.4 KB entry, once to the table file — and syncs four times: twice for the log, once per table file, and once to truncate the log. Autocommit pays that 10,000 times where the transaction pays it once, and since 135 of the integer rows share a page, the transaction's one commit writes about 75 pages and the few nodes above them, where autocommit writes more than 20,000 pages and nodes. Nothing in the transaction run is written until the commit, but its inserts aren't free: each takes its own copy of the page it lands on and stores the changed copy back, and the first change a statement makes to a pending page or node saves the prior version so that a failed statement can be rolled back. A copy costs more the more rows the page holds, which is why the integer keys are the slower of the two in a transaction: an integer key takes 8 bytes on disk and a text key 24, so a page holds 135 of the integer rows and 78 of the text ones. On macOS a plain `fsync` is cheap (tens of microseconds here), but with four per commit, syncing is over half of the autocommit time (`make profile PROFILE=insert`); most of the rest is writing the log entries and the table file, and opening the log. With `FULL_FSYNC` set, each sync costs about 3 ms on this machine, so syncs would take nearly all of the autocommit time instead.

### Inserts at 100,000 rows

The same one-column table, sequential integer keys, and built-in timer (including `COMMIT`), in two scenarios: building a 100,000-row table from empty, and adding keys 100,001–110,000 to a table that already holds 100,000 rows (restored from a snapshot before each trial). Each figure is the median of 5 trials, except the autocommit build (3 trials of about 26 s each); no warm-up run was discarded.

| Scenario | Autocommit | Single transaction |
|----------|-----------:|-------------------:|
| Build a 100,000-row table (keys 1–100,000) | 25.8 s (~3,900 inserts/s) | 0.65 s (~153,000 inserts/s) |
| Add 10,000 rows to a 100,000-row table | 2.72 s (~3,700 inserts/s) | 0.066 s (~151,000 inserts/s) |

- **Throughput falls only a little as the table grows.** Compared with the 10,000-row runs above (~4,300 and ~160,000 inserts/s), the transaction build loses about 5% and autocommit 10 to 15%. Each autocommit insert is dominated by its own commit, and that commit grows with the depth of the tree: a key above every key in the table changes the last page and the key bound in every node above it, which is three objects once the tree has more than one leaf.
- **Nearly all of a transaction's time goes to the inserts, not the commit.** In the 100,000-row build the commit is about 5% of the run. The inserts are three quarters of it, and nearly half of the run is spent copying pages, since each insert copies the page it lands on out of the pending changes and back, all 135 rows of it once the page is full (`make profile PROFILE=insert-txn`). Every pending page and node is held in memory until the commit, and the build's memory footprint peaks at about 22 MB (`/usr/bin/time -l`).
- **Adding to an existing table in one transaction costs about the same per row as building one** (~151,000 against ~153,000 inserts/s). Keys above every key in the table all go to the last page, which is in memory once the first insert has read it and the nodes above it from disk.

### Comparison with SQLite

`make bench BENCH=sqlite` generates four workloads. Each creates a table, inserts 10,000 sequential rows (integer or text primary key, bare or wrapped in one transaction), then runs `DELETE FROM` and `DROP TABLE`. Both engines are timed the same way: wall-clock time for the whole process, including startup, with a fresh database every trial. SQLite 3.45.3 runs the identical script through its CLI on default settings: rollback journal (`journal_mode=delete`), `synchronous=FULL`, no custom pragmas.

| Benchmark | StripeSQL | SQLite | Ratio |
|-----------|----------:|-------:|------:|
| `10k.sql` (int PK, no transaction) | 2.44 s | 2.79 s | 0.9× |
| `10k_txn.sql` (int PK, single transaction) | 0.103 s | 0.019 s | 5.5× |
| `10k_str.sql` (text PK, no transaction) | 2.42 s | 2.72 s | 0.9× |
| `10k_str_txn.sql` (text PK, single transaction) | 0.081 s | 0.020 s | 4.0× |

```mermaid
xychart-beta
    title "StripeSQL vs SQLite — median seconds over 5 trials (lower is better)"
    x-axis ["10k", "10k_txn", "10k_str", "10k_str_txn"]
    y-axis "Seconds" 0 --> 4
    bar "StripeSQL" [2.44, 0.103, 2.42, 0.081]
    bar "SQLite" [2.79, 0.019, 2.72, 0.020]
```

- **Both engines give the same guarantee.** Each bare `INSERT` is its own transaction on both, and every commit — including `CREATE TABLE` and `DROP TABLE` — is durable and atomic across crashes: SQLite journals the original pages before overwriting them (rollback journal, `synchronous=FULL`), StripeSQL logs the new ones (a redo log), and neither returns until the commit is synced. Neither uses `F_FULLFSYNC` by default.
- **The autocommit runs are within about 15% of each other.** Both engines spend them syncing 10,000 commits, and these are the figures that move most from run to run.
- **In the transaction runs, the closing `DELETE FROM` is over a third of StripeSQL's time.** With integer keys it takes about 0.04 s of the ~0.10 s total, because rows are deleted one at a time, each deletion storing a changed copy of its page, and every emptied page is then removed from the B+ tree individually, with borrow/merge rebalancing along the way; SQLite erases a table's contents wholesale when `DELETE` has no `WHERE` clause. The inserts themselves take about 0.06 s (see Transaction batching). The B+ tree's linear search within internal nodes, and node structs allocated at the full configured order (`M_GLOBAL`, see `const.h`) regardless of fill, are other identified costs relative to SQLite's B-tree.
- **Text and integer keys cost SQLite the same; in a StripeSQL transaction the integer keys are slower.** Integer rows are smaller, so more of them share a page (135 against 78), and every insert and delete copies the page it changes. In autocommit the commit hides the difference.

---

## VIII. Testing and Coverage

### Running the tests

```sh
make test            # unit tests (a few seconds)
make crashtest       # crash-recovery test (macOS only, under a minute)
make coverage        # coverage report for the unit tests
make coverage-full   # coverage report for both test suites (macOS only, under a minute)
```

`make test` builds `src/run_tests.c` together with every source file except `main.c` and runs it in a temporary directory, since the tests create and delete tables and the schema file. A failed assertion aborts the run with a non-zero exit status. The `Error:` messages printed along the way come from tests that check error handling.

| Suite | Source | Covers |
|-------|--------|--------|
| Storage engine | `src/storage_engine/testing.c` | Ordering keys and their order; slotted pages; table files, headers, and dirty hashmaps; slots, page headers and nodes sized by the type of the table's keys; creating, loading, and deleting tables; B+ tree search, pages splitting (at the end, in the middle, and three ways), rows resized in place, node splits, deletion, borrowing, and merging, with the whole tree's structure checked while rows are inserted in a scattered order and deleted in order, from the middle, and at random from a three-level tree; the process lock; the write-ahead log and crash recovery, including a forked process whose commit fails after the commit point |
| SQL interpreter | `src/SQL_interpreter/testing.c` | Bytecode chunks, values, the lexer, the parser, the schema hashtable and file, the bytecode generator, the VM, and whole statements run through `interpret()`: result types, comparison operators and `LIKE`, comments, deletes with a `WHERE` clause, updates that resize rows and split pages under the scan, rows too large for a page, scans returning rows in primary-key order, keys of the wrong type, the cache of open tables, who owns the strings in results and expressions, statement rollback, and errors halting statements |
| Crash recovery | `crashtest/` | The built binary end to end, killed just before every file write and `fsync` of a commit (see *Crash-recovery test* under Usage) |

### Coverage

Measured with clang's source-based coverage (`-fprofile-instr-generate -fcoverage-mapping`, summarized by `llvm-cov`). Test code (`testing.c`, `run_tests.c`) is excluded.

| Tests | Lines | Functions | Branches |
|-------|------:|----------:|---------:|
| Unit tests (`make coverage`) | 82.7% | 92.1% | 73.6% |
| Crash-recovery test alone | 55.5% | 74.7% | 44.6% |
| Both (`make coverage-full`) | **81.7%** | **92.0%** | **72.5%** |

The unit-test row covers every file except `main.c`, which the unit tests don't link; the other two rows include it. `debug.c`, the bytecode disassembler and execution tracer used only in debug builds, never runs. Without it, both suites together cover 86% of lines and 97% of functions.

<details>
<summary>Per-file coverage (both suites)</summary>

| File | Lines | Functions | Branches |
|------|------:|----------:|---------:|
| `SQL_interpreter/chunk.c` | 100.0% | 100.0% | 100.0% |
| `SQL_interpreter/generator.c` | 81.0% | 100.0% | 72.8% |
| `SQL_interpreter/lexer.c` | 87.5% | 100.0% | 82.9% |
| `SQL_interpreter/parser.c` | 80.1% | 97.7% | 77.4% |
| `SQL_interpreter/schema.c` | 98.9% | 100.0% | 93.8% |
| `SQL_interpreter/vm.c` | 82.9% | 100.0% | 72.6% |
| `storage_engine/bplus.c` | 93.5% | 95.2% | 81.8% |
| `storage_engine/file.c` | 94.3% | 100.0% | 75.0% |
| `storage_engine/ordering.c` | 70.6% | 100.0% | 85.3% |
| `storage_engine/page.c` | 91.3% | 100.0% | 86.8% |
| `storage_engine/tableIO.c` | 90.2% | 94.7% | 76.7% |
| `storage_engine/wal.c` | 97.1% | 100.0% | 69.6% |
| `main.c` | 50.9% | 85.7% | 43.1% |
| `memory.c` | 75.0% | 100.0% | 75.0% |
| `value.c` | 76.9% | 66.7% | 100.0% |
| `debug.c` | 0.0% | 0.0% | 0.0% |

</details>

The main gaps:
- **The REPL** (`repl()` in `main.c`).
- **Code with no callers**, which no test can reach: `max` and `nodeAtMinimum` in `bplus.c`, `loadPrev`/`loadNext` and the garbage-collection stubs `moveNode`/`movePage` in `tableIO.c`, plus `getSQLType`/`encodeSQLType` in `value.c`.

---

## IX. Attributions and Outro

The interpreter architecture — bytecode chunk, stack-based VM, single-pass code generator — was inspired by Robert Nystrom's [*Crafting Interpreters*](https://craftinginterpreters.com/). The storage engine (B+ tree, slotted pages, dirty-hashmap write-back, etc.) was designed and implemented independently.

Copyright (c) 2026 Ethan Kothavale. Distributed under the MIT License — see [LICENSE.md](LICENSE.md), or the license header in any source file, for the full text.
