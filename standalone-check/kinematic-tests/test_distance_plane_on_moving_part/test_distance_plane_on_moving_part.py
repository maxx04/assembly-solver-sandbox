"""
Regressionstest: Abstand Punkt-Ebene mit der Ebene am BEWEGLICHEN Teil konvergiert.

Anlass (2026-10-27, jointtype-matrix distance_pointfirst): OndselSolver
DispCompIecJecKeqc::calcPostDynCorrectorIteration() fuellte die Nebendiagonale der zweiten
Ableitung nach den Euler-Parametern des Bezugsrahmens K mit at(i) statt at(j). Liegt der
Bezugsrahmen (die Ebene) an einem beweglichen Teil, rechnet Newton mit falscher Hesse-Matrix und
divergiert ("iterNo > iterMax") - auch Vanilla (Weekly 2026.10.21). Ebene am geerdeten Teil war
nicht betroffen (Fixierung ueberdeckt den Fehler).

Aufbau (zur Laufzeit): BoxA geerdet (Punkt Vertex2), BoxB beweglich (Ebene Face6), Start exakt
mit Abstand 0, dann Abstand 8 und aus gedrehter Startlage. Erwartet: solve() == 0, Abstand 8.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_distance_plane_on_moving_part/test_distance_plane_on_moving_part.py
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

DISTANCE = 5


def run(label, start_perturb, distance):
    d = tempfile.mkdtemp()
    doc = App.newDocument("PlaneOnMoving")
    doc.saveAs(os.path.join(d, "top.FCStd"))
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    a = jtm._box(doc, asm, "BoxA", d)
    doc.recompute()
    jtu.ground_object(a)
    b = jtm._box(doc, asm, "BoxB", d)
    doc.recompute()
    point, plane = jtm.vjcs("Vertex2"), jtm.fjcs("Face6")
    start = a.Placement.multiply(point).multiply(plane.inverse()).multiply(start_perturb)
    b.Placement = start
    joint = jtm._joint(asm, DISTANCE, b, a, plane, point, ("Face6", "Vertex2"), {"Distance": 0.0})
    b.Placement = start
    doc.recompute()
    joint.Distance = distance
    rc = asm.solve()
    j_plane = b.Placement.multiply(plane)
    j_point = a.Placement.multiply(point)
    good, text = jtm.check_point_plane(j_plane, j_point, {"Distance": distance})
    ok = rc == 0 and good
    print(f"{label}: solve rc={rc}, {text} -> {'OK' if ok else 'FEHLER'}")
    App.closeDocument(doc.Name)
    return ok


def main():
    ok = run("exakter Start, Abstand 8", App.Placement(), 8.0)
    ok &= run("gedrehter Start, Abstand 0", jtm.B_PERTURB, 0.0)
    ok &= run("gedrehter Start, Abstand 8", jtm.B_PERTURB, 8.0)
    print("RESULT: PASS" if ok else "RESULT: FAIL")
    return 0 if ok else 1
