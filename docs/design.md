# SQL Database Engine - Architecture Design Document

## 1. Project Overview

**Goal:** Build a SQLite-like relational database engine from scratch to understand database internals.

**Language:** C++17

**Scope:** Educational project focusing on core database concepts with potential for production-quality code.

---

## 2. High-Level Architecture

```
┌─────────────────────────────────────────────┐
│         SQL Query Interface (REPL)          │
└─────────────────┬───────────────────────────┘
                  │
┌─────────────────▼───────────────────────────┐
│              SQL Parser                      │
│  ┌──────────┐      ┌──────────────┐        │
│  │  Lexer   │ ───▶ │    Parser    │        │
│  └──────────┘      └──────┬───────┘        │
└─────────────────────────────┼───────────────┘
                  │           │
                  │    ┌──────▼───────┐
                  │    │     AST      │
                  │    └──────┬───────┘
┌─────────────────▼───────────▼───────────────┐
│           Query Optimizer                    │
│  - Logical Plan                              │
│  - Physical Plan (Execution Plan)            │
└─────────────────┬───────────────────────────┘
                  │
┌─────────────────▼───────────────────────────┐
│          Execution Engine                    │
│  - Volcano Iterator Model                    │
│  - Operators: Scan, Filter, Project, Join    │
└─────────────────┬───────────────────────────┘
                  │
┌─────────────────▼───────────────────────────┐
│           Storage Manager                    │
│  ┌────────────┐  ┌────────────┐            │
│  │ Buffer Pool│  │ Disk Manager│            │
│  └────────────┘  └────────────┘            │
└─────────────────────────────────────────────┘
                  │
┌─────────────────▼───────────────────────────┐
│            Catalog Manager                   │
│  - Schema metadata                           │
│  - Table information                         │
│  - Index information                         │
└─────────────────────────────────────────────┘
```

---

## 3. Component Design

### 3.1 Type System

**Supported Types (Phase 1):**
- `INTEGER` - 4 bytes, signed
- `FLOAT` - 8 bytes, IEEE 754 double
- `VARCHAR(n)` - variable length, max n bytes
- `BOOLEAN` - 1 byte

**Value Representation:**
```cpp
class Value {
    DataType type;
    bool is_null;
    // Internal storage (union or variant)
    
    // Operations:
    // - Comparison (=, <, >, !=, <=, >=)
    // - Arithmetic (+, -, *, /)
    // - Type casting
    // - Serialization/Deserialization
};
```

### 3.2 Schema & Tuple

**Schema:**
```cpp
class Schema {
    std::vector<ColumnDefinition> columns;
    
    uint32_t GetColumnIndex(const std::string& name);
    uint32_t GetTupleSize();  // Fixed or variable
    bool IsValid(const Tuple& tuple);
};
```

**Tuple (Row):**
```cpp
class Tuple {
    std::vector<Value> values;
    Schema* schema;
    
    Value GetValue(uint32_t col_idx);
    void SetValue(uint32_t col_idx, const Value& val);
};
```

### 3.3 Lexer

**Responsibilities:**
- Tokenize SQL input string
- Identify keywords, identifiers, literals, operators
- Track line/column for error reporting

**Token Types:**
- Keywords: SELECT, FROM, WHERE, INSERT, etc.
- Identifiers: table names, column names
- Literals: numbers, strings
- Operators: =, <, >, *, +, etc.
- Delimiters: (, ), ,, ;

### 3.4 Parser

**Approach:** Recursive Descent Parser

**Output:** Abstract Syntax Tree (AST)

**AST Node Types:**
- `SelectStatement`
- `InsertStatement`
- `CreateTableStatement`
- `Expression` (binary op, unary op, literal, column ref)

**Grammar (Simplified):**
```
SelectStmt := SELECT columns FROM table [WHERE condition]
columns    := * | column_list
column_list:= identifier [, identifier]*
condition  := expression
expression := term [(AND|OR) term]*
term       := factor [compare_op factor]
factor     := identifier | literal | (expression)
```

### 3.5 Query Optimizer

**Phase 1:** Simple rule-based optimization
- Predicate pushdown (apply WHERE filters early)
- Projection pushdown (only fetch needed columns)

**Phase 2+:** Cost-based optimization (`src/optimizer/estimator.{h,cpp}`)
- Statistics: `ANALYZE [t]` scans each table once and stores its row count and, per column, the distinct non-NULL count, NULL count, min and max as a `stats` row of the schema table. Statistics are not maintained on writes; they describe the table as of the last `ANALYZE`, and estimates scale them to the current row count.
- Selectivity of one condition:

  | Condition | With statistics | Without |
  |---|---|---|
  | `col = literal` | non-NULL share / distinct | 0.1 (1 / rows on a unique index) |
  | `col <, <=, >, >=, BETWEEN` | linear interpolation over [min, max], at least 0.01 | 0.2 |
  | `IS [NOT] NULL` | NULL share | 0.1 |
  | `IN (list)` | sum of the `=` selectivities | 0.33 |
  | `LIKE` / anything else | 0.25 / 0.33 | same |

  `AND` multiplies, `OR` is p + q − pq, and `NOT` is 1 − p (independence assumed).
- Equi-join size: |L| × |R| / max(distinct(l), distinct(r)); an unanalyzed column counts as a key.
- Index selection: a unique point lookup always uses its index; any other index is used only when its conditions keep at most 25% of the rows. The defaults sit below that, so an unanalyzed database uses every applicable index.
- Join order: for inner and cross joins of three or more tables, all `ON` and `WHERE` conjuncts go into one pool. Start from the relation with the smallest filtered estimate, then keep adding the connected relation that gives the smallest result (unconnected ones last). Each conjunct is applied at the first step where its tables are available. A `LEFT JOIN` anywhere keeps the written order. `SELECT *` still lists columns in `FROM` order.
- Hash joins build on the input with the smaller estimate.

**Output:** Physical execution plan (tree of operators)

### 3.6 Execution Engine

**Model:** Volcano Iterator Model (pull-based)

**Base Operator Interface:**
```cpp
class Operator {
    virtual void Open() = 0;
    virtual Tuple Next() = 0;  // Returns next tuple or null
    virtual void Close() = 0;
};
```

**Initial Operators:**
- `SeqScan` - sequential table scan
- `Filter` - applies WHERE conditions
- `Projection` - selects columns
- `Limit` - limits results

**Phase 2 Operators:**
- `IndexScan` - uses B-tree index
- `NestedLoopJoin`, `HashJoin`, `MergeJoin`
- `Aggregate` - GROUP BY, COUNT, SUM, etc.
- `Sort` - ORDER BY

### 3.7 Storage Manager (Phase 2+)

**Page-Based Storage:**
- Fixed page size (4KB or 8KB)
- Slotted page format for variable-length tuples
- Header: page type, free space, slot count
- Slots: offset/length pairs
- Tuples: stored from end of page backward

**Buffer Pool Manager:**
- LRU or Clock replacement policy
- Pin/Unpin mechanism
- Dirty page tracking
- Write-back on eviction

**Disk Manager:**
- Read/Write pages to/from disk
- File format: header page + data pages
- Free space management

### 3.8 Catalog Manager

**Responsibilities:**
- Store metadata about tables, columns, indexes
- Persist schema information
- Provide lookup functions

**Phase 1:** In-memory catalog
**Phase 2:** Persist catalog to disk (system tables)

---

## 4. Development Phases

### Phase 0: Foundation (Week 1)
- [ ] Project structure & CMake setup
- [ ] Core data structures (Value, Schema, Tuple)
- [ ] Unit test framework
- [ ] Logging/error handling utilities

### Phase 1: Basic Query Execution (Week 2-3)
- [ ] Lexer implementation
- [ ] Parser for SELECT statements
- [ ] AST definitions
- [ ] In-memory table storage
- [ ] SeqScan, Filter, Projection operators
- [ ] Simple executor (no optimization)

**Milestone:** Execute `SELECT col1, col2 FROM table WHERE col3 > 10`

### Phase 2: DDL & DML (Week 4)
- [ ] CREATE TABLE parser & executor
- [ ] INSERT parser & executor
- [ ] DELETE, UPDATE (basic)
- [ ] Catalog management

**Milestone:** Create tables, insert data, query data

### Phase 3: Disk Storage (Week 5-6)
- [ ] Page format design
- [ ] Disk manager
- [ ] Buffer pool manager
- [ ] Persist tables to disk
- [ ] Recovery on restart

### Phase 4: Indexing (Week 7-8)
- [ ] B-tree implementation
- [ ] CREATE INDEX support
- [ ] Index scan operator
- [ ] Optimizer: index vs seq scan selection

### Phase 5: Joins & Aggregation (Week 9-10)
- [ ] JOIN parser & executor
- [ ] Nested loop join
- [ ] Hash join
- [ ] GROUP BY, aggregate functions

### Phase 6: Transactions (Week 11-12)
- [ ] Lock manager
- [ ] Transaction manager
- [ ] Write-ahead logging (WAL)
- [ ] ACID compliance

---

## 5. Project Structure

```
sql-engine/
├── CMakeLists.txt                 # Root CMake file
├── README.md                      # Project overview
├── .gitignore                     # Git ignore file
│
├── docs/
│   ├── design.md                  # Architecture design document
│   └── development.md             # Development guide
│
├── src/
│   ├── CMakeLists.txt             # Source CMake file
│   │
│   ├── common/                    # Common utilities and data structures
│   │   ├── CMakeLists.txt
│   │   ├── types.h
│   │   ├── value.h
│   │   ├── value.cpp
│   │   ├── schema.h
│   │   ├── schema.cpp
│   │   ├── tuple.h
│   │   ├── tuple.cpp
│   │   ├── logger.h
│   │   └── logger.cpp
│   │
│   ├── lexer/                     # Lexical analyzer
│   │   ├── CMakeLists.txt
│   │   ├── lexer.h
│   │   ├── lexer.cpp
│   │   ├── token.h
│   │   └── token.cpp
│   │
│   ├── parser/                    # Parser and AST
│   │   ├── CMakeLists.txt
│   │   ├── parser.h
│   │   ├── parser.cpp
│   │   ├── ast.h
│   │   └── ast.cpp
│   │
│   ├── optimizer/                 # Query optimizer
│   │   ├── CMakeLists.txt
│   │   ├── optimizer.h
│   │   └── optimizer.cpp
│   │
│   ├── execution/                 # Execution engine
│   │   ├── CMakeLists.txt
│   │   ├── executor.h
│   │   ├── executor.cpp
│   │   ├── operator.h
│   │   ├── seq_scan.h
│   │   ├── seq_scan.cpp
│   │   ├── filter.h
│   │   ├── filter.cpp
│   │   ├── projection.h
│   │   └── projection.cpp
│   │
│   ├── storage/                   # Storage layer (Phase 3+)
│   │   ├── CMakeLists.txt
│   │   ├── table.h
│   │   ├── table.cpp
│   │   ├── page.h
│   │   ├── page.cpp
│   │   ├── buffer_pool.h
│   │   ├── buffer_pool.cpp
│   │   ├── disk_manager.h
│   │   └── disk_manager.cpp
│   │
│   ├── catalog/                   # Catalog manager
│   │   ├── CMakeLists.txt
│   │   ├── catalog.h
│   │   └── catalog.cpp
│   │
│   └── main.cpp                   # REPL entry point
│
├── test/
│   ├── CMakeLists.txt
│   ├── common/
│   │   ├── value_test.cpp
│   │   ├── schema_test.cpp
│   │   └── tuple_test.cpp
│   ├── lexer/
│   │   └── lexer_test.cpp
│   ├── parser/
│   │   └── parser_test.cpp
│   └── integration/
│       └── query_test.cpp
│
├── third_party/                   # External dependencies
│   └── googletest/                # Testing framework (git submodule)
│
└── build/                         # Build directory (gitignored)
    └── (generated files)
```

---

## 6. Design Decisions & Rationale

### 6.1 Row vs Column Store
**Decision:** Row-oriented storage
**Rationale:** Simpler to implement, better for OLTP which is more common in SQLite-like use cases.

### 6.2 Execution Model
**Decision:** Volcano Iterator Model
**Rationale:** Simple, modular, easy to understand. Vectorized execution can be added later.

### 6.3 Parser Approach
**Decision:** Hand-written recursive descent
**Rationale:** Educational value, full control, no external parser generator needed.

### 6.4 Memory Management
**Decision:** Smart pointers (std::unique_ptr, std::shared_ptr)
**Rationale:** Modern C++, prevents memory leaks, clear ownership semantics.

### 6.5 Error Handling
**Decision:** Exceptions for parser/lexer errors, return codes for storage operations
**Rationale:** Exceptions for rare errors, return codes for performance-critical paths.

---

## 7. Testing Strategy

- Unit tests for each component
- Integration tests for query execution
- Performance benchmarks
- SQL compliance tests (subset of SQLite test suite)

---

## 8. Future Extensions

- Query compilation (JIT)
- Parallel query execution
- Advanced indexes (hash, GiST)
- Full-text search
- Window functions
- CTEs (Common Table Expressions)
- Foreign keys & constraints
- Views
- Stored procedures

---

## 9. References

- **SQLite Architecture:** https://www.sqlite.org/arch.html
- **CMU 15-445 Database Systems:** https://15445.courses.cs.cmu.edu/
- **Database Internals** by Alex Petrov
- **Architecture of a Database System** (Hellerstein et al.)