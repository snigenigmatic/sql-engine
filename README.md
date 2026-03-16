# SQL Engine

An educational SQL database engine built from scratch in C++ to understand database internals.

## Features

- Core data structures (Value, Schema, Tuple)
- Type system: `INTEGER`, `FLOAT`, `VARCHAR`, `BOOLEAN`
- Lexer and full SQL parser
- Volcano/iterator query execution model
- Full DML/DDL: `CREATE TABLE`, `DROP TABLE`, `INSERT`, `SELECT`, `UPDATE`, `DELETE`
- `WHERE` clause with comparison and logical operators
- Column projection (`SELECT col1, col2 ...`)
- BTree index support: `CREATE INDEX`, point lookups (`=`), range scans (`>`, `>=`, `<`, `<=`)
- Query planner: automatically uses index scan when an index exists on the filtered column
- Disk-based persistence via `DiskManager`
- Interactive REPL

### Planned
- JOIN operations
- Transaction support (ACID)
- Query optimizer

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

# Run the REPL
./build/src/sqlengine
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
│   ├── catalog/   # Table and index registry
│   ├── execution/ # Operators: SeqScan, Filter, Projection, IndexScan, Executor
│   ├── storage/   # Table, BTree, DiskManager, BufferPool
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
./build/src/sqlengine
```

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

-- Indexes
CREATE INDEX idx_id ON users (id);
SELECT * FROM users WHERE id = 1;    -- uses index point lookup
SELECT * FROM users WHERE id > 1;   -- uses index range scan
```

### REPL Commands

| Command | Description |
|---|---|
| `tables` | List all tables and their columns |
| `save` | Persist all tables to disk |
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
- [ ] **Phase 5**: JOIN operations
- [ ] **Phase 6**: Transactions

## Architecture

See [docs/design.md](docs/design.md) for detailed architecture documentation.

## Resources

- [SQLite Architecture](https://www.sqlite.org/arch.html)
- [CMU 15-445 Database Systems](https://15445.courses.cs.cmu.edu/)
- [Database Internals by Alex Petrov](https://www.databass.dev/)

## License

MIT License - see LICENSE file for details
