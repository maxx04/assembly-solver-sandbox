"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_redundancy_group_flat.py EINMALIG:
BoxA (geerdet) -[Revolute "Dreh"]- BoxB, BoxA -[Slider "Gleit"]- BoxC -[Fixed "Starr"]- BoxD,
zuletzt Parallel "Parallel" BoxB-BoxA (Achsen senkrecht zur Drehachse). Alle Gelenke erfuellt.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_redundancy_group_flat/build_fixture.py
"""
import os
import sys

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
from test_redundancy_group_flat import (  # noqa: E402
    FIX_PLC1, FIX_PLC2, PAR_PLC1, PAR_PLC2, REV_PLC1, REV_PLC2, SLD_PLC1, SLD_PLC2, TEST_NAME,
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
    boxC.Placement = boxA.Placement.multiply(SLD_PLC1).multiply(SLD_PLC2.inverse())
    boxD.Placement = boxC.Placement.multiply(FIX_PLC1).multiply(FIX_PLC2.inverse())

    jtu.make_joint(assembly, 1, boxA, boxB, REV_PLC1, REV_PLC2).Label = "Dreh"
    jtu.make_joint(assembly, 3, boxA, boxC, SLD_PLC1, SLD_PLC2, elem1="Face4", elem2="Face3").Label = "Gleit"
    jtu.make_joint(assembly, 0, boxC, boxD, FIX_PLC1, FIX_PLC2, elem1="Face2", elem2="Face5").Label = "Starr"
    jtu.make_joint(assembly, 6, boxB, boxA, PAR_PLC1, PAR_PLC2).Label = "Parallel"
    doc.recompute()
    doc.save()
    print("Fixture geschrieben:", fixture_file)
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
