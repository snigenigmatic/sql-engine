# Changelog

All notable repository changes for this implementation cycle are listed below.

## 2026-03-22 - AST + Optimizer Foundation

### Added
- AST introspection APIs in `src/parser/ast.h`:
  - `ExpressionTypeToString(...)`
  - `StatementTypeToString(...)`
  - `DumpExpression(...)`
  - `DumpStatement(...)`
- AST rendering implementation in `src/parser/ast.cpp`
- Logical planning model in `src/optimizer/optimizer.h`:
  - `LogicalPlanType`
  - `LogicalPlanNode`
  - `Optimizer::BuildLogicalPlan(...)`
  - `Optimizer::ExplainLogicalPlan(...)`
- Physical planning model in `src/optimizer/optimizer.h`:
  - `PhysicalPlanType`
  - `PhysicalPlanNode`
  - `Optimizer::BuildPhysicalPlan(...)`
  - `Optimizer::ExplainPhysicalPlan(...)`
- Logical + physical planner implementation in `src/optimizer/optimizer.cpp`
- New parser/optimizer tests in `test/parser/parser_test.cpp`:
  - `DumpSelectAst`
  - `DumpUpdateAst`
  - `BuildLogicalPlanForSelect`
  - `BuildLogicalPlanForSelectStarWithoutWhere`
  - `BuildPhysicalPlanUsesIndexWhenAvailable`
  - `BuildPhysicalPlanFallsBackToSeqScanWithoutIndex`
  - `BuildPhysicalPlanFallsBackToSeqScanOnTypeMismatch`
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
  - Integration tests for `JOIN + WHERE` on left/right table columns.
  - Integration tests for swapped `ON` sides, ambiguous columns, and join-key type mismatch.

### Changed
- `src/execution/executor.h`
  - Added physical-plan execution helpers:
    - `BuildOperatorTree(...)`
    - `ResolveProjectionIndices(...)`
  - Included optimizer integration.
- `src/execution/executor.cpp`
  - Reworked `BuildPlan(...)` to use optimizer-produced physical plan.
  - Added runtime physical-plan-to-operator translation:
    - `SeqScan`, `IndexScan`, `Filter`, `Projection`.
  - Centralized projection index resolution from planned projection columns.
  - Added direct JOIN execution path using `NestedLoopJoin`.
  - Added projection resolution for qualified columns and joined output tuples.
  - Added support to strip column qualifiers (`table.col`) during expression evaluation.
  - Enabled `JOIN + WHERE` by applying `Filter` on top of join output with a join-context schema.
- `src/execution/filter.h`
  - Predicate pointer changed from mutable `Expression*` to `const Expression*` for safer non-owning use.
- `src/parser/ast.h`
  - `SelectStatement` now stores optional join metadata:
    - `join_table`, `join_left_column`, `join_right_column`.
- `src/parser/ast.cpp`
  - AST dump now includes JOIN metadata in `SelectStatement` output.
- `src/optimizer/optimizer.cpp`
  - Optimizer now builds JOIN physical plans via a `NestedLoopJoin` node.
- `src/optimizer/optimizer.h`
  - Added `NESTED_LOOP_JOIN` physical plan type and JOIN metadata fields in `PhysicalPlanNode`.
- `src/execution/executor.cpp`
  - Removed JOIN special-casing from `BuildPlan(...)`.
  - Executor now consumes optimizer JOIN physical plan nodes.
- `src/execution/CMakeLists.txt`
  - Execution CMake wiring updated to compile `nested_loop_join.cpp`.
- `src/execution/filter.cpp`
  - Added stricter qualified/unqualified column resolution over joined schemas.
  - Ambiguous unqualified filter references now fail explicitly.
- `src/execution/nested_loop_join.cpp`
  - Join-key type mismatch now safely skips non-comparable pairs.

### Fixed
- Added a type-compatibility guard in physical index planning:
  - `src/optimizer/optimizer.cpp`
  - Planner now validates `literal_value.GetType()` against indexed column `DataType` before choosing `INDEX_SCAN`.
  - On mismatch (for example `WHERE id = '2'` when `id` is `INTEGER`), planner now falls back to `SEQ_SCAN + FILTER` instead of building an index plan that can throw during BTree comparisons.

### Validation
- `cmake --build build && ctest --test-dir build --output-on-failure`
- Result: all tests passing.

### Notes
- Current JOIN support is `INNER JOIN ... ON left_col = right_col` for one join table.
- JOIN planning is still executor-driven; optimizer JOIN nodes are the next step.
- JOIN planning now flows through optimizer physical plans; next step is join algorithm selection and cost-based choices.
