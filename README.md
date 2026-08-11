StripeSQL is a relational database management system written from scratch in C, using only the C standard library. It supports a SQL front end backed by a custom B+ tree storage engine with slotted-page file format.

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
  │        └───────────┘                                     │
  └──────────────────────────────────────────────────────────┘

  Schema (.scma) loaded by schema.c is shared between the Generator
  and VM to resolve column names and primary key positions.
```

**Lexer** (`src/SQL_interpreter/lexer.c`) — scans the raw query string into a flat token array. Multi-word clauses such as `INSERT INTO` are matched at the parse level, not here.

**Parser** (`src/SQL_interpreter/parser.c`) — consumes the token array and produces an AST. Each node carries a type tag, a keyword token, a flag for sub-variants (e.g. `DISTINCT`, `PRIMARY KEY`), and up to seven children.

**Generator** (`src/SQL_interpreter/generator.c`) — walks the AST and emits a bytecode `chunk` against the schema. Detects primary-key equality in `WHERE` clauses and emits `OP_KEY_SEARCH` instead of a scan loop.

**VM** (`src/SQL_interpreter/vm.c`) — stack-based interpreter that executes the bytecode chunk. Maintains up to four concurrent scanners, each holding a cursor into a B+ tree (`src/storage_engine/scanner.h`). A session-scoped transaction registry, kept outside the per-statement VM state, lets `BEGIN TRANSACTION` hold a table open — dirty writes uncommitted — across multiple statements until `COMMIT` or `DISCARD`.

**B+ tree** (`src/storage_engine/bplus.c`) — the index structure that maps page numbers to disk addresses. Leaf nodes link bidirectionally for sequential scans.

**Slotted page** (`src/storage_engine/page.c`) — variable-length records are stored in slotted pages. Each slot holds an offset key, a pointer into the entry array, and a byte length.

**Table I/O** (`src/storage_engine/tableIO.c`) — serialises pages and nodes to `.tbl` files in the `tables/` directory. Writes are buffered in dirty stacks and flushed to disk on `commit()`.

---

## II. Supported Features

**Data Definition**
- `CREATE TABLE name (col type [PRIMARY KEY], ...)` — creates a new table and registers it in the schema
- `DROP TABLE name` — deletes the table file and removes the schema entry
- Column types: `int`, `text`
- `PRIMARY KEY` constraint on a single column per table

**Data Manipulation**
- `INSERT INTO table VALUES (v1, v2, ...)` — inserts a row; the primary key is converted to an internal page number via a reversible ordering transform (not a hash). String literals must be single-quoted (`'...'`); double-quoted or unquoted string values are reported as an error at compile time instead of being silently misparsed.
- `SELECT * FROM table` and `SELECT col1, col2, ... FROM table`
- `SELECT DISTINCT ...` — deduplicates result rows
- `UPDATE table SET col = expr [WHERE ...]`
- `DELETE FROM table [WHERE ...]`

**Transactions**
- `BEGIN TRANSACTION` — starts a transaction; every table touched by a subsequent statement stays open, with its writes buffered but not flushed to disk
- `COMMIT` — writes all changes made since `BEGIN TRANSACTION` to disk and closes the tables
- `DISCARD` — drops all changes made since `BEGIN TRANSACTION` without writing anything to disk
- A single transaction may span multiple tables
- `BEGIN TRANSACTION` while already in a transaction, or `COMMIT`/`DISCARD` with none active, reports an error and is a no-op

**Filtering and Expressions**
- `WHERE` clause with `=`, `!=`, `<`, `<=`, `>`, `>=`
- `AND`, `OR`, `NOT` logical operators with correct precedence
- `LIKE` pattern matching
- `IS NULL` and `IS NOT NULL`
- Arithmetic: `+`, `-`, `*`, `/`, unary `-`

**Result Set Operations**
- `ORDER BY col [ASC | DESC]`
- `LIMIT n`

**Query Optimization**
- Primary key equality (`WHERE pk_col = literal`) uses `OP_KEY_SEARCH` — a direct B+ tree lookup — instead of a full table scan, in `SELECT`, `UPDATE`, and `DELETE` statements alike

**Execution Modes**
- Interactive REPL (`./main`)
- Batch file execution (`./main file.sql`) supporting multiple semicolon-delimited statements
- Single-line comments (`-- ...`) and block comments (`/* ... */`) in SQL files

---

## III. Roadmap

The following features are next on the todo list, roughly in priority order:

1. **Column reordering in queries** — `INSERT INTO t (b, a) VALUES (2, 1)` and `SELECT b, a FROM t` with non-natural column ordering are not yet handled.
2. **File-level garbage collection** — `condenseStripe` and `condenseAll` are stubbed in `tableIO.c`; implementing them will reclaim space from deleted records.
3. **Propagate I/O errors** — `readNode` and `readPage` currently do not propagate failure to callers.
4. **Merge schema and hash table** — the schema struct and the in-memory hash table currently maintain separate representations of column metadata; unifying them will simplify the generator and VM.
5. **File structure analysis mode** - a new mode which creates a new database populated with the attributes of a given directory's files and subdirectories.

---

## IV. Known Issues

| # | Description |
|---|-------------|
| 1 | Entering a blank line in the REPL causes a segfault. As a workaround, always enter a valid SQL statement or `Ctrl-D` to exit. |
| 2 | `readNode` and `readPage` silently swallow I/O errors instead of returning a failure code to the caller. |
| 3 | Column reordering in `INSERT` and `SELECT` is not supported — column order in a query must match the order declared in `CREATE TABLE`. |
| 4 | A fatal error partway through a transaction (e.g. a compile error, which calls `exit()`) does not auto-`DISCARD` — the transaction's open table handles are simply leaked without committing or writing back. |
| 5 | There is no page overflow policy. Database records are dispersed across but if too many records are assigned to a page, insertion becomes impossible until the page is emptied. |

---

## V. Usage

### Build

```sh
make
```

This compiles all source files with `clang` and produces the `main` binary in the project root. Requires `clang` and `make`.

### Run the REPL

```sh
./main
```

Type SQL statements ending with `;` and press Enter. Press `Ctrl-D` to exit. Do not enter blank lines (see Known Issues).

### Run a SQL file

```sh
./main file.sql
```

All statements in the file are executed in order. The total wall-clock time is printed after the last statement. Exit codes mirror the POSIX convention: `65` for a compile error, `70` for a runtime error, `60` for a load error.

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
Add a `case` to the dispatch loop in `run()`. Operations that touch the disk go through the scanner and the B+ tree API in `bplus.c`.

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

The table below shows representative timings on an Apple M-series chip for a single `users` table with `(id int PRIMARY KEY, name text)`.

| Operation | Rows | Time |
|-----------|------|------|
| Sequential INSERT | 100 | ~2 ms |
| Full table scan SELECT | 100 | ~1 ms |
| Primary key lookup SELECT | — | < 0.5 ms |

These figures are rough baselines; performance will vary with page fill factor, tree depth, and disk speed. No formal benchmark harness exists yet.

---

## VIII. Attributions and Outro

The interpreter architecture — bytecode chunk, stack-based VM, single-pass code generator — was inspired by Robert Nystrom's [*Crafting Interpreters*](https://craftinginterpreters.com/). The storage engine (B+ tree, slotted pages, dirty-stack write-back, etc.) was designed and implemented independently.

Copyright (c) 2026 Ethan Kothavale. Distributed under the MIT License — see the license header in any source file for the full text.
