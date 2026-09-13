# Addendum candidate for issue #32477 — 2026-09-13

**Status: NOT yet posted — draft for review before pasting into the issue.**

## Context

While building a real, everyday 3-part corner assembly (three 20×20 T-slot extrusions
joined at right angles) entirely through the normal Assembly GUI — no scripts, no
`Detach1`/`Detach2`, no direct `Placement1`/`Placement2` assignment, just "Create Joint" →
Fixed → click a face, click a face+vertex combo — one of the three Fixed Joints
(`Joint003`, connecting a 360 mm profile to a 320 mm profile at their corner) reproducibly
triggers OndselSolver's own redundant-constraint detector on exactly the constraint family
identified as the root cause of #32477:

```
MbD: Checking for redundant constraints.
MbD: dJointE /OndselAssembly/.../Joint003 has the following constraint(s) removed:
    RedundantConstraintDirectionCosineConstraintIyJx
Assembly: Solve of '...' finished with 1 redundant joint(s): Joint003.
```

## Why this might matter for #32477

The original issue's impact assessment concluded that normal interactive use is safe, and
that the ambiguity only manifests through a scripted, discontinuous `Placement` jump
(`Detach1/2=True` + a single large jump in starting rotation). This is the first time we've
seen the *same* constraint family (`DirectionCosineConstraintI?J?`, `FixedJoint`'s three
cross-axis orthogonality conditions) surface a warning from a **real, GUI-driven joint**,
with no scripting involved at all.

## Why this might *not* be the same bug (important caveat)

Unlike the original bug — where the solver silently converges to one of two equally valid
roots with no warning at all — here OndselSolver's own redundancy checker explicitly
*detects* one of the three cross-axis constraints as linearly dependent on the rest of the
system and drops it before solving. The remaining system then converges cleanly and
reproducibly (`solve()` returns 0; the result is bit-for-bit identical across repeated
`solve()` calls).

We verified the resulting geometry directly against the joint's own reference elements
after solving:

- Face normal of `Face3` (Profil_002) vs. `Face118` (Profil_003): dot product = **-1.0000**
  (exactly antiparallel — flush contact, as intended).
- Distance between the two reference vertices used to build the joint (`Vertex2` /
  `Vertex186`): **0.0000 mm** (exact coincidence).

So the *position* and *face-flush* part of the joint is unambiguously correct. What we have
**not** independently verified is whether the specific rotation *about* the shared face
normal is the intended one, or the spurious 180°-about-that-axis alternate root described in
the original root-cause analysis — checking that requires knowing the intended orientation
independently of the solver's own output, which we don't have a fully independent way to
confirm here.

## Attached file

`CornerJointRedundancy-RealGUI.FCStd` — the actual 3-profile corner assembly. `Joint003` is
the one that triggers the warning; recompute/solve to reproduce.

## Question for maintainers

Is the redundant-constraint detector here doing exactly what it's supposed to (safely
dropping a linearly-dependent equation in a well-posed system, unrelated to #32477's
root-uniqueness problem), or is this the same underlying ambiguity showing up for the first
time through a non-scripted, ordinary GUI workflow? If the latter, this would narrow the
"only scripted/discontinuous-jump usage is affected" conclusion from the original
investigation.
