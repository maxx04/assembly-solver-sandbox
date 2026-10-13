"""
Regressionstest: Joint-Referenz in ein Body-Feature mit eigener Placement
("Link.BaseFeature.Edge1").

Anlass (2026-10-13, PROJ_CNC3018): in BG75 Schrauben per Teiletausch ersetzt. Die neue Schraube ist
ein PartDesign-Body, dessen BaseFeature (importierte Geometrie) um 90 Grad gedreht ist; die Joints
referenzieren "Schraube_Link" + "BaseFeature.Edge9". findPlacement() (UtilsAssembly.py) berechnet
Placement1/2 relativ zu getObject(ref) = dem Link, getGlobalPlacement(ref) lief aber den ganzen Pfad
ab und nahm die BaseFeature-Placement ein zweites Mal mit - die Schraube lag nach dem Solve um 90
Grad quer (die Kreise der beiden Joint-Seiten standen senkrecht zueinander). Jetzt endet die
Weltlage an getObject(ref) (UtilsAssembly.getJcsGlobalPlc(), AssemblyUtils
getJcsReferenceGlobalPlacement()).

Aufbau: Grundteil (geerdet, Box) und ein Link auf einen Body, dessen BaseFeature ein Zylinder mit
Placement 90 Grad um Y ist. Fixed-Joint: Box-Kante (Kreis eines Lochs gibt es nicht - eine
Box-Flaeche reicht) <-> Link + "BaseFeature.EdgeN" (Kreiskante des Zylinders), Placement2 von
FreeCAD selbst berechnet (Detach2 = False).
Geprueft: z-Achse der Joint-Seite am Zylinder liegt auf der Achse der referenzierten Kreiskante
(vor und nach dem Solve), Solve ok, Joint in der Solver-Sicht erfuellt.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_body_basefeature_reference_flat/test_body_basefeature_reference_flat.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_body_basefeature_reference_flat/build_fixture.py
"""
import math
import os
import shutil
import sys
import tempfile

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "test_originals_untouched"))

TEST_NAME = "test_body_basefeature_reference_flat"
TOL_DEG = 1e-4


def fixture_paths():
    return {s: os.path.join(THIS_DIR, "fixtures", f"{TEST_NAME}_{s}.FCStd") for s in ("body", "asm")}


def axis_angle_deg(a, b):
    d = max(-1.0, min(1.0, abs(a.dot(b)) / (a.Length * b.Length)))
    return math.degrees(math.acos(d))


def check_axis(label, joint):
    import UtilsAssembly
    ref = joint.Reference2
    jcs = UtilsAssembly.getJcsGlobalPlc(joint.Placement2, ref)
    edge = ref[0].getSubObject(ref[1][0])
    angle = axis_angle_deg(jcs.Rotation.multVec(App.Vector(0, 0, 1)), edge.Curve.Axis)
    print(f"{label}: Joint-z gegen Kantenachse {angle:.6f} Grad")
    return angle < TOL_DEG


def main():
    import test_originals_untouched as tou

    out_base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    out = os.path.splitext(out_base)[0] if out_base else os.path.join(THIS_DIR, "fcstd-output", "default")
    if os.path.isdir(out):
        shutil.rmtree(out)
    shutil.copytree(os.path.join(THIS_DIR, "fixtures"), out)

    doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_asm.FCStd"))
    asm = [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]
    joint = [o for o in doc.Objects if getattr(o, "JointType", "") == "Fixed"][0]
    print(f"Referenz: {joint.Reference2[0].Name} {joint.Reference2[1]}")
    ok = check_axis("vor dem Solve", joint)
    doc.recompute()
    rc = asm.solve()
    print(f"Solve rc={rc}")
    ok &= rc == 0
    ok &= check_axis("nach dem Solve", joint)
    with tempfile.TemporaryDirectory() as tmp:
        errors, _ = tou.check_joints_in_solver_view(asm, tmp)
    for e in errors:
        print(f"  FEHLER: {e}")
    ok &= not errors

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
