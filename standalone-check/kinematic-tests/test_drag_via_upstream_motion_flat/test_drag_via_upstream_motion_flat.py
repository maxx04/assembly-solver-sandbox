"""
Regressionstest: Ziehen an einem Teil, das ueber ein Gelenk OHNE eigenen Zieh-Modus (Distance)
mit Masse verbunden ist, zugleich aber starr an einem Bewegungsgelenk haengt.

Anlass (2026-10-11, PROJ_CNC3018): in BG22 liess sich BG75 an der Halteplatte nicht um die
Gewindestange drehen. Klick auf die Halteplatte -> Joint010 (Distance zum Rahmen), also nur freies
Verschieben. Die Drehung gibt die Zylinderfuehrung an der Gewindestange vor (Halteplatte - Motor -
Buchse - Stange per Fixed). Zwei Teile des Fixes (wie ViewProviderAssembly::findDragMode()):
  1. getDragJointOfPart(teil) findet das Bewegungsgelenk ueber die Fixed-Kette (2026-10-12:
     allgemein das mit den wenigsten Freiheitsgraden, vorher getUpstreamMotionJoint()).
  2. getDownstreamParts(..., umgangene): das Distance-Gelenk zaehlt nicht als Erdung, sonst
     werden die am Block haengenden Teile nicht mitgezogen und der Drag-Solver haelt alles fest.

Aufbau: BoxA (geerdet) -[Revolute]- BoxB -[Fixed]- BoxC -[Slider]- BoxD
        BoxC -[Distance Punkt-in-Ebene, Ebene senkrecht zur Drehachse]- BoxA (verträglich mit
        der Drehung, als ERSTES Gelenk angelegt, damit der Klick auf BoxC es zuerst findet).
Geklickt wird BoxC, gedreht wird um die Revolute-Achse in STEPS Schritten bis ANGLE Grad.
Erwartet: BoxB/BoxC/BoxD drehen sich mit, alle Gelenke erfuellt.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_drag_via_upstream_motion_flat/test_drag_via_upstream_motion_flat.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_drag_via_upstream_motion_flat/build_fixture.py
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

TEST_NAME = "test_drag_via_upstream_motion_flat"
BOXES = ("BoxA", "BoxB", "BoxC", "BoxD")
REV_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # BoxA
REV_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=5)  # BoxB
FIX_PLC1 = jtu.face_jcs_placement("Face3", flip=False, extra_z_rotation_deg=30)  # BoxB
FIX_PLC2 = jtu.face_jcs_placement("Face4", flip=True, extra_z_rotation_deg=-10)  # BoxC
SLD_PLC1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=15)  # BoxC
SLD_PLC2 = jtu.face_jcs_placement("Face5", flip=True, extra_z_rotation_deg=40)  # BoxD
ANGLE = 30.0
STEPS = 10
TOL_ANGLE = 1e-3


def main():
    import test_originals_untouched as tou

    fixture_file = jtu.fixture_path(THIS_DIR)
    out_path = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD", "/tmp/kinematic_test_drag_upstream.FCStd")
    jtu.copy_fixture_to_output(fixture_file, out_path, box_labels=BOXES)
    doc = App.openDocument(out_path)
    boxA, boxB, boxC, boxD = (jtu.get_by_label(doc, n) for n in BOXES)
    assembly = doc.getObject("Assembly")
    doc.recompute()
    assembly.solve()

    ok = True
    clicked_joint, _, _ = assembly.getJointOfPartConnectingToGround(boxC)
    print(f"Klick auf BoxC -> {clicked_joint.Label} ({clicked_joint.JointType})")
    if clicked_joint.JointType != "Distance":
        print("FEHLER: Fixtur ungeeignet - der Klick findet nicht zuerst das Distance-Gelenk")
        return 1
    part, joint, name, bypassed = assembly.getDragJointOfPart(boxC)
    print(f"Bewegungsgelenk: {joint.Label if joint else None} an {part.Label if part else None}, "
          f"umgangen: {[b.Label for b in bypassed]}")
    if part != boxB or joint is None or joint.JointType != "Revolute":
        print("FEHLER: getDragJointOfPart() findet die Revolute an BoxB nicht")
        return 1
    if clicked_joint not in bypassed:
        print("FEHLER: das Distance-Gelenk fehlt in der Liste der umgangenen Verbindungen")
        return 1
    downstream = assembly.getDownstreamParts(part, joint, bypassed)
    drag_parts = [part] + [p for p in downstream if p != part]
    print(f"Mitgezogen: {[p.Label for p in drag_parts]}")
    if set(drag_parts) != {boxB, boxC, boxD}:
        print("FEHLER: erwartet BoxB, BoxC, BoxD")
        ok = False

    jcs = jtu.jcs_world(boxB, REV_PLC2)
    init = {p: App.Placement(p.Placement) for p in drag_parts}
    start_d = App.Placement(boxD.Placement)
    assembly.preDrag(drag_parts)
    for step in range(1, STEPS + 1):
        turn = jcs.multiply(App.Placement(App.Vector(), App.Rotation(App.Vector(0, 0, 1), ANGLE * step / STEPS)))
        turn = turn.multiply(jcs.inverse())
        for p in drag_parts:
            p.Placement = turn.multiply(init[p])
        assembly.doDragStep()
    assembly.postDrag()

    turned = (boxD.Placement.Rotation.inverted() * start_d.Rotation).Angle * 57.29577951308232
    print(f"BoxD gedreht: {turned:.4f} Grad (erwartet {ANGLE})")
    if abs(turned - ANGLE) > TOL_ANGLE:
        print("FEHLER: Block hat sich nicht um die Drehachse gedreht")
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
