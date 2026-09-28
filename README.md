# SQL Engine

An educational SQL database engine built from scratch in C++ to understand database internals.

## Features

- Core data structures (Value, Schema, Tuple)
- Type system: `INTEGER`, `FLOAT`, `VARCHAR`, `BOOLEAN`
- Lexer and full SQL parser
- Volcano/iterator query execution model
- Full DML/DDL: `CREATE TABLE`, `DROP TABLE`, `INSERT`, `SELECT`, `UPDATE`, `DELETE`
- `WHERE` clause with comparison and logical operators (`AND`, `OR`, `NOT`)
- SQL `NULL`: `NULL` literals, `IS [NOT] NULL`, three-valued logic (a comparison with `NULL` is unknown, and `WHERE` keeps only true rows), and joins never match `NULL` keys
- `INTEGER` and `FLOAT` values compare and combine numerically
- Constraints: `PRIMARY KEY`, `NOT NULL`, `UNIQUE`, `DEFAULT`, and `CREATE UNIQUE INDEX`; values are type-checked on write and `VARCHAR(n)` lengths are enforced
- `INSERT INTO t (col, ...) VALUES ...`: columns left out take their default
- Column projection and computed columns: `SELECT name, price * qty AS total ...`
- `ORDER BY` (by expression, output alias or position; `ASC`/`DESC`; NULLs first), `LIMIT` / `OFFSET`, `SELECT DISTINCT`
- Aggregates: `COUNT(*)`, `COUNT` / `SUM` / `AVG` / `MIN` / `MAX` (optionally `DISTINCT`), `GROUP BY` (expressions, positions or aliases), `HAVING` (hash aggregation). Aggregates skip NULLs. Every selected column must be grouped or aggregated. `SUM` of integers raises an error if the total overflows `INTEGER`.
- Joins: `[INNER] JOIN`, `LEFT [OUTER] JOIN`, `CROSS JOIN` and comma joins, any number of tables, any `ON` condition, and table aliases (self-joins included). Inner joins of three or more tables are reordered by estimated cost; `LEFT` joins keep the order written. Each step is a nested loop, hash or index join. `WHERE` conditions on one table filter it before the joins where that is safe.
- Subqueries: scalar `(SELECT ...)`, `[NOT] EXISTS (SELECT ...)` and `x [NOT] IN (SELECT ...)`, anywhere an expression goes (including UPDATE and DELETE). They may be correlated, using columns of the enclosing query (`WHERE EXISTS (SELECT 1 FROM orders o WHERE o.cid = c.id)`). Each subquery runs once per statement, or once per distinct set of outer values when correlated.
- Expressions: `+ - * /` (with integer overflow checks), unary minus, `LIKE` / `NOT LIKE` (`%`, `_`), `IN (...)`, `BETWEEN ... AND ...`
- Disk-resident B+tree indexes: `CREATE INDEX`, point lookups (`=`), range scans (`>`, `>=`, `<`, `<=`), maintained row by row on INSERT/UPDATE/DELETE
- Query planner: uses an index scan when an index exists on the filtered column and the estimate says it keeps few enough rows; `EXPLAIN` shows each step's estimated row count
- `ANALYZE [table]` collects per-column statistics (distinct values, NULLs, min/max) that are stored in the database file and drive the planner's estimates
- Single-file database: tables and index definitions are stored in 4 KB pages (slotted heaps + a `sqlite_master`-style schema table) behind an LRU buffer pool
- Crash safety: a write-ahead log makes every statement atomic and durable; committed work is recovered after a crash, anything uncommitted is discarded
- Interactive REPL with dot commands (`.tables`, `.schema`, `.read`, `.timer`), or run a script from stdin

- Transactions: `BEGIN` / `COMMIT` / `ROLLBACK`, with statement-level rollback inside a transaction

### Planned
- `ANALYZE` and cost-based planning (see [docs/roadmap.md](docs/roadmap.md))

## Building

### Prerequisites
- CMake 3.14 or higher
- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- Git (for fetching Google Test)

### Build Instructions

```bash
# Clone the repository
git clone <repository-url>
cd sql-engine

# Configure and build (from repo root)
cmake -B build
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Run the REPL (creates/opens mydb.db; default is sqlengine.db)
./build/src/sqlengine mydb.db
```

### Build Options

| Option | Default | Description |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` | Build type (`Debug` / `Release`) |
| `BUILD_TESTS` | `ON` | Build Google Test suites |
| `ENABLE_LOGGING` | `ON` | Enable internal logging |

```bash
# Release build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Disable tests
cmake -B build -DBUILD_TESTS=OFF

# Disable logging
cmake -B build -DENABLE_LOGGING=OFF
```

## Project Structure

```
sql-engine/
├── src/
│   ├── common/    # Value, Schema, Tuple
│   ├── lexer/     # Tokenizer
│   ├── parser/    # SQL parser + AST
│   ├── catalog/   # Catalog (schema table), Database, legacy snapshot importer
│   ├── execution/ # Operators: SeqScan, Filter, Projection, IndexScan, Executor
│   ├── storage/   # Pager, BufferPool, TablePage/TableHeap, Table, BTree
│   └── optimizer/ # (stub, planned)
├── test/
│   ├── integration/  # End-to-end SQL tests
│   └── parser/       # Parser unit tests
├── docs/
└── third_party/
```

## Usage

### REPL

```bash
./build/src/sqlengine [database-file]   # default: sqlengine.db
./build/src/sqlengine mydb.db < script.sql   # run a script: no banner or prompts
```

Statements end with `;` and may span lines. Results print as aligned tables (numbers right-aligned) with a row count. Floats print as the shortest text that reads back as the same value (`0.1`, `2.0`, `1e+20`).

Each statement is atomic and durable: it is committed to the write-ahead log (`<file>-wal`) when it succeeds and rolled back when it fails. The log is copied into the database file by checkpoints (automatically, on `save`, and on exit), and replayed on the next start if the process crashes. Only one process can have a database open at a time. A text snapshot left in `.sqlengine/` by older versions is imported automatically the first time a new database file is created.

```sql
-- DDL
CREATE TABLE users (id INTEGER PRIMARY KEY, name VARCHAR(50) NOT NULL, age INTEGER DEFAULT 0);
DROP TABLE users;

-- DML
INSERT INTO users VALUES (1, 'Alice', 25), (2, 'Bob', 30);
SELECT * FROM users WHERE age > 25 ORDER BY age DESC LIMIT 10;
SELECT name, age + 1 AS next_age FROM users WHERE name LIKE 'A%';
SELECT age, COUNT(*) AS n FROM users GROUP BY age HAVING n > 1 ORDER BY n DESC;
SELECT u.name, COUNT(o.id) FROM users u LEFT JOIN orders o ON o.user_id = u.id GROUP BY u.name;
UPDATE users SET age = 99 WHERE id = 1;
DELETE FROM users WHERE id = 2;

-- Transactions
BEGIN;
UPDATE users SET age = 26 WHERE id = 1;
DELETE FROM users WHERE id = 1;
ROLLBACK;   -- or COMMIT;

-- Indexes
CREATE INDEX idx_id ON users (id);
SELECT * FROM users WHERE id = 1;    -- uses index point lookup
SELECT * FROM users WHERE id > 1;   -- uses index range scan
```

### REPL Commands

| Command | Description |
|---|---|
| `.tables` | List tables with their columns and row counts |
| `.schema [table]` | Show the `CREATE TABLE` / `CREATE INDEX` statements for one table, or all |
| `.read FILE` | Run the SQL in a file, stopping at the first failing statement |
| `.timer on\|off` | Show how long each statement takes |
| `.save` | Checkpoint: copy the write-ahead log into the database file |
| `.help` | Show commands and SQL examples |
| `.quit` / `.exit` | Exit (an open transaction is rolled back) |

The older spellings without the dot (`tables`, `save`, `help`, `quit`) still work.

## Testing

```bash
# Build and run all tests
cmake --build build && ctest --test-dir build --output-on-failure

# Run a specific test binary
./build/test/query_test
./build/test/parser_test

# Verbose output
ctest --test-dir build --output-on-failure --verbose
```

Golden SQL tests live in `test/sql/*.test` (run by `sql_logic_test`), in a small subset of the sqllogictest format:

```
statement ok
CREATE TABLE t (id INTEGER, name VARCHAR(10))

statement error UNIQUE constraint failed
INSERT INTO t VALUES (1, 'dup')

query rowsort
SELECT id, name FROM t
----
1|ann
2|bo
```

`query nosort` compares rows in order; `rowsort` sorts both sides first. Each file runs in a fresh database. The expected results were cross-checked against SQLite; where this engine deliberately differs (strict `GROUP BY`, 32-bit `INTEGER`, errors instead of NULL for division by zero), the file says so. CI runs everything in Debug, Release (`-Wall -Wextra`) and ASan/UBSan builds.

## Development Phases

- [x] **Phase 0**: Project setup and core data structures
- [x] **Phase 1**: Lexer and parser
- [x] **Phase 2**: In-memory query execution (SeqScan, Filter, Projection)
- [x] **Phase 3**: Disk-based storage with buffer pool
- [x] **Phase 4**: BTree indexes with query planner integration
- [x] **Phase 5**: JOIN operations
  - [x] Parse `INNER JOIN ... ON ...` with qualified column references
  - [x] Execute joins via `NestedLoopJoin`
  - [x] Add rule-based join algorithm choice (`NestedLoopJoin` vs `HashJoin`)
  - [x] Support `JOIN + WHERE` (single-table pushdown + post-join filter)
  - [x] Add correctness checks (ambiguous columns, swapped `ON` sides, type-mismatch safety)
  - [x] Add `EXPLAIN` command in REPL to print physical plan (`SeqScan`/`IndexScan`/`Join` path)
  - [x] Add join-condition index matching (`IndexNestedLoopJoin` when index exists on join column)
- [x] **Phase 6**: Transactions (see M5 and M6 below)

The path to a fully working embedded database (page storage, WAL, transactions, SQL coverage) is tracked in [docs/roadmap.md](docs/roadmap.md).
- [x] **M1**: Page layer, single-file `Pager`, LRU `BufferPoolManager` with RAII `PageGuard`
- [x] **M2**: Slotted-page `TableHeap` with RIDs; tables, scans, joins and indexes run on pages
- [x] **M3**: Persistent catalog in the database file; REPL opens `sqlengine <file.db>` (replaces `.sqlengine/` text snapshots)
- [x] **M4**: On-disk B+tree indexes keyed by (value, RID), maintained incrementally
- [x] **M5**: Write-ahead log with crash recovery, checkpoints, and atomic per-statement commit/rollback
- [x] **M6**: `BEGIN` / `COMMIT` / `ROLLBACK` with savepoint-based statement rollback (single connection, so serializable)
- [x] **M7**: SQL coverage
  - [x] NULL semantics and a single shared expression evaluator
  - [x] Constraints (`PRIMARY KEY`, `NOT NULL`, `UNIQUE`, `DEFAULT`), type checking, `INSERT` column lists
  - [x] Expressions: arithmetic, computed and aliased SELECT columns, `LIKE`, `IN`, `BETWEEN` (index range scans)
  - [x] `ORDER BY`, `LIMIT` / `OFFSET`, `SELECT DISTINCT`
  - [x] Aggregates (`COUNT`/`SUM`/`AVG`/`MIN`/`MAX`), `GROUP BY`, `HAVING`
  - [x] `LEFT` / `CROSS` joins, joins of any number of tables, table aliases
  - [x] Subqueries: scalar, `EXISTS`, `IN (SELECT ...)`, correlated
- [x] **M8**: Usability and hardening
  - [x] Dot commands (`.tables`, `.schema`, `.read`, `.timer`), aligned output, float formatting, piped scripts
  - [x] Golden SQL tests (`test/sql`), sanitizer and Release CI jobs
  - [x] `ANALYZE` and cost-based planning
### Extra Goal
- [ ] **Distributed Query Processing**
## Architecture

See [docs/design.md](docs/design.md) for detailed architecture documentation.

## Resources

- [SQLite Architecture](https://www.sqlite.org/arch.html)
- [CMU 15-445 Database Systems](https://15445.courses.cs.cmu.edu/)
- [Database Internals by Alex Petrov](https://www.databass.dev/)

## License

MIT License - see [LICENSE](LICENSE) file for details
