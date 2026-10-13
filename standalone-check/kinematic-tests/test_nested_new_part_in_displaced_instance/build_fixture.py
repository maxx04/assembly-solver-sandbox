"""
build_fixture.py - baut die festen Ausgangsdateien fuer
test_nested_new_part_in_displaced_instance.py EINMALIG: Mid (nur BoxD, geerdet) und Grand (BoxC
geerdet, Mid flexibel per Fixed mit 90 Grad angebunden - Mid steckt in Grand versetzt).

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_nested_new_part_in_displaced_instance/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402
from test_nested_new_part_in_displaced_instance import OUTER_PLC1, OUTER_PLC2, fixture_paths  # noqa: E402


def label_joint_group(asm):
    for child in asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"


def main():
    paths = fixture_paths()
    os.makedirs(os.path.dirname(paths["mid"]), exist_ok=True)

    mid_doc = App.newDocument("NewPartMid")
    mid_doc.saveAs(paths["mid"])
    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(paths["mid"]), ["BoxD"])
    boxD = jtu.link_external_box(mid_doc, "BoxD", box_paths["BoxD"])
    mid_asm = mid_doc.addObject("Assembly::AssemblyObject", "Assembly")
    mid_asm.addObject(boxD)
    mid_doc.recompute()
    import UtilsAssembly
    UtilsAssembly.getJointGroup(mid_asm)  # eine einzige JointGroup, die auch der Test findet
    jtu.ground_object(boxD)
    mid_doc.recompute()

    grand_doc, grand_asm, boxC, midlink = ntu.new_grand_assembly_with_sublink(
        "NewPartGrand", mid_asm, paths["mid"], paths["grand"], box_name="BoxC", link_name="MidLink"
    )
    boxD_mirror = ntu.get_mirror(midlink, "BoxD")
    jtu.make_joint(grand_asm, 0, boxC, boxD_mirror, OUTER_PLC1, OUTER_PLC2, elem1="Face4", elem2="Face5")
    label_joint_group(grand_asm)
    grand_doc.recompute()
    rc = grand_asm.solve()
    grand_asm.solve()
    print(f"Grand solve rc={rc}, BoxD-Spiegel {boxD_mirror.Placement}")
    grand_doc.save()
    mid_doc.save()
    print("RESULT: PASS" if rc == 0 else "RESULT: FAIL")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
