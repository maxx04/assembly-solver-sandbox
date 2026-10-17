"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_redundancy_tolerance_flat.py EINMALIG:
BoxA (geerdet) -[Fixed]- BoxC; BoxB -[Gleiter1]- BoxA und -[Gleiter2]- BoxC, beide Gleiter
parallel und stimmig (zwei Fuehrungen).

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_redundancy_tolerance_flat/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
from test_redundancy_tolerance_flat import (  # noqa: E402
    FIX_PLC1, FIX_PLC2, SIDE_OFFSET, SLD1_PLC1, SLD1_PLC2, TEST_NAME)


def main():
    fixture_file = jtu.fixture_path(THIS_DIR)
    doc, assembly, boxA, boxB = jtu.new_flat_two_box_assembly(TEST_NAME, fixture_file)
    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(fixture_file), ["BoxC"])
    boxC = jtu.link_external_box(doc, "BoxC", box_paths["BoxC"])
    assembly.addObject(boxC)
    doc.recompute()

    fix = jtu.make_joint(assembly, 0, boxA, boxC, FIX_PLC1, FIX_PLC2, elem1="Face2", elem2="Face1")
    fix.Label = "Starr"
    boxC.Placement = boxA.Placement.multiply(FIX_PLC1).multiply(FIX_PLC2.inverse())

    w1 = boxA.Placement.multiply(SLD1_PLC1)
    boxB.Placement = w1.multiply(SLD1_PLC2.inverse())
    s1 = jtu.make_joint(assembly, 3, boxA, boxB, SLD1_PLC1, SLD1_PLC2)
    s1.Label = "Gleiter1"

    w2 = w1.multiply(App.Placement(SIDE_OFFSET, App.Rotation()))
    s2 = jtu.make_joint(
        assembly, 3, boxC, boxB, boxC.Placement.inverse().multiply(w2), boxB.Placement.inverse().multiply(w2)
    )
    s2.Label = "Gleiter2"
    doc.recompute()
    rc = assembly.solve()
    print(f"solve rc={rc}")
    doc.save()
    print("Fixture geschrieben:", fixture_file)
    print("RESULT: PASS" if rc == 0 else "RESULT: FAIL")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
