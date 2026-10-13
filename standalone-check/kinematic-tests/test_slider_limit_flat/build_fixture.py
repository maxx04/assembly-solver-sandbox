"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_slider_limit_flat.py EINMALIG:
BoxA (geerdet) -[Slider mit Grenze]- BoxB, BoxB exakt bei Gelenkwert 0.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_slider_limit_flat/build_fixture.py
"""
import os
import sys

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
from test_slider_limit_flat import JOINT_TYPE_INDEX, LIMIT_MAX, LIMIT_MIN, PLC1, PLC2, TEST_NAME  # noqa: E402


def main():
    fixture_file = jtu.fixture_path(THIS_DIR)
    doc, assembly, boxA, boxB = jtu.new_flat_two_box_assembly(TEST_NAME, fixture_file)
    joint = jtu.make_joint(assembly, JOINT_TYPE_INDEX, boxA, boxB, PLC1, PLC2)
    joint.EnableLengthMin = True
    joint.LengthMin = LIMIT_MIN
    joint.EnableLengthMax = True
    joint.LengthMax = LIMIT_MAX
    boxB.Placement = boxA.Placement.multiply(PLC1).multiply(PLC2.inverse())
    doc.recompute()
    doc.save()
    print("Fixture geschrieben:", fixture_file)
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
