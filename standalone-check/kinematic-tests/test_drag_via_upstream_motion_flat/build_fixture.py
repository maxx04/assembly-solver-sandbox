"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_drag_via_upstream_motion_flat.py
EINMALIG: BoxA (geerdet) -[Revolute]- BoxB -[Fixed]- BoxC -[Slider]- BoxD, dazu ein Distance
(Punkt in Ebene, Ebene senkrecht zur Drehachse) BoxC - BoxA, als ERSTES Gelenk angelegt. Alle
Gelenke erfuellt.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_drag_via_upstream_motion_flat/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
from test_drag_via_upstream_motion_flat import (  # noqa: E402
    FIX_PLC1, FIX_PLC2, REV_PLC1, REV_PLC2, SLD_PLC1, SLD_PLC2, TEST_NAME,
)


def main():
    fixture_file = jtu.fixture_path(THIS_DIR)
    doc, assembly, boxA, boxB = jtu.new_flat_two_box_assembly(TEST_NAME, fixture_file)
    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(fixture_file), ["BoxC", "BoxD"])
    boxC = jtu.link_external_box(doc, "BoxC", box_paths["BoxC"])
    boxD = jtu.link_external_box(doc, "BoxD", box_paths["BoxD"])
    assembly.addObject(boxC)
    assembly.addObject(boxD)
    doc.recompute()

    boxB.Placement = boxA.Placement.multiply(REV_PLC1).multiply(REV_PLC2.inverse())
    boxC.Placement = boxB.Placement.multiply(FIX_PLC1).multiply(FIX_PLC2.inverse())
    boxD.Placement = boxC.Placement.multiply(SLD_PLC1).multiply(SLD_PLC2.inverse())

    # Distance Punkt-in-Ebene: Ebene an BoxC mit Normale = Drehachse (z der Revolute-JCS),
    # Punkt an BoxA genau in dieser Ebene (dort, wo die Drehachse sie durchstoesst - bleibt
    # bei jeder Drehung um die Achse erfuellt).
    axis_world = boxB.Placement.multiply(REV_PLC2)
    plane_world = App.Placement(
        axis_world.Base + axis_world.Rotation.multVec(App.Vector(0, 0, 7)), axis_world.Rotation
    )
    dist_plc1 = boxC.Placement.inverse().multiply(plane_world)  # Ebene, BoxC
    dist_plc2 = boxA.Placement.inverse().multiply(App.Placement(plane_world.Base, App.Rotation()))  # Punkt, BoxA
    jtu.make_joint(assembly, 5, boxC, boxA, dist_plc1, dist_plc2, elem1="Face6", elem2="Vertex1").Label = "Abstand"
    jtu.make_joint(assembly, 1, boxA, boxB, REV_PLC1, REV_PLC2).Label = "Dreh"
    jtu.make_joint(assembly, 0, boxB, boxC, FIX_PLC1, FIX_PLC2, elem1="Face3", elem2="Face4").Label = "Starr"
    jtu.make_joint(assembly, 3, boxC, boxD, SLD_PLC1, SLD_PLC2, elem1="Face2", elem2="Face5").Label = "Gleit"
    doc.recompute()
    doc.save()
    print("Fixture geschrieben:", fixture_file)
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
