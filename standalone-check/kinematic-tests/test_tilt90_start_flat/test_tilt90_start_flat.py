"""
Eindeutige Gelenke bei genau 90 Grad gekippter Startlage (2026-11-26).

Die Kippwinkel der eindeutigen Gelenke (Fixed, Revolute, Cylindrical, Slider, Parallel, Abstand
Ebene-Ebene) waren atan2(s, c) der Achs-Komponenten. Bei genau 90 Grad Kippung ist ein Kippwinkel
atan2(0, 0) = 0: gilt als erfuellt, keine Ableitung - der Loeser strich ihn als redundant und das
Gelenk blieb STILL verletzt (rc = 0), ausserhalb von FreeCAD Absturz (FreeCADMbD-Beispiel
mateproblembobcat). Seit 2026-11-26 in dieser Naehe stereografisch 2s/(1+c)
(OndselSolver AxisAngleIeqcJeqc).

Aufbau je Fall: Quader A geerdet, Quader B um 90 Grad (bzw. 96/120 Grad) gedreht, Gelenk
Deckflaeche A - Deckflaeche B ohne Vor-Ausrichtung. Geprueft: nach dem Solve z-Achse von B = +-z
(Fixed zusaetzlich x), rc = 0. Vorher 20 von 30, jetzt 29 von 30.

BEKANNT OFFEN (zaehlt nicht als Fehler, wird gemeldet): Revolute aus 120 Grad um (1,1,1), B 25 mm
daneben - Newton laeuft auseinander, ohne die Euler-Normierung (PartFrame) loest er; Vanilla loest.
Siehe Memory todo-sandbox-fails-mbd-examples-convergence.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_tilt90_start_flat/test_tilt90_start_flat.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

V = App.Vector
OUT = os.path.join(THIS_DIR, "fcstd-output")
KNOWN = {("Revolute", "120 um (1,1,1)")}

ROTATIONS = [
    ("90 um X", App.Rotation(V(1, 0, 0), 90)),
    ("90 um Y", App.Rotation(V(0, 1, 0), 90)),
    ("90 um (1,1,0)", App.Rotation(V(1, 1, 0), 90)),
    ("120 um (1,1,1)", App.Rotation(V(0, 1, 0), 90).multiply(App.Rotation(V(0, 0, 1), 90))),
    ("96 um (0.9,-0.3,0.3)", App.Rotation(V(1, 0, 0), 90).multiply(App.Rotation(V(0, 0, 1), 37))),
]


def case(jtype, rot):
    import JointObject

    doc = App.newDocument("t90")
    doc.saveAs(os.path.join(OUT, f"{jtype}.FCStd"))
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    a = doc.addObject("Part::Box", "A")
    b = doc.addObject("Part::Box", "B")
    b.Placement = App.Placement(V(30, 0, 20), rot)
    asm.addObject(a)
    asm.addObject(b)
    doc.recompute()
    jtu.ground_object(a)
    j = jtu.make_joint(asm, JointObject.JointTypes.index(jtype), a, b,
                       App.Placement(V(5, 5, 10), App.Rotation()), App.Placement(V(5, 5, 10), App.Rotation()),
                       "Face6", "Face6")
    if jtype == "Distance":
        j.Distance = 0.0
    doc.recompute()
    rc = asm.solve()
    zb = b.Placement.Rotation.multVec(V(0, 0, 1))
    xb = b.Placement.Rotation.multVec(V(1, 0, 0))
    ok = rc == 0 and abs(abs(zb.z) - 1) < 1e-9 and (jtype != "Fixed" or abs(abs(xb.x) - 1) < 1e-9)
    App.closeDocument(doc.Name)
    return ok, rc, zb


def main():
    os.makedirs(OUT, exist_ok=True)
    failed, known = 0, 0
    for jtype in ("Distance", "Fixed", "Revolute", "Cylindrical", "Slider", "Parallel"):
        for label, rot in ROTATIONS:
            ok, rc, zb = case(jtype, rot)
            tag = "OK" if ok else ("BEKANNT OFFEN" if (jtype, label) in KNOWN else "FEHLER")
            if not ok and tag == "FEHLER":
                failed += 1
            if not ok and tag != "FEHLER":
                known += 1
            print(f"{jtype:12s} {label:22s} rc={rc} zB={tuple(round(c, 3) for c in zb)} -> {tag}")
    print(f"{30 - failed - known} von 30 erfuellt, {known} bekannt offen, {failed} Fehler")
    print("RESULT: PASS" if failed == 0 else "RESULT: FAIL")
    return 0 if failed == 0 else 1
