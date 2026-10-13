"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_drag_joint_fewest_dof_flat.py
EINMALIG: BoxA (geerdet) -[Slider]- BoxB -[Fixed]- BoxC, BoxD -[Fixed]- BoxA, als ERSTES Gelenk
Zylinderfuehrung BoxC - BoxD (Achse parallel zum Slider, seitlich versetzt). Alle Gelenke erfuellt.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_drag_joint_fewest_dof_flat/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
from test_drag_joint_fewest_dof_flat import (  # noqa: E402
    CYL_OFFSET, FIX_AD1, FIX_AD2, FIX_BC1, FIX_BC2, SLD_PLC1, SLD_PLC2, TEST_NAME,
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

    boxB.Placement = boxA.Placement.multiply(SLD_PLC1).multiply(SLD_PLC2.inverse())
    boxC.Placement = boxB.Placement.multiply(FIX_BC1).multiply(FIX_BC2.inverse())
    boxD.Placement = boxA.Placement.multiply(FIX_AD1).multiply(FIX_AD2.inverse())

    slider_world = boxA.Placement.multiply(SLD_PLC1)
    cyl_world = slider_world.multiply(App.Placement(CYL_OFFSET, App.Rotation()))
    cyl_c = boxC.Placement.inverse().multiply(cyl_world)
    cyl_d = boxD.Placement.inverse().multiply(cyl_world)
    jtu.make_joint(assembly, 2, boxC, boxD, cyl_c, cyl_d, elem1="Face6", elem2="Face6").Label = "Zylinder"
    jtu.make_joint(assembly, 3, boxA, boxB, SLD_PLC1, SLD_PLC2).Label = "Gleit"
    jtu.make_joint(assembly, 0, boxB, boxC, FIX_BC1, FIX_BC2, elem1="Face3", elem2="Face4").Label = "StarrBC"
    jtu.make_joint(assembly, 0, boxA, boxD, FIX_AD1, FIX_AD2, elem1="Face2", elem2="Face5").Label = "StarrAD"
    doc.recompute()
    doc.save()
    print("Fixture geschrieben:", fixture_file)
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
