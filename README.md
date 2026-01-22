# SQL Engine

An educational SQL database engine built from scratch in C++ to understand database internals.

## Features

### Current (Phase 0-1)
- Core data structures (Value, Schema, Tuple)
- Basic type system (INTEGER, FLOAT, VARCHAR, BOOLEAN)
- Lexer for SQL tokenization
- Simple SELECT query parsing

### Planned
- Query optimizer
- Disk-based storage with buffer pool
- B-tree indexes
- JOIN operations
- Transaction support (ACID)

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

# Initialize and update git submodules (for Google Test)
git submodule update --init --recursive

# Create build directory
mkdir build
cd build

# Configure
cmake ..

# Build
cmake --build .

# Run tests
ctest --output-on-failure

# Run the REPL
./sqlengine
```

### Build Options

```bash
# Debug build (default)
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Release build
cmake -DCMAKE_BUILD_TYPE=Release ..

# Disable tests
cmake -DBUILD_TESTS=OFF ..

# Disable logging
cmake -DENABLE_LOGGING=OFF ..
```

## Project Structure

```
sql-engine/
├── src/           # Source code
│   ├── common/    # Core data structures
│   ├── lexer/     # Tokenizer
│   ├── parser/    # SQL parser
│   ├── execution/ # Query execution
│   ├── storage/   # Storage engine
│   └── catalog/   # Metadata management
├── test/          # Unit tests
├── docs/          # Documentation
└── third_party/   # External dependencies
```

## Usage

### REPL Mode

```bash
$ ./sqlengine
sql> CREATE TABLE users (id INTEGER, name VARCHAR(50), age INTEGER);
Table created.

sql> INSERT INTO users VALUES (1, 'Alice', 25);
1 row inserted.

sql> SELECT * FROM users WHERE age > 20;
id | name  | age
---|-------|----
1  | Alice | 25
```

## Development Phases

- [x] **Phase 0**: Project setup and core data structures
- [ ] **Phase 1**: Lexer and basic parser
- [ ] **Phase 2**: In-memory query execution
- [ ] **Phase 3**: Disk-based storage
- [ ] **Phase 4**: B-tree indexes
- [ ] **Phase 5**: JOIN operations
- [ ] **Phase 6**: Transactions

## Architecture

See [docs/design.md](docs/design.md) for detailed architecture documentation.

## Testing

```bash
# Run all tests
ctest

# Run specific test
./value_test

# Run tests with verbose output
ctest --output-on-failure --verbose
```

## Contributing

This is an educational project. Feel free to:
- Report bugs
- Suggest features
- Submit pull requests

## Resources

- [SQLite Architecture](https://www.sqlite.org/arch.html)
- [CMU 15-445 Database Systems](https://15445.courses.cs.cmu.edu/)
- [Database Internals by Alex Petrov](https://www.databass.dev/)

## License

MIT License - see LICENSE file for details