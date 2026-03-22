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

### Deleted
- No files deleted in this cycle.

