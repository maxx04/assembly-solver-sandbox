"""
build_fixture.py - baut die festen Ausgangsdateien fuer test_nested_rigid_to_flexible_switch.py
EINMALIG: Sub, Mid (Sub STARR mit Container-Versatz, an BoxD), Grand (Mid flexibel an BoxC,
gedreht - Mid steckt in Grand an anderer Stelle als in Mid selbst). Alle Gelenke erfuellt.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_nested_rigid_to_flexible_switch/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402
from test_nested_rigid_to_flexible_switch import (  # noqa: E402
    INNER_PLC1, INNER_PLC2, MID_PLC1, MID_PLC2, OUTER_PLC1, OUTER_PLC2, SUBLINK_OFFSET, fixture_paths,
)


def label_joint_group(asm):
    for child in asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"


def main():
    paths = fixture_paths()
    os.makedirs(os.path.dirname(paths["sub"]), exist_ok=True)

    sub_doc, sub_asm, subA, subB = ntu.new_sub_assembly_doc(
        "RigidSwitchSub", paths["sub"], box_a_name="SubA", box_b_name="SubB"
    )
    jtu.make_joint(sub_asm, 0, subA, subB, INNER_PLC1, INNER_PLC2)
    subB.Placement = subA.Placement.multiply(INNER_PLC1).multiply(INNER_PLC2.inverse())
    label_joint_group(sub_asm)
    sub_doc.recompute()

    mid_doc, mid_asm, boxD, sublink = ntu.new_grand_assembly_with_sublink(
        "RigidSwitchMid", sub_asm, paths["sub"], paths["mid"], box_name="BoxD", link_name="SubLink"
    )
    sublink.Rigid = True
    mid_doc.recompute()
    mirror_subB = ntu.get_mirror(sublink, "SubB")
    joint = jtu.make_joint(mid_asm, 0, boxD, mirror_subB, MID_PLC1, MID_PLC2)
    sub = f"{mirror_subB.Name}.Face1"
    joint.Reference2 = (sublink, [sub, sub])
    # starrer Koerper = Container: Lage so, dass der Joint erfuellt ist, mit grossem Versatz
    target_subB = boxD.Placement.multiply(MID_PLC1).multiply(MID_PLC2.inverse())
    sublink.Placement = target_subB.multiply(mirror_subB.Placement.inverse())
    label_joint_group(mid_asm)
    mid_doc.recompute()
    rc_mid = mid_asm.solve()
    print(f"Mid solve rc={rc_mid}, SubLink-Placement {sublink.Placement}")

    grand_doc, grand_asm, boxC, midlink = ntu.new_grand_assembly_with_sublink(
        "RigidSwitchGrand", mid_asm, paths["mid"], paths["grand"], box_name="BoxC", link_name="MidLink"
    )
    boxD_mirror = ntu.get_mirror(midlink, "BoxD")
    jtu.make_joint(grand_asm, 0, boxC, boxD_mirror, OUTER_PLC1, OUTER_PLC2, elem1="Face4", elem2="Face5").Label = "Joint"
    label_joint_group(grand_asm)
    grand_doc.recompute()
    rc = grand_asm.solve()
    grand_asm.solve()
    sub_mirror = [o for o in midlink.Group if o.TypeId == "Assembly::AssemblyLink"][0]
    print(f"Grand solve rc={rc}, SubLink-Spiegel Rigid={sub_mirror.Rigid} Placement {sub_mirror.Placement}")

    for doc in (sub_doc, mid_doc, grand_doc):
        doc.save()
    for p in paths.values():
        print(f"Fixture geschrieben: {p}")
    ok = rc == 0 and rc_mid == 0 and sub_mirror.Rigid
    print("RESULT: PASS" if ok else "RESULT: FAIL (Fixtur-Aufbau)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
