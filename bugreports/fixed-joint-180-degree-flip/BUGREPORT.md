# FreeCAD Bugreport — Vorbereitet, zur Prüfung durch den Nutzer

**Ziel-URL zum Einreichen:** https://github.com/FreeCAD/FreeCAD/issues/new?template=1-FUNCTIONAL_PROBLEM_REPORT.yml

Noch NICHT eingereicht — dies ist der vorbereitete Text zur Kontrolle vor dem Absenden.

---

## Problem description

The Assembly workbench's **Fixed Joint** can settle into a configuration rotated by
**exactly 180°** about the joint's shared axis after solving, instead of matching the
orientation actually specified by the joint's `Placement1`/`Placement2` connector
placements. The position is solved correctly (sub-micron accuracy); only the rotation is
wrong, by exactly 180°.

This is fully deterministic and reproducible — it does **not** depend on how far the part
was moved before solving (tested from 5 mm to 999 mm of positional error, all producing
the identical wrong 180°-rotated result), which rules out it being an ordinary
Newton-convergence/starting-guess issue. It occurs specifically when the two joint
connectors' local Z axes are configured to point in **opposite** directions — which is
exactly the configuration produced whenever two faces are joined so they sit flush against
each other rather than overlapping (a completely normal, everyday Assembly use case: e.g.
joining `Face1` of one part to `Face1` of a mating part positioned next to it).

**Root cause (traced in source):** `src/3rdParty/OndselSolver/OndselSolver/FixedJoint.cpp`,
`FixedJoint::initializeGlobally()`, constrains the relative rotation using exactly three
cross-axis orthogonality constraints:

```cpp
addConstraint(CREATE<DirectionCosineConstraintIqcJqc>::ConstraintWith(frmI, frmJ, 1, 0)); // Y_I · X_J = 0
addConstraint(CREATE<DirectionCosineConstraintIqcJqc>::ConstraintWith(frmI, frmJ, 2, 0)); // Z_I · X_J = 0
addConstraint(CREATE<DirectionCosineConstraintIqcJqc>::ConstraintWith(frmI, frmJ, 2, 1)); // Z_I · Y_J = 0
```

These three equations do **not** uniquely determine the relative orientation. Given each
frame's own orthonormality, **both** of the following satisfy all three equations equally
well:

- `X_J = X_I, Y_J = Y_I, Z_J = Z_I` (the intended, correct solution)
- `X_J = -X_I, Y_J = -Y_I, Z_J = +Z_I` (a spurious solution: a 180° rotation about the
  shared Z axis)

Newton's method converges to whichever root is numerically closer to the current/starting
orientation — independent of position, which explains why the offset magnitude (5–999 mm)
never changes the outcome, only the *starting rotation* does. For the "faces flush against
each other" (flipped-Z) configuration that real face-to-face assemblies require, the
spurious root frequently ends up closer to a neutral starting orientation than the correct
one, so the solver reliably picks the wrong branch.

The same structural weakness exists in `RevoluteJoint`/`CylindricalJoint`
(`RevoluteJoint.cpp`/`CylindricalJoint.cpp`), which use only two such constraints
(`Z_I·X_J=0`, `Z_I·Y_J=0`), leaving `Z_J = ±Z_I` undetermined — for these joint types this
is admittedly less severe since the shared axis itself is still correctly found and a
0°/180° ambiguity along the *free* rotational DOF is arguably tolerable, but it is worth
being aware the same class of degeneracy is present there too.

**Impact:** any Fixed Joint between two faces that must sit flush against each other (the
standard way two parts are joined face-to-face) can silently end up mirrored/flipped by
180° after a solve, with no error or warning — this can misplace parts in ways that are not
obviously wrong at a glance (position is exactly right), and would affect anyone using Fixed
joints between face-to-face mated parts, especially after dragging/moving a part and
re-solving.

**Attached files:**
- `FixedJoint180BugRepro.FCStd.zip` — the saved result of running the script below,
  showing the wrong (180°-flipped) BoxB placement.
- `FixedJoint180BugRepro.py` — the exact, self-contained Python script used to reproduce
  the issue (paste into the FreeCAD Python console, or run via `FreeCAD script.py`).

---

## Workbench affected

Assembly

---

## Steps to reproduce

1. Open the FreeCAD Python console (or run the attached script directly).
2. Run the attached `FixedJoint180BugRepro.py`. It:
   - Creates two `Part::Box` objects (`BoxA`, `BoxB`) in an `Assembly::AssemblyObject`.
   - Grounds `BoxA`.
   - Creates a **Fixed** joint between `BoxA.Face1` and `BoxB.Face1`, with `Detach1`/
     `Detach2 = True` and explicit `Placement1`/`Placement2` values whose local Z axes
     point in **opposite** directions (`Placement1`'s Z axis = `Face1`'s real outward
     normal `(-1,0,0)`; `Placement2`'s Z axis = the *opposite* direction `(1,0,0)`) — this
     is the standard "flip" needed so two faces end up flush against each other instead of
     overlapping (see `UtilsAssembly.flipPlacement()`/`arePlacementSameDir()`, which exist
     precisely for this purpose during normal face-to-face joint creation).
   - Sets `BoxB.Placement` to an arbitrary "wrong" starting placement
     (`Vector(999,-999,999)`, identity rotation) to simulate a user having dragged the part.
   - Calls `assembly.solve(False)`.
   - Prints the analytically-expected `BoxB.Placement` (computed directly from
     `Placement1`/`Placement2`, pure placement algebra, no solver involved) next to the
     actual solved result, and the position/rotation difference.
3. Compare the two placements printed at the end.

Also reproducible with an arbitrarily *small* starting displacement (verified down to
5 mm) — the specific value in the script (999 mm) is not required to trigger it.

**Note on `Detach1`/`Detach2`:** the script sets these to `True` purely so it can assign
`Placement1`/`Placement2` directly in Python without them being immediately recomputed from
`Reference1`/`Reference2` geometry on the next recompute (see
`JointObject.py::updateJCSPlacements()` — `Detach1`/`2` only gate that one recompute step;
they have no effect on, and are not read by, the solver itself). `Detach` is **not** required
to trigger the bug and is not part of the root cause — it is only a convenient way to force
the exact "opposite Z axes" connector configuration deterministically from a script. The
identical configuration arises in completely ordinary interactive use with `Detach1`/`2` left
at their default `False`: selecting two faces so they end up flush against each other (the
everyday case) already produces connector placements with opposite Z axes via
`UtilsAssembly.flipPlacement()`/`arePlacementSameDir()`, so normal face-to-face Fixed joints
can hit this same ambiguity without the user ever touching `Detach`.

---

## Expected behavior

`BoxB.Placement` after solving should exactly equal `BoxA.Placement * Placement1 *
Placement2.inverse()` (this is the unique, unambiguous solution required for `Placement1`
and `Placement2` — a Fixed joint's connectors, by definition, should have zero degrees of
freedom relative to each other). Position and rotation should both match to solver
tolerance.

---

## Actual behavior

The position matches exactly (sub-`1e-9` mm). The **rotation is off by exactly 180°**
about the joint's shared axis — reproducibly, deterministically, on every run, regardless
of how large the initial position/rotation error was before solving (tested 5 mm through
999 mm, identical result every time).

Console output from the attached script:
```
BoxB.Placement (aus Placement1/Placement2 analytisch erwartet): Placement [Pos=(2.22045e-15,0,10), Yaw-Pitch-Roll=(180,-2.54444e-14,180)]
BoxB.Placement (tatsaechlich vom Solver berechnet):            Placement [Pos=(-2.22045e-15,0,0), Yaw-Pitch-Roll=(0,2.54444e-14,0)]
Positions-Differenz: 10.000000 mm
Rotations-Differenz: 180.000000 deg  <-- should be 0
```
(German print labels — the important columns are: expected vs. actual placement, and the
computed rotation difference of exactly 180°.)

---

## Development version info

Reproduced on a **from-source build of the current `main` branch**, run with `--safe-mode`
(addons/macros/user configuration disabled) to rule out any addon or user-configuration
interference. Confirmed identical result in Safe Mode as in normal mode.

- OS: Ubuntu 24.04.4 LTS, Linux 7.0.0-31-generic, x86_64
- Word size: 64-bit
- Version: 26.3.0dev
- Build type: Release (sandbox self-build, `--safe-mode` verified)
- Branch: (detached, based on FreeCAD `main`)
- Hash: `bafe119010358a8f30207efbaa94c967090dde79`
- Build revision: 48260 (Git), build date 2026/08/25
- Python version: 3.12.3
- Qt version: 6.4.2
- Coin version: 4.0.2
- OCC version: 7.6.3
- Locale: de

*(This was tested against a locally-built checkout close to `main`, not an official weekly
build — please let me know if you need me to additionally verify against an official
weekly-build binary and I will do so before/after filing.)*

---

## Last known good version

Not applicable — this is not reported as a regression; the constraint-formulation issue in
`FixedJoint::initializeGlobally()` appears to be present as far back as the join/constraint
code shown above goes (unrelated to any specific FreeCAD version), and has not been
observed to work correctly in this exact "flipped-Z" configuration in any tested version.

---

## Suggested labels

Status: Needs triage, Status: Needs confirmation (per template default)

---

## Note on how this was found

Found and root-caused via a Claude Code assisted testing session while building an
independent Assembly-solver regression test suite (github.com/maxx04's FreeCAD Assembly
sandbox). Happy to share the fuller test matrix / additional repro variants (including the
same class of ambiguity affecting `RevoluteJoint`/`CylindricalJoint`) if useful.
