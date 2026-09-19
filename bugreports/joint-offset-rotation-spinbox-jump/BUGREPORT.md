# FreeCAD Bugreport — Vorbereitet, zur Prüfung durch den Nutzer

**Ziel-URL zum Einreichen:** https://github.com/FreeCAD/FreeCAD/issues/new?template=1-FUNCTIONAL_PROBLEM_REPORT.yml

Noch NICHT eingereicht — dies ist der vorbereitete Text zur Kontrolle vor dem Absenden.

---

## Problem description

The Assembly workbench's Joint editing dialog has a "Rotation" ("Drehung") spinbox for a
joint's `Offset2` (and identically for `Offset1`, and the `X`/`Y`/`Z`/`Yaw`/`Pitch`/`Roll`
fields in the "advanced offset" Placement editor). Once the entered angle exceeds 180°, the
spinbox's own displayed value abruptly jumps to a large negative number instead of
continuing smoothly — e.g. typing/incrementing to 190° makes the spinbox suddenly display
**-170°**. The underlying 3D rotation itself is perfectly correct and continuous; only the
displayed number is wrong, and it stays wrong (the dialog "gets stuck" showing the wrapped
value) for any further input in that direction.

**Root cause (traced in source):** `src/Mod/Assembly/JointObject.py`,
`TaskAssemblyCreateJoint` class, uses `Base::Rotation::getYawPitchRoll()` in two places on
the same live-editing session:

```python
def onRotationChanged(self, quantity):
    if self.blockOffsetRotation:
        return
    yaw = self.jForm.rotationSpinbox.property("rawValue")
    ypr = self.joint.Offset2.Rotation.getYawPitchRoll()
    self.joint.Offset2.Rotation.setYawPitchRoll(yaw, ypr[1], ypr[2])
```

```python
def updateOffsetWidgets(self):
    ...
    self.jForm.rotationSpinbox.setProperty(
        "rawValue", self.joint.Offset2.Rotation.getYawPitchRoll()[0]
    )
```

`getYawPitchRoll()` always returns its yaw component wrapped to the canonical range
**(-180°, +180°]**. `onRotationChanged()` accepts *any* raw spinbox value the user has
typed/scrolled to (Qt spinboxes do not canonicalize input) and stores it via
`setYawPitchRoll()`. `updateOffsetWidgets()` — called after essentially every property
change/recompute while the dialog is open — then reads the value back and **overwrites**
the spinbox's own `rawValue` with the wrapped result. Any user input outside (-180°, 180°]
therefore gets silently rewritten to its wrapped equivalent the moment the widgets resync,
producing the visible jump, even though the actual 3D orientation the joint solves to
changes perfectly smoothly the whole time.

Because a joint's actual downstream orientation is typically some other placement's
rotation **combined with** this offset (e.g. for a Fixed joint whose `Placement1`/
`Placement2` come from a face normal, the moving part's own displayed angle can end up as
"90° − Offset2.yaw" or similar), the *visible* jump point from the operator's perspective is
not necessarily at ±180° for the part being moved — it can appear shifted to a different,
seemingly arbitrary pair of angles (in the real-world case that uncovered this, on a nested
flexible sub-assembly, the jump appeared to sit at 90°/270° for the moved part, purely
because of that downstream sign relationship — the underlying wrap itself is always exactly
at ±180° in `Offset2`'s own frame, as the attached script demonstrates directly).

**Impact:** anyone using the "Rotation" spinbox (or the advanced offset Placement editor) to
rotate a Fixed/Revolute/Cylindrical/etc. joint's connector past 180° in either direction hits
this — the dialog appears to "stop working" or "jump" past a certain angle, even though
nothing is actually broken about the solve itself. This can look exactly like the joint has
a hard rotation limit around some angle, when none exists.

**Attached files:**
- `OffsetRotationSpinboxJumpRepro.py` — the exact, self-contained Python script used to
  reproduce the issue (paste into the FreeCAD Python console, or run via
  `FreeCADCmd script.py`). Uses the dialog's own two functions verbatim (renamed as local
  helpers) against a real `Assembly::AssemblyObject` with a real Fixed joint — no GUI
  needed to observe the bug.
- `OffsetRotationSpinboxJumpRepro.FCStd` — the saved result of running the script.

---

## Workbench affected

Assembly

---

## Steps to reproduce

1. Open the FreeCAD Python console (or run the attached script directly).
2. Run the attached `OffsetRotationSpinboxJumpRepro.py`. It:
   - Creates two `Part::Box` objects (`BoxA`, `BoxB`) in an `Assembly::AssemblyObject`.
   - Grounds `BoxA`.
   - Creates a **Fixed** joint between `BoxA.Face2` and `BoxB.Face1`.
   - Calls the dialog's own two rotation functions (`onRotationChanged()`'s write logic and
     `updateOffsetWidgets()`'s read-back logic, copied verbatim as local helpers) in a loop,
     simulating a user incrementally turning the "Rotation" spinbox from 0° up to 359°.
   - Prints, for each step, the angle the (simulated) user typed next to what the dialog
     would display afterwards.
3. Alternatively, reproduce interactively: create any Fixed joint via the GUI, open it for
   editing, and use the "Drehung"/"Rotation" spinbox to turn it past 180° (e.g. type 190) —
   the displayed value jumps to -170.

---

## Expected behavior

The spinbox's displayed value should track the user's input continuously — either by simply
not overwriting the user's raw value with a wrapped equivalent at all, or (if some
canonical internal storage range is required) by choosing the display value **closest to
the previous display value** (i.e. unwrap relative to the last shown angle) rather than
always snapping to the fixed (-180°, 180°] window.

---

## Actual behavior

Console output from the attached script:
```
=== Simuliert: Nutzer dreht die 'Drehung'-Spinbox schrittweise inkrementell hoch ===
Nutzer tippt    0.0 deg  ->  Dialog liest zurueck und zeigt an:    0.000 deg (Abweichung:     0.0 deg)
Nutzer tippt   45.0 deg  ->  Dialog liest zurueck und zeigt an:   45.000 deg (Abweichung:     0.0 deg)
Nutzer tippt   90.0 deg  ->  Dialog liest zurueck und zeigt an:   90.000 deg (Abweichung:    -0.0 deg)
Nutzer tippt  135.0 deg  ->  Dialog liest zurueck und zeigt an:  135.000 deg (Abweichung:     0.0 deg)
Nutzer tippt  179.0 deg  ->  Dialog liest zurueck und zeigt an:  179.000 deg (Abweichung:     0.0 deg)
Nutzer tippt  180.0 deg  ->  Dialog liest zurueck und zeigt an:  180.000 deg (Abweichung:     0.0 deg)
Nutzer tippt  185.0 deg  ->  Dialog liest zurueck und zeigt an: -175.000 deg (Abweichung:  -360.0 deg)  <-- SPRUNG (Anzeige != Eingabe)
Nutzer tippt  190.0 deg  ->  Dialog liest zurueck und zeigt an: -170.000 deg (Abweichung:  -360.0 deg)  <-- SPRUNG (Anzeige != Eingabe)
Nutzer tippt  200.0 deg  ->  Dialog liest zurueck und zeigt an: -160.000 deg (Abweichung:  -360.0 deg)  <-- SPRUNG (Anzeige != Eingabe)
Nutzer tippt  225.0 deg  ->  Dialog liest zurueck und zeigt an: -135.000 deg (Abweichung:  -360.0 deg)  <-- SPRUNG (Anzeige != Eingabe)
Nutzer tippt  270.0 deg  ->  Dialog liest zurueck und zeigt an:  -90.000 deg (Abweichung:  -360.0 deg)  <-- SPRUNG (Anzeige != Eingabe)
Nutzer tippt  315.0 deg  ->  Dialog liest zurueck und zeigt an:  -45.000 deg (Abweichung:  -360.0 deg)  <-- SPRUNG (Anzeige != Eingabe)
Nutzer tippt  359.0 deg  ->  Dialog liest zurueck und zeigt an:   -1.000 deg (Abweichung:  -360.0 deg)  <-- SPRUNG (Anzeige != Eingabe)
```
(German print labels — "Nutzer tippt X" = "user types X", "Dialog liest zurueck und zeigt
an" = "dialog reads back and displays", "Abweichung" = "deviation", "SPRUNG" = "JUMP".)

Confirmed identical on a stock/vanilla FreeCAD build (unmodified `JointObject.py`) — not
related to any local patch.

---

## Development version info

- OS: Ubuntu 24.04.4 LTS, Linux 7.0.0-31-generic, x86_64
- Word size: 64-bit
- Version: 26.3.0dev
- Build type: Release (sandbox self-build)
- Branch: (detached, based on FreeCAD `main`)
- Python version: 3.12.3
- Qt version: 6.4.2
- Locale: de

---

## Last known good version

Not applicable — not reported as a regression; `getYawPitchRoll()`'s (-180°, 180°] wrap
range and `updateOffsetWidgets()`'s unconditional read-back-and-overwrite pattern both
appear to predate this investigation by a long margin.

---

## Suggested labels

Status: Needs triage, Status: Needs confirmation (per template default)

---

## Note on how this was found

Found while investigating a live user-reported "rotation doesn't work"/"advanced offsets
jump strangely" symptom on a real, multi-level-nested Assembly project (Claude Code assisted
testing session, same sandbox as the `fixed-joint-180-degree-flip` report). The user first
noticed the jump appearing to sit around 90°/270° for the part being moved; tracing the
actual `Offset2` value directly showed the true wrap point is exactly ±180°, shifted in the
operator's view by an unrelated "90° − yaw" sign relationship coming from how that
particular Fixed joint's face-normal-derived `Placement1`/`Placement2` compose with the
offset.
