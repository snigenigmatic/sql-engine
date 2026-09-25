# Roadmap: SQLite-like embedded database

## Context
Phases 0–5 are done: lexer, parser, Volcano executor, joins (NLJ/Hash/INLJ), EXPLAIN. But the engine isn't a real database yet:
- `Table` (`src/storage/table.h`) is a `std::vector<Tuple>` in memory. Data only persists when the user types `save`, which writes text files (`DiskManager::SaveCatalog`). A crash loses everything.
- `Page` and `BufferPoolManager` (`src/storage/page.h`, `buffer_pool.h`) are empty stubs.
- `BTree` is in-memory only. It stores vector positions (`row_index`), is never persisted, and `Catalog::RebuildIndexesForTable` rebuilds it after every INSERT/UPDATE/DELETE.
- There are no transactions, no NULL/constraint semantics, and no ORDER BY, GROUP BY, aggregates or LIMIT.

**Target (agreed):** a SQLite-like embedded database. It runs in a single process with a single database file, stores data in pages, survives crashes using a WAL, supports ACID transactions and covers the everyday SQL features. The REPL stays the interface.
**Order (agreed):** build the storage foundation first, then transactions, then SQL features.

Each milestone below ships as its own PR with tests. CI must stay green (`cmake --build build && ctest`).

---

## M1 — Page layer + buffer pool
- `Page`: 4 KB frame with `page_id`, pin count and dirty flag, plus typed header accessors.
- New `src/storage/pager.{h,cpp}` (or repurpose `DiskManager`) to do raw `ReadPage` / `WritePage` / `AllocatePage` on one `.db` file. Page 0 is the file header (magic, version, page count, free-list head, catalog root page).
- `BufferPoolManager`: fixed number of frames, a page table, LRU (or clock) replacement, `FetchPage` / `NewPage` / `UnpinPage` / `FlushPage` / `FlushAll`, and an RAII `PageGuard` so pages are never left pinned.
- Tests: `test/storage/buffer_pool_test.cpp` covering eviction, dirty write-back and pin safety.

## M2 — Slotted-page heap files + RID
- Tuple serialization: add binary `Serialize` / `Deserialize` to `Tuple` / `Value` (`src/common/`), including a null bitmap.
- `TablePage` (slotted page) and `TableHeap` (linked list of pages) with `InsertTuple → RID{page_id, slot}`, `GetTuple(RID)`, `UpdateTuple` (in place, or delete + insert when the row grows), `MarkDelete`, and an iterator.
- Rewrite `Table` as a thin wrapper around `TableHeap`. Replace `GetTuples()` / `row_index` users with an iterator that yields `(RID, Tuple)`. Call sites to migrate: `seq_scan.cpp`, `index_scan.cpp`, `nested_loop_join.cpp`, `hash_join.cpp`, `index_nested_loop_join.cpp`, and the DELETE/UPDATE paths in `executor.cpp` (~L598–693).
- Keep the executor's operator interfaces (`operator.h`) unchanged so the existing `query_test` suite keeps working as a regression suite.

## M3 — Persistent catalog
- Store the system tables `__tables(name, root_page, schema_blob)` and `__indexes(name, table, column, root_page)` as heap tables, with their root recorded in the page-0 header.
- `Catalog` loads from and writes to them. Remove the text-based `SaveCatalog` / `LoadCatalog` and make the `save` REPL command a no-op or checkpoint. Also make the `tables` command read from the catalog.
- DROP TABLE frees the table's pages onto the free list.

## M4 — On-disk B+tree
- Replace the `shared_ptr` nodes in `btree.{h,cpp}` with page-backed internal and leaf nodes. Leaves are linked through a `next_page_id`. Keys are fixed-width for INT/FLOAT/BOOL, and VARCHAR keys use a prefix with overflow or a length cap.
- Values become `RID`. Support duplicate keys with `(key, RID)` composite ordering.
- Implement real `Insert` / `Delete` (with split, merge and redistribute) and range iterators.
- Maintain indexes incrementally inside INSERT/UPDATE/DELETE and delete `RebuildIndexesForTable`.
- Update `IndexScan` / `IndexNestedLoopJoin` to use RIDs. The optimizer (`optimizer.cpp`) needs only signature changes.

## M5 — WAL + crash recovery (durability)
- New `src/recovery/`: `LogManager` appends physical/physiological records (page_id, offset, before/after images, txn_id, LSN) to `<db>.wal`. Each page header stores `page_lsn`.
- Enforce the WAL rule in `BufferPoolManager` (flush the log up to `page_lsn` before writing the page) and fsync on commit.
- Recovery on open does an ARIES-lite redo of committed work and undo of losers. Checkpointing is triggered by the `save` command and on clean exit.
- Tests: a crash-simulation harness (kill the process or drop the buffer pool without flushing, reopen, assert state).

## M6 — Transactions (Phase 6)
- Parser/lexer: `BEGIN`, `COMMIT`, `ROLLBACK`. Every statement runs in auto-commit mode unless it is inside an explicit transaction.
- `TransactionManager` + `Transaction` (txn_id, state, write set, undo chain through the WAL).
- Isolation: a single-writer database lock with serializable semantics, like SQLite's model. This is enough for embedded use. MVCC is out of scope.
- ROLLBACK undoes changes to both heap and index pages through log records.

## M7 — SQL completeness
Add these in small PRs, each with parser tests plus `query_test` cases:
1. **NULL semantics**: the `NULL` literal, `IS [NOT] NULL`, three-valued logic in `filter.cpp`, and NULL-aware comparisons and joins.
2. **Constraints**: `PRIMARY KEY` (auto-creates a unique index), `NOT NULL`, `UNIQUE`, `DEFAULT`, and `INSERT INTO t(cols) VALUES`.
3. **Expressions**: arithmetic in SELECT/WHERE/SET, column aliases (`AS`), `SELECT` of expressions, `LIKE`, `IN (...)` and `BETWEEN`. Generalize `SelectStatement::columns` from `vector<string>` to a vector of expressions.
4. **ORDER BY / LIMIT / OFFSET / DISTINCT**: new `Sort` (external merge sort over temporary pages when the input exceeds the buffer), `Limit` and `Distinct` operators.
5. **Aggregates**: `COUNT/SUM/AVG/MIN/MAX`, `GROUP BY`, `HAVING` and a `HashAggregate` operator.
6. **Joins**: `LEFT [OUTER] JOIN`, multi-way joins (more than two tables) and table aliases. Generalize `join_table` from `optional` to a join list.
7. **Subqueries** (stretch goal): `IN (SELECT …)`, `EXISTS` and scalar subqueries.

## M8 — Usability & hardening
- CLI: `sqlengine <file.db>`, multi-line input, `.tables` / `.schema` meta-commands and running a `.sql` script from a file or stdin.
- Pretty table output with row count and timing.
- `ANALYZE` for basic table stats (row count, distinct count), so the optimizer can choose join order and index vs. scan by cost.
- CI additions: ASan/UBSan job, a sqllogictest-style golden test runner (`test/sql/*.test`) and a small benchmark.
- Update `README.md` phases and `docs/design.md` as each milestone lands.

---

## Critical files
- Storage: `src/storage/{page,buffer_pool,disk_manager,table,btree}.{h,cpp}`, plus new `table_heap`, `table_page`, `b_plus_tree_page` and `pager` files
- Catalog: `src/catalog/catalog.{h,cpp}`
- Execution: `src/execution/*.cpp` (RID migration, new Sort/Limit/Aggregate operators), `executor.cpp`
- Front end: `src/lexer/lexer.cpp` (keywords), `src/parser/{ast.h,parser.cpp}`
- New: `src/recovery/`, `src/concurrency/` (transaction manager)
- Tests: `test/CMakeLists.txt` (`add_sqlengine_test` macro), new `test/storage/`, `test/recovery/`

## Verification (per milestone)
- `cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure` passes. The existing `query_test` must stay green throughout M1–M4 as the regression suite.
- M1–M4: run the REPL, insert about 100k rows with a buffer pool smaller than the data, restart, and confirm that SELECT and index queries return the same results.
- M5–M6: kill the process with `kill -9` in the middle of a transaction, reopen, and verify that committed rows are present and uncommitted ones are gone. `BEGIN; …; ROLLBACK;` must restore the prior state, including indexes.
- M7: golden SQL tests compared against expected outputs. sqlite3 can serve as an oracle for the expected results.

