"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_drag_bundled_part_flat.py EINMALIG:
BoxA (geerdet) -[Slider]- BoxB -[Fixed]- BoxC, alle Joints erfuellt.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_drag_bundled_part_flat/build_fixture.py
"""
import os
import sys

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
from test_drag_bundled_part_flat import (  # noqa: E402
    FIXED_PLC1, FIXED_PLC2, SLIDER_PLC1, SLIDER_PLC2, TEST_NAME,
)


def main():
    fixture_file = jtu.fixture_path(THIS_DIR)
    doc, assembly, boxA, boxB = jtu.new_flat_two_box_assembly(TEST_NAME, fixture_file)
    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(fixture_file), ["BoxC"])
    boxC = jtu.link_external_box(doc, "BoxC", box_paths["BoxC"])
    assembly.addObject(boxC)
    doc.recompute()
    jtu.make_joint(assembly, 3, boxA, boxB, SLIDER_PLC1, SLIDER_PLC2).Label = "Slider"
    jtu.make_joint(assembly, 0, boxB, boxC, FIXED_PLC1, FIXED_PLC2, elem1="Face3", elem2="Face4").Label = "Fixed"
    boxB.Placement = boxA.Placement.multiply(SLIDER_PLC1).multiply(SLIDER_PLC2.inverse())
    boxC.Placement = boxB.Placement.multiply(FIXED_PLC1).multiply(FIXED_PLC2.inverse())
    doc.recompute()
    doc.save()
    print("Fixture geschrieben:", fixture_file)
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
