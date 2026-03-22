# Changelog

## 2026-03-22

### Added
- JOIN lexer/token support:
  - Added `JOIN` keyword token.
  - Added `DOT` delimiter token for qualified names.
- Parser JOIN syntax support for:
  - `SELECT ... FROM <table> JOIN <table> ON <left_col> = <right_col>;`
  - Qualified columns like `users.id`.
- New execution operator:
  - `NestedLoopJoin` (`src/execution/nested_loop_join.h/.cpp`)
- JOIN tests:
  - Lexer tokenization for JOIN + qualified columns.
  - Parser JOIN AST test.
  - Integration tests for basic INNER JOIN and `SELECT *` JOIN.

### Changed
- `SelectStatement` now stores optional join metadata:
  - `join_table`, `join_left_column`, `join_right_column`.
- `Executor`:
  - Added direct JOIN execution path using `NestedLoopJoin`.
  - Added projection resolution for qualified columns and joined output tuples.
  - Added support to strip column qualifiers (`table.col`) during expression evaluation.
- `ast` dumping now includes JOIN metadata in `SelectStatement` output.
- Optimizer currently rejects JOIN statements explicitly with clear errors (JOIN execution is handled directly in executor for now).
- Execution CMake wiring updated to compile `nested_loop_join.cpp`.

### Notes
- Current JOIN support is `INNER JOIN ... ON left_col = right_col` for one join table.
- `WHERE` with JOIN is intentionally not yet supported in execution path.

