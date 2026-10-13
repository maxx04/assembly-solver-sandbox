"""
Regressionstest: Gelenkgrenzen eines Sliders gelten auch beim normalen Solve, nicht nur beim
Ziehen.

Anlass (2026-10-11, PROJ_CNC3018): BG75 wurde in BG22 per Zylinder-Fuehrung angebunden. Die
M5-Schrauben (mit Scheibe/Nutenstein, per Slider mit Grenze +-2.1mm an der Halteplatte) blieben
beim Solve in der Welt stehen und landeten 67-172mm ausserhalb ihres Langlochs. OndselSolver
prueft Grenzen nur im Drag-Schritt; AssemblyObject::solve() haengt deshalb einen Grenz-Durchgang
an runPreDrag() an.

Aufbau: BoxA (geerdet) -[Slider, Grenze LIMIT_MIN..LIMIT_MAX]- BoxB. Die Fixtur liegt bei
Gelenkwert 0. Der Test schiebt BoxB um PUSH mm entlang der Slider-Achse (wie ein Teil, das von
anderen Joints mitgenommen wurde) und loest: der Slider ist dann erfuellt, aber ausserhalb der
Grenze. Erwartet: nach dem Solve liegt der Gelenkwert wieder innerhalb der Grenze, der Slider ist
erfuellt.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_slider_limit_flat/test_slider_limit_flat.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_slider_limit_flat/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

TEST_NAME = "test_slider_limit_flat"
JOINT_TYPE_INDEX = 3  # Slider
PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=25)
PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)
LIMIT_MIN = -2.1
LIMIT_MAX = 2.1
PUSH = 60.0
TOL = 1e-6


def slider_value(boxA, boxB):
    rel = jtu.jcs_world(boxA, PLC1).inverse().multiply(jtu.jcs_world(boxB, PLC2))
    return rel


def main():
    fixture_file = jtu.fixture_path(THIS_DIR)
    out_path = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD", "/tmp/kinematic_test_slider_limit.FCStd")
    jtu.copy_fixture_to_output(fixture_file, out_path)
    doc = App.openDocument(out_path)
    boxA = jtu.get_by_label(doc, "BoxA")
    boxB = jtu.get_by_label(doc, "BoxB")
    assembly = doc.getObject("Assembly")
    doc.recompute()

    axis = jtu.jcs_world(boxA, PLC1).Rotation.multVec(App.Vector(0, 0, 1))
    plc = App.Placement(boxB.Placement)
    plc.Base = plc.Base + axis * PUSH
    boxB.Placement = plc
    print(f"Gelenkwert vor dem Solve: {slider_value(boxA, boxB).Base.z:.4f} mm "
          f"(Grenze {LIMIT_MIN}..{LIMIT_MAX})")

    rc = assembly.solve(False)
    rel = slider_value(boxA, boxB)
    value = rel.Base.z
    print(f"Gelenkwert nach dem Solve: {value:.4f} mm")
    ok = rc == 0
    if not ok:
        print(f"FEHLER: Solve rc={rc}")
    if not (LIMIT_MIN - TOL <= value <= LIMIT_MAX + TOL):
        print("FEHLER: Gelenkwert ausserhalb der Grenze - Grenzen beim Solve nicht beachtet")
        ok = False
    ok &= jtu.check_slider(jtu.report("Nach dem Solve", jtu.jcs_world(boxA, PLC1), jtu.jcs_world(boxB, PLC2)))

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
