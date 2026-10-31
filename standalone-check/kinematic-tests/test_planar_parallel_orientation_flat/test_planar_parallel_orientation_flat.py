"""
Regressionstest Regel C8 (docs/CONSTRAINTS.md): Planar-Gelenk (Abstand Ebene-Ebene) und
Parallel-Gelenk stehen mit entgegengesetzten JCS-z-Achsen - ausser das Gelenk steht schon (bis
10 Grad) gleichsinnig (bestehendes buendiges Modell, "Ausrichtung wechseln"), dann bleibt das.

Vorher (Vanilla): OndselSolver erlaubte zJ = +-zI (zI.xJ = zI.yJ = 0), Newton landete auf der
Lage, die der Startlage naeher war - 70 Grad Start ergab "gleichsinnig".

Aufbau je Fall: BoxA (geerdet) -[Gelenk Face6 von A, Face5 von B]- BoxB. B startet so, dass die
JCS von B gegenueber der von A um THETA um x gekippt ist (0 = gleichsinnig, 180 = entgegen).
"anlegen": wie in der GUI JointObject.preSolveOpposite() (klappt B auf die Gegenseite, wenn es
gleichsinnig, aber nicht buendig steht), dann zweimal Solve. "Skript": ohne diesen Schritt - der
Solver haelt dann eindeutig die naehere Seite. Geprueft: Vorzeichen zI.zJ, Gelenk erfuellt.

Aufruf:
    ./common/run-test.sh <freecad> test_planar_parallel_orientation_flat/test_planar_parallel_orientation_flat.py
"""
import math
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

DISTANCE, PARALLEL = 5, 6
PLC1 = jtu.face_jcs_placement("Face6", extra_z_rotation_deg=20)
PLC2 = jtu.face_jcs_placement("Face5", extra_z_rotation_deg=35)
# (Gelenktyp, Name, Startwinkel, wie GUI angelegt, erwartetes Vorzeichen zI.zJ)
CASES = [
    (DISTANCE, "Abstand", 180.0, True, -1),
    (DISTANCE, "Abstand", 0.0, True, +1),
    (DISTANCE, "Abstand", 5.0, True, +1),
    (DISTANCE, "Abstand", 70.0, True, -1),
    (DISTANCE, "Abstand", 110.0, True, -1),
    (DISTANCE, "Abstand", 70.0, False, +1),
    (PARALLEL, "Parallel", 180.0, True, -1),
    (PARALLEL, "Parallel", 0.0, True, +1),
    (PARALLEL, "Parallel", 70.0, True, -1),
    (PARALLEL, "Parallel", 70.0, False, +1),
]
TOL = 1e-6


def run_case(index, jtype, name, theta, created, expected):
    path = os.path.join(THIS_DIR, "fcstd-output", f"case{index}", "case.FCStd")
    doc, asm, boxA, boxB = jtu.new_flat_two_box_assembly(f"C8case{index}", path)
    tilt = App.Placement(App.Vector(3, -2, 4), App.Rotation(App.Vector(1, 0, 0), theta))
    boxB.Placement = boxA.Placement.multiply(PLC1).multiply(tilt).multiply(PLC2.inverse())
    joint = jtu.make_joint(asm, jtype, boxA, boxB, PLC1, PLC2, "Face6", "Face5")
    # wie in der GUI: erst die Auswahl (setJointConnectors -> preSolveOpposite), dann Werte - das
    # Setzen von Distance loest sofort einen Solve aus
    if created and hasattr(joint.Proxy, "preSolveOpposite"):  # Vanilla: gibt es nicht
        joint.Proxy.preSolveOpposite(joint)
    if jtype == DISTANCE:
        joint.Distance = 0.0
    doc.recompute()
    rc = asm.solve()
    asm.solve()  # zweimal: Lage muss stabil bleiben
    jcs1 = jtu.jcs_world(boxA, PLC1)
    jcs2 = jtu.jcs_world(boxB, PLC2)
    z1 = jcs1.Rotation.multVec(App.Vector(0, 0, 1))
    z2 = jcs2.Rotation.multVec(App.Vector(0, 0, 1))
    dot = z1.dot(z2)
    dist = abs((jcs2.Base - jcs1.Base).dot(z1))
    ok = rc == 0 and abs(abs(dot) - 1.0) < TOL and (dot > 0) == (expected > 0)
    if jtype == DISTANCE:
        ok = ok and dist < 1e-5
    print(f"{name} Start {theta:5.1f} Grad ({'anlegen' if created else 'Skript'}): rc={rc}, zI.zJ={dot:+.6f} "
          f"({'gleichsinnig' if dot > 0 else 'entgegen'}), Abstand {dist:.2e}, "
          f"erwartet {'gleichsinnig' if expected > 0 else 'entgegen'} -> {'OK' if ok else 'FEHLER'}")
    App.closeDocument(doc.Name)
    return ok


def main():
    ok = True
    for i, case in enumerate(CASES):
        ok &= run_case(i, *case)
    print("RESULT: PASS" if ok else "RESULT: FAIL (siehe Ausgabe oben)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
