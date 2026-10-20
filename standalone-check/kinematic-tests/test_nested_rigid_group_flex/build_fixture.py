"""
build_fixture.py - baut die festen Ausgangsdateien fuer test_nested_rigid_group_flex.py
EINMALIG: Mid (BoxA geerdet, BoxB, BoxD in einer starren Gruppe), Grand (BoxC geerdet,
BoxS -[Slider]- BoxC, MidLink flexibel und versetzt, BoxS -[Fixed]- BoxB-Instanz).

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_nested_rigid_group_flex/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402
from test_nested_rigid_group_flex import (  # noqa: E402
    B_IN_MID, D_IN_MID, FIXED_PLC1, FIXED_PLC2, MID_DROP, SLIDER_PLC1, SLIDER_PLC2, TEST_NAME,
    fixture_paths,
)


def main():
    import JointObject
    import UtilsAssembly
    paths = fixture_paths()
    os.makedirs(os.path.dirname(paths["mid"]), exist_ok=True)

    mid_doc = App.newDocument(f"{TEST_NAME}_mid")
    mid_doc.saveAs(paths["mid"])
    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(paths["mid"]), ["BoxA", "BoxB", "BoxD", "BoxC"])
    boxA = jtu.link_external_box(mid_doc, "BoxA", box_paths["BoxA"])
    boxB = jtu.link_external_box(mid_doc, "BoxB", box_paths["BoxB"])
    boxD = jtu.link_external_box(mid_doc, "BoxD", box_paths["BoxD"])
    mid_asm = mid_doc.addObject("Assembly::AssemblyObject", "Assembly")
    for box in (boxA, boxB, boxD):
        mid_asm.addObject(box)
    boxB.Placement = B_IN_MID
    boxD.Placement = D_IN_MID
    joint_group = UtilsAssembly.getJointGroup(mid_asm)
    mid_doc.recompute()
    jtu.ground_object(boxA)
    rg = joint_group.newObject("App::FeaturePython", "RigidGroupJoint")
    JointObject.RigidGroupJoint(rg, [boxA, boxB, boxD])
    if App.GuiUp:
        JointObject.ViewProviderRigidGroupJoint(rg.ViewObject)
    mid_doc.recompute()
    mid_doc.save()

    grand_doc, grand_asm, boxC, midlink = ntu.new_grand_assembly_with_sublink(
        f"{TEST_NAME}_grand", mid_asm, paths["mid"], paths["grand"], box_name="BoxC", link_name="MidLink"
    )
    midlink.Placement = MID_DROP
    boxS = jtu.link_external_box(grand_doc, "BoxD", box_paths["BoxD"])  # Koerper von BoxD
    boxS.Label = "BoxS"
    grand_asm.addObject(boxS)
    boxS.Placement = boxC.Placement.multiply(SLIDER_PLC1).multiply(SLIDER_PLC2.inverse())
    grand_doc.recompute()
    jtu.make_joint(grand_asm, 3, boxC, boxS, SLIDER_PLC1, SLIDER_PLC2, elem1="Face6", elem2="Face5")
    mirror_b = ntu.get_mirror(midlink, "BoxB")
    jtu.make_joint(grand_asm, 0, boxS, mirror_b, FIXED_PLC1, FIXED_PLC2, elem1="Face2", elem2="Face1")
    grand_doc.recompute()
    rc = grand_asm.solve()
    print(f"Grand solve rc={rc}")
    grand_doc.save()
    mid_doc.save()
    print("RESULT: PASS" if rc == 0 else "RESULT: FAIL")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
