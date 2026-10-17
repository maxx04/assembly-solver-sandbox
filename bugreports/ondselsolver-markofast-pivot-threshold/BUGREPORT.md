# Assembly: OndselSolver GESpMatParPvMarkoFast picks near-zero pivots (false "singular matrix")

Component: `src/3rdParty/OndselSolver` (FreeCAD/OndselSolver, issues are disabled there),
`OndselSolver/GESpMatParPvMarkoFast.cpp`, checked on `main` (4be80eef02).

## Problem

`GESpMatParPvMarkoFast::doPivoting()` selects the pivot row of column `p` only by

```cpp
criterion = aip / std::pow(2.0, spRowi->size());
```

The factor `2^rowSize` dominates the magnitude: a tiny entry in a short row beats an entry of
order 1 in a long row. Numerical leftovers from earlier elimination steps (here `2.4e-25`) can
therefore be chosen as pivot. The final check `max < singularPivotTolerance` then throws
`SingularMatrixError` although the matrix is not singular.

## Effect

`PosICNewtonRaphson` treats this as a singular matrix: `handleSingularMatrix()` runs
`lookForRedundantConstraints()` (full-pivot elimination of the whole matrix) and continues with
`GESpMatParPvPrecise`. Both are much slower than the fast solver.

Observed in a large nested assembly (114 joints, 742 position unknowns, matrix size 1471): every
solve hit a pivot of `2.4e-25` in the column of a slider joint equation that is linearly
independent of the others (checked by orthogonalising the constraint rows). This was the same
column in every solve. With the change below no singular matrix is reported any more, and the
solve time dropped from 1.4 s to 0.44 s. The observation was made in a FreeCAD build with
additional patches; the selection rule itself is unchanged upstream.

## Suggested fix

Threshold partial pivoting: only rows whose entry is at least a fraction of the largest
magnitude in the column compete on the sparsity criterion (0.1 is the usual value).

```cpp
constexpr double pivotThreshold = 0.1;
double columnMax = 0.0;
for (ssize_t k = (ssize_t)m - 1; k >= (ssize_t)p; k--) {
    auto& rowk = matrixA->at(k);
    auto it = rowk->find(p);
    if (it != rowk->end()) {
        columnMax = std::max(columnMax, std::abs(it->second));
    }
}
const double eligibleMagnitude = pivotThreshold * columnMax;
// ... then in both places where the criterion is computed:
criterion = aip >= eligibleMagnitude ? aip / std::pow(2.0, spRowi->size()) : -1.0;
```

The bookkeeping of `rowPositionsOfNonZerosInPivotColumn` stays unchanged. In our tests
(kinematic test suite, drag tests, simulation) results are identical apart from the removed
false singularity.

---
Found and analysed by Claude (Anthropic) while working on FreeCAD assembly performance.
