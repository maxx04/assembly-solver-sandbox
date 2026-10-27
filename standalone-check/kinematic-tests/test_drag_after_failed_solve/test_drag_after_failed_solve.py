"""
Regressionstest: Ziehen nach gescheitertem Solve stuerzt nicht ab.

Anlass (2026-10-27, jointtype-matrix gears_coaxial__duplicate vor dem Traeger-Fix): solve()
scheiterte ("Coaxial GearJoint requires a carrier marker"), preDrag() behielt das halb aufgebaute
Solver-Modell, der erste doDragStep() stuerzte in OndselSolver ab (SIGSEGV in
DirectionCosineIeqcJec::calcPostDynCorrectorIteration). preDrag() verwirft das Modell jetzt bei
gescheitertem Solve.

Aufbau (zur Laufzeit, der Startzustand ist hier egal - nur der Solve muss scheitern): koaxiale
Zahnraeder BoxB/BoxD, deren Drehgelenke an zwei VERSCHIEDENEN geerdeten Teilen haengen (BoxA,
BoxC) - es gibt keinen gemeinsamen Traeger, solve() liefert -1. Danach preDrag/doDragStep/postDrag
wie beim Ziehen mit der Maus. Erwartet: kein Absturz, BoxD/BoxB unveraendert.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_drag_after_failed_solve/test_drag_after_failed_solve.py
"""
import os
import sys
import tempfile

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "jointtype-matrix"))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import jointtype_matrix as jtm  # noqa: E402

P = jtm.P
GEARS = 11


def main():
    d = tempfile.mkdtemp()
    doc = App.newDocument("DragAfterFail")
    doc.saveAs(os.path.join(d, "top.FCStd"))
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    a = jtm._box(doc, asm, "BoxA", d)
    doc.recompute()
    jtu.ground_object(a)
    c = jtm._box(doc, asm, "BoxC", d)
    doc.recompute()
    jtu.ground_object(c)
    parts = {}
    for label, frame in (("BoxB", a), ("BoxD", c)):
        part = jtm._box(doc, asm, label, d)
        doc.recompute()
        jtm._joint(asm, jtm.REVOLUTE, frame, part, P(0, 0, 10), jtm.BOTTOM, ("Face6", "Face5"))
        part.Placement = P(0, 0, 10).multiply(jtm.BOTTOM.inverse())
        parts[label] = part
    doc.recompute()
    b, dd = parts["BoxB"], parts["BoxD"]
    jtm._joint(asm, GEARS, b, dd, jtm.BOTTOM, jtm.BOTTOM, ("Face5", "Face5"),
               {"Distance": 10.0, "Distance2": 5.0})
    doc.recompute()

    rc = asm.solve()
    print(f"solve rc={rc} (erwartet != 0)")
    if rc == 0:
        print("RESULT: FAIL (Solve sollte scheitern - Aufbau pruefen)")
        return 1

    b_before = b.Placement
    asm.preDrag([dd])
    turned = dd.Placement.multiply(App.Placement(App.Vector(), App.Rotation(App.Vector(0, 0, 1), 6)))
    for _ in range(4):
        dd.Placement = turned
        asm.doDragStep()
    asm.postDrag()
    print("Ziehen nach gescheitertem Solve ueberlebt")

    if not b.Placement.isSame(b_before, 1e-9):
        print("RESULT: FAIL (BoxB wurde trotz gescheitertem Solve bewegt)")
        return 1
    print("RESULT: PASS")
    return 0
