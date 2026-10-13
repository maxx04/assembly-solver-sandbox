"""
Regressionstest: ein per Fixed gebuendeltes Teil wird allein gezogen.

Anlass (2026-10-11, PROJ_CNC3018): in BG22 die Halteplatte von BG75 gezogen - sie loeste sich
150mm vom Motor. Die Halteplatte ist per Fixed mit Motor/Gewindestange zu EINEM Solver-Koerper
gebuendelt (offsetPlc != Identitaet). preDrag() verwarf gebuendelte Teile immer; war es das
einzige gezogene Teil, blieb draggedParts leer, doDragStep() tat nichts, und das Teil folgte der
Maus ohne Solver.

Aufbau: BoxA (geerdet) -[Slider]- BoxB -[Fixed, mit Versatz]- BoxC. Gezogen wird NUR BoxC
(wie die GUI, wenn der Klick ueber ein anderes Gelenk als den Slider auf BoxC faellt), PUSH mm
entlang der Slider-Achse, in STEPS Schritten.
Erwartet: Fixed BoxB-BoxC und Slider erfuellt, BoxB hat sich mitbewegt.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_drag_bundled_part_flat/test_drag_bundled_part_flat.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_drag_bundled_part_flat/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

TEST_NAME = "test_drag_bundled_part_flat"
SLIDER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=25)  # BoxA
SLIDER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # BoxB
FIXED_PLC1 = jtu.face_jcs_placement("Face3", flip=False, extra_z_rotation_deg=40)  # BoxB
FIXED_PLC2 = jtu.face_jcs_placement("Face4", flip=True, extra_z_rotation_deg=-20)  # BoxC
PUSH = 20.0
STEPS = 10
TOL_POS = 1e-6
TOL_ROT = 1e-4


def main():
    fixture_file = jtu.fixture_path(THIS_DIR)
    out_path = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD", "/tmp/kinematic_test_drag_bundled.FCStd")
    jtu.copy_fixture_to_output(fixture_file, out_path, box_labels=("BoxA", "BoxB", "BoxC"))
    doc = App.openDocument(out_path)
    boxA = jtu.get_by_label(doc, "BoxA")
    boxB = jtu.get_by_label(doc, "BoxB")
    boxC = jtu.get_by_label(doc, "BoxC")
    assembly = doc.getObject("Assembly")
    doc.recompute()
    assembly.solve()

    axis = jtu.jcs_world(boxA, SLIDER_PLC1).Rotation.multVec(App.Vector(0, 0, 1))
    start_b = App.Placement(boxB.Placement)
    start_c = App.Placement(boxC.Placement)

    assembly.preDrag([boxC])
    for step in range(1, STEPS + 1):
        plc = App.Placement(start_c)
        plc.Base = plc.Base + axis * (PUSH * step / STEPS)
        boxC.Placement = plc
        assembly.doDragStep()
    assembly.postDrag()

    ok = True
    fixed_rel = jtu.jcs_world(boxB, FIXED_PLC1).inverse().multiply(jtu.jcs_world(boxC, FIXED_PLC2))
    dist = fixed_rel.Base.Length
    rot = fixed_rel.Rotation.Angle
    # Fixed: JCS deckungsgleich, bis auf die bekannte 180-Grad-Loesung um z (#32477)
    rot_ok = rot < TOL_ROT or abs(rot - 3.141592653589793) < TOL_ROT
    print(f"Fixed BoxB-BoxC nach dem Ziehen: {dist:.6f} mm / {rot:.6f} rad")
    if dist > TOL_POS or not rot_ok:
        print("FEHLER: BoxC hat sich von BoxB geloest (gebuendeltes Teil ohne Solver gezogen)")
        ok = False
    ok &= jtu.check_slider(jtu.report("Slider nach dem Ziehen", jtu.jcs_world(boxA, SLIDER_PLC1),
                                      jtu.jcs_world(boxB, SLIDER_PLC2)))
    moved_b = (boxB.Placement.Base - start_b.Base).Length
    print(f"BoxB mitbewegt: {moved_b:.4f} mm (erwartet {PUSH})")
    if abs(moved_b - PUSH) > 1e-3:
        print("FEHLER: BoxB ist dem gezogenen BoxC nicht gefolgt")
        ok = False

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
