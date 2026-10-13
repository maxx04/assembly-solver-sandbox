"""
build_fixture.py - baut die festen Ausgangsdateien fuer
test_nested_loose_subassembly_without_ground.py EINMALIG: Mid (BoxD -[Fixed]- BoxA, keine
Erdung), Grand (BoxC geerdet, Mid flexibel, lose und versetzt abgelegt).

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_nested_loose_subassembly_without_ground/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402
from test_nested_loose_subassembly_without_ground import MID_DROP, MID_PLC1, MID_PLC2, fixture_paths  # noqa: E402


def main():
    import UtilsAssembly
    paths = fixture_paths()
    os.makedirs(os.path.dirname(paths["mid"]), exist_ok=True)
    mid_doc = App.newDocument("LooseNoGroundMid")
    mid_doc.saveAs(paths["mid"])
    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(paths["mid"]), ["BoxD", "BoxA"])
    boxD = jtu.link_external_box(mid_doc, "BoxD", box_paths["BoxD"])
    boxA = jtu.link_external_box(mid_doc, "BoxA", box_paths["BoxA"])
    mid_asm = mid_doc.addObject("Assembly::AssemblyObject", "Assembly")
    mid_asm.addObject(boxD)
    mid_asm.addObject(boxA)
    UtilsAssembly.getJointGroup(mid_asm)
    mid_doc.recompute()
    jtu.make_joint(mid_asm, 0, boxD, boxA, MID_PLC1, MID_PLC2, elem1="Face2", elem2="Face1")
    boxA.Placement = boxD.Placement.multiply(MID_PLC1).multiply(MID_PLC2.inverse())
    mid_doc.recompute()
    mid_doc.save()

    grand_doc, grand_asm, boxC, midlink = ntu.new_grand_assembly_with_sublink(
        "LooseNoGroundGrand", mid_asm, paths["mid"], paths["grand"], box_name="BoxC", link_name="MidLink"
    )
    midlink.Placement = MID_DROP
    grand_doc.recompute()
    rc = grand_asm.solve()
    print(f"Grand solve rc={rc}")
    grand_doc.save()
    mid_doc.save()
    print("RESULT: PASS" if rc == 0 else "RESULT: FAIL")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
