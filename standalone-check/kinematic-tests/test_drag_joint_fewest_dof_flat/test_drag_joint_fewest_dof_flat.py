"""
Regressionstest: Zieh-Gelenk = Bewegungsgelenk mit den wenigsten Freiheitsgraden.

Anlass (2026-10-12, PROJ_CNC3018): in BG22 sprang BG37 beim Ziehen am Traeger auf 180 Grad.
Der Traeger haengt an zwei Verbindungen zur Masse: Zylinderfuehrung zur BG75-Gewindestange
(Verschieben + Drehen) und - ueber Fixed - Slider an der Fuehrung (nur Verschieben).
findDragMode() nahm die zuerst gefundene (Zylinder), die Maus gab Drehziele vor; ab 90 Grad
landete der Solver auf der zweiten Slider-Loesung (180 Grad um die eigene Achse, #32477).
Jetzt: AssemblyObject::getDragJointOfPart() waehlt das Bewegungsgelenk mit den wenigsten
Freiheitsgraden (Fixed ueber die starre Kette verfolgt).

Aufbau: BoxA (geerdet) -[Slider]- BoxB -[Fixed]- BoxC, BoxD -[Fixed]- BoxA, dazu als ERSTES
Gelenk eine Zylinderfuehrung BoxC - BoxD, Achse parallel zur Slider-Achse, seitlich versetzt.
Geprueft:
  1. Fixtur: der Klick auf BoxC findet zuerst den Zylinder (alter Weg).
  2. getDragJointOfPart(BoxC) -> Slider an BoxB, Zylinder unter den umgangenen Verbindungen.
  3. GUI-Ziel wie vorher (Zylinder-Modus: Verschieben + Drehen um die Zylinderachse, bis 150 Grad)
     -> mit dem Slider-Modus wird nur verschoben: BoxB/BoxC verschoben, KEINE Drehung, Gelenke
     erfuellt.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_drag_joint_fewest_dof_flat/test_drag_joint_fewest_dof_flat.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_drag_joint_fewest_dof_flat/build_fixture.py
"""
import os
import sys
import tempfile

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "test_originals_untouched"))
import joint_test_utils as jtu  # noqa: E402

TEST_NAME = "test_drag_joint_fewest_dof_flat"
BOXES = ("BoxA", "BoxB", "BoxC", "BoxD")
SLD_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # BoxA
SLD_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=5)  # BoxB
FIX_BC1 = jtu.face_jcs_placement("Face3", flip=False, extra_z_rotation_deg=30)  # BoxB
FIX_BC2 = jtu.face_jcs_placement("Face4", flip=True, extra_z_rotation_deg=-10)  # BoxC
FIX_AD1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=15)  # BoxA
FIX_AD2 = jtu.face_jcs_placement("Face5", flip=True, extra_z_rotation_deg=40)  # BoxD
CYL_OFFSET = App.Vector(15, 0, 0)  # Zylinderachse seitlich zur Slider-Achse
PUSH = 12.0
STEPS = 12
TOL = 1e-6


def main():
    import test_originals_untouched as tou

    fixture_file = jtu.fixture_path(THIS_DIR)
    out_path = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD", "/tmp/kinematic_test_drag_fewest_dof.FCStd")
    jtu.copy_fixture_to_output(fixture_file, out_path, box_labels=BOXES)
    doc = App.openDocument(out_path)
    boxA, boxB, boxC, boxD = (jtu.get_by_label(doc, n) for n in BOXES)
    assembly = doc.getObject("Assembly")
    doc.recompute()
    assembly.solve()

    first, _, _ = assembly.getJointOfPartConnectingToGround(boxC)
    print(f"Erstes Gelenk zur Masse an BoxC: {first.Label} ({first.JointType})")
    if first.JointType != "Cylindrical":
        print("FEHLER: Fixtur ungeeignet - der alte Weg findet nicht zuerst den Zylinder")
        return 1

    part, joint, name, bypassed = assembly.getDragJointOfPart(boxC)
    print(f"Zieh-Gelenk: {joint.Label if joint else None} ({joint.JointType if joint else '-'}) "
          f"an {part.Label if part else None}, umgangen: {[b.Label for b in bypassed]}")
    ok = True
    if part != boxB or joint is None or joint.JointType != "Slider":
        print("FEHLER: erwartet den Slider an BoxB")
        return 1
    if first not in bypassed:
        print("FEHLER: der Zylinder fehlt unter den umgangenen Verbindungen")
        ok = False

    downstream = assembly.getDownstreamParts(part, joint, bypassed)
    drag_parts = [part] + [p for p in downstream if p != part]
    print(f"Mitgezogen: {[p.Label for p in drag_parts]}")
    if set(drag_parts) != {boxB, boxC}:
        print("FEHLER: erwartet BoxB, BoxC")
        ok = False

    # Slider-Modus wie tryMouseMove(): nur Verschieben entlang der Slider-Achse.
    axis = jtu.jcs_world(boxB, SLD_PLC2).Rotation.multVec(App.Vector(0, 0, 1))
    init = {p: App.Placement(p.Placement) for p in drag_parts}
    rot_c = App.Rotation(boxC.Placement.Rotation)
    assembly.preDrag(drag_parts)
    for step in range(1, STEPS + 1):
        for p in drag_parts:
            plc = App.Placement(init[p])
            plc.Base = plc.Base + axis * (PUSH * step / STEPS)
            p.Placement = plc
        assembly.doDragStep()
    assembly.postDrag()

    moved = (boxC.Placement.Base - init[boxC].Base).Length
    turned = (boxC.Placement.Rotation.inverted() * rot_c).Angle * 57.29577951308232
    print(f"BoxC verschoben {moved:.4f} mm (erwartet {PUSH}), gedreht {turned:.6f} Grad")
    if abs(moved - PUSH) > 1e-3 or turned > 1e-4:
        print("FEHLER: BoxC nicht sauber entlang des Sliders verschoben")
        ok = False
    with tempfile.TemporaryDirectory() as tmp:
        errors, _ = tou.check_joints_in_solver_view(assembly, tmp)
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
