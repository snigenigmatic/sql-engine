# Changelog

All notable repository changes for this implementation cycle are listed below.

## 2026-03-22 - AST + Optimizer Foundation

### Added
- `CHANGELOG.md` (this file)
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
- `src/execution/filter.h`
  - Predicate pointer changed from mutable `Expression*` to `const Expression*` for safer non-owning use.

### Fixed
- Added a type-compatibility guard in physical index planning:
  - `src/optimizer/optimizer.cpp`
  - Planner now validates `literal_value.GetType()` against indexed column `DataType` before choosing `INDEX_SCAN`.
  - On mismatch (for example `WHERE id = '2'` when `id` is `INTEGER`), planner now falls back to `SEQ_SCAN + FILTER` instead of building an index plan that can throw during BTree comparisons.

### Added Tests
- `test/parser/parser_test.cpp`
  - `BuildPhysicalPlanFallsBackToSeqScanOnTypeMismatch`
- `test/integration/query_test.cpp`
  - `IndexedPredicateTypeMismatchFallsBackSafely`

### Validation
- `cmake --build build && ctest --test-dir build --output-on-failure`
- Result: all tests passing.



### Deleted
- No files deleted in this cycle.

