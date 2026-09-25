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
- Column projection (`SELECT col1, col2 ...`)
- Disk-resident B+tree indexes: `CREATE INDEX`, point lookups (`=`), range scans (`>`, `>=`, `<`, `<=`), maintained row by row on INSERT/UPDATE/DELETE
- Query planner: automatically uses index scan when an index exists on the filtered column
- Single-file database: tables and index definitions are stored in 4 KB pages (slotted heaps + a `sqlite_master`-style schema table) behind an LRU buffer pool
- Crash safety: a write-ahead log makes every statement atomic and durable; committed work is recovered after a crash, anything uncommitted is discarded
- Interactive REPL

- Transactions: `BEGIN` / `COMMIT` / `ROLLBACK`, with statement-level rollback inside a transaction

### Planned
- SQL coverage: constraints, expressions in SELECT, `ORDER BY` / `LIMIT`, aggregates and `GROUP BY`, outer and multi-way joins (see [docs/roadmap.md](docs/roadmap.md))

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
```

Each statement is atomic and durable: it is committed to the write-ahead log (`<file>-wal`) when it succeeds and rolled back when it fails. The log is copied into the database file by checkpoints (automatically, on `save`, and on exit), and replayed on the next start if the process crashes. Only one process can have a database open at a time. A text snapshot left in `.sqlengine/` by older versions is imported automatically the first time a new database file is created.

```sql
-- DDL
CREATE TABLE users (id INTEGER, name VARCHAR(50), age INTEGER);
DROP TABLE users;

-- DML
INSERT INTO users VALUES (1, 'Alice', 25), (2, 'Bob', 30);
SELECT * FROM users WHERE age > 25;
SELECT name, age FROM users;
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
| `tables` | List all tables and their columns |
| `save` | Checkpoint: copy the write-ahead log into the database file |
| `help` | Show SQL syntax reference |
| `quit` / `exit` | Save and exit |

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
- [ ] **M7**: SQL coverage
  - [x] NULL semantics and a single shared expression evaluator
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
