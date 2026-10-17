# Assembly: OndselSolver NewtonRaphson::isConvergedToNumericalLimit() uses wrong index and integer division

Component: `src/3rdParty/OndselSolver` (FreeCAD/OndselSolver, issues are disabled there),
`OndselSolver/NewtonRaphson.cpp`, checked on `main` (4be80eef02).

## Problem

```cpp
auto dxNormIterNo = dxNorms->at(iterNo);
if (iterNo > 0) {
    auto dxNormIterNoOld = dxNorms->at(iterNo);          // line 115
    auto farTooLargeError = dxNormIterNo > tooLargeTol;
    auto worthIterating = dxNormIterNo > (smallEnoughTol * pow(10.0, (iterNo / iterMax) * nDecade));  // line 117
    ...
    if (dxNormIterNo < (0.5 * dxNormIterNoOld)) {
        stillConverging = true;
    }
    else {
        if (!farTooLargeError) nDivergence++;
        stillConverging = nDivergence < nDivergenceMax;
    }
```

1. Line 115 reads the current entry instead of the previous one (`iterNo - 1`). The test
   `dxNormIterNo < 0.5 * dxNormIterNoOld` is therefore never true. Every iteration with
   `dxNorm < 1e-2` is counted as a divergence, even when the step size is still halving.
2. Line 117: `iterNo / iterMax` is an integer division (both `size_t`) and always 0 for
   `iterNo < iterMax`. The `worthIterating` threshold stays at machine epsilon instead of
   relaxing with the iteration count, as the comment ("worthIterating is less stringent with
   IterNo") intends.

## Effect

Whenever `dxNorm` does not drop below machine epsilon within a few iterations (slow or
stagnating convergence), the check returns "converged" after `nDivergenceMax` (3) iterations
below `1e-2`, independent of actual progress. Well-conditioned systems are not affected, since
they reach epsilon in 1–3 iterations.

Observed in a large nested assembly (114 joints, redundant but consistent constraints): every
position solve ran exactly 5 iterations with `dxNorm` plateauing around `1e-5`, even when no
part moved between consecutive solves. The observation was made in a FreeCAD build with
additional patches; the convergence check itself is unchanged upstream.

## Suggested fix

```cpp
auto dxNormIterNoOld = dxNorms->at(iterNo - 1);
auto worthIterating = dxNormIterNo > (smallEnoughTol * pow(10.0, (double(iterNo) / double(iterMax)) * nDecade));
```

---
Found and analysed by Claude (Anthropic) while working on FreeCAD assembly performance.
