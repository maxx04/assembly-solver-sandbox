# Addendum candidate for issue #32477 — 2026-09-21

**Status: NOT yet posted — draft for review before pasting into the issue.**

## Context

A new trigger for the same constraint family, found while diagnosing an unrelated report of a
part "jumping" after solve in a nested-assembly project: a **closed kinematic loop across two
independently-anchored instances of the same flexible sub-assembly**.

Structure (`NestedLoopClosureRotationAmbiguity.zip`, `Assembly_top.FCStd` +
`Assembly_fuehrung.FCStd`): a top-level `Rahmen` with three instances of a small flexible
sub-assembly (`Assembly_fuehrung`: a rail `Fuehrung` with two sliders `Halter`/`Halter001`).
Each instance's `Halter` is independently anchored to `Rahmen` via its own top-level Fixed
joint. In addition, one extra top-level Fixed joint (`Joint003`) cross-connects instance 2's
`Halter001` to instance 3's `Halter001` — closing a loop: `Rahmen → instance2.Halter → (internal
slider) → instance2.Halter001 → Joint003 (Fixed) → instance3.Halter001 → (internal slider,
reverse) → instance3.Halter004 → Rahmen`.

## Result

After `solve()`, every part downstream of the loop closure in instance 3 (`Fuehrung002`,
`Halter003`, `Halter005` — the rail itself included, which has no joint that should rotate it
at all) picks up an unexpected, non-zero rotation:

- Before solve: all three at `Yaw-Pitch-Roll=(0,0,0)`.
- After solve (patched sandbox build): `Yaw-Pitch-Roll=(65.8907, 0, 0)`, position otherwise
  plausible (rail position unchanged, both `Halter` parts slid along their rails).
- After solve (**vanilla FreeCAD**, `install-clean-test`, same file, no scripting involved
  beyond `solve()`+`recompute()`): `Yaw-Pitch-Roll=(65.8903, 0.297, -0.133)` — same dominant
  rotation, small extra off-axis components. **Confirms this is not a sandbox-patch
  regression.**

`solve()` also emits, in both builds:

```
Log: MbD: Checking for redundant constraints.
Log: MbD:     RedundantConstraintDirectionCosineConstraintIyJx
Log: MbD:     RedundantConstraintDirectionCosineConstraintIzJx
Log: MbD:     RedundantConstraintDirectionCosineConstraintIzJy
Wrn: Assembly: Solve of '...' finished with 1 redundant joint(s): Joint...
```

— the exact same `DirectionCosineConstraintI?J?` family (`FixedJoint`'s three cross-axis
orthogonality conditions, root-caused in the original issue) flagged as redundant here, now in
a loop-closure context rather than a single isolated joint.

## Why this might matter for #32477

This narrows the "only scripted `Detach1/2` + discontinuous `Placement` jump" impact
assessment further: here the ambiguity surfaces through ordinary joint topology (two anchors +
one closing joint), not through any script directly setting `Placement1`/`Placement2`. We have
**not** confirmed whether this specific fixture was built purely through the GUI or partly
through a helper script (unlike the 2026-09-13 addendum, which was a hand-built GUI corner
case) — flagging that uncertainty rather than overclaiming.

## Minimal reproduction (2 instances) + an additional, arguably more important finding

Removing the third, uninvolved instance (it isn't part of the loop at all) leaves exactly the
minimal structure: 2 independently-anchored instances + 1 closing Fixed joint
(`Assembly_top_2instances.FCStd` in the attached zip). The ambiguity still reproduces — but
**which** instance receives the spurious rotation, and by how much, changed
(`-55.4574°` on instance 2 this time, instead of `65.8907°` on instance 3), even though the
removed instance had no kinematic connection to the loop at all. This shows the root selection
is sensitive to unrelated document content/object ordering, not purely to the loop's own
geometry.

More strikingly: on this same minimal file, in the **same vanilla FreeCAD process**, two
*consecutive* solves of the *unchanged* structure landed on two *different* roots:

- The automatic solve triggered by `App.openDocument()` (document restore/recompute) produced
  `-55.4574°` on instance 2 (`Fuehrung001`/`Halter002`), instance 3 clean at `0°`.
- Our own explicit `assembly.solve()` call immediately afterward, no document change in
  between, produced `65.8907°` on instance 3 (`Fuehrung002`/`Halter005`) instead, instance 2
  now clean at `0°`.

So this isn't just "one wrong root picked once" — the same solver, same process, same
unmodified structure, picks a *different* one of (at least) two valid roots depending on which
particular solve pass is examined. That is a stronger, more concerning form of the ambiguity
than the original single-joint 180° case.

## Why this might *not* be conclusive

We have not verified analytically what the "correct" rotation should be for this specific
geometry (unlike the original issue, where 0° vs. 180° was unambiguous from symmetry) — with a
closed loop it's less obvious a priori that 0° is even the uniquely intended solution. It's
possible additional, independent geometry-specific factors contribute here. We consider it
plausible-but-unconfirmed that this is the same root cause manifesting through loop closure
rather than a distinct issue.

## Attached files

`NestedLoopClosureRotationAmbiguity.zip`:
- `Assembly_top.FCStd` — the original 3-instance find (open this one; needs
  `Assembly_fuehrung.FCStd` alongside it).
- `Assembly_top_2instances.FCStd` — the minimal 2-instance reproduction described above (also
  needs `Assembly_fuehrung.FCStd` alongside it).
- `Assembly_fuehrung.FCStd` — the shared linked sub-assembly document both of the above
  reference.

Recompute or solve the top document's `Assembly` object to reproduce; for the non-determinism
finding, compare the state right after `openDocument()` against the state right after an
explicit `solve()` call.
