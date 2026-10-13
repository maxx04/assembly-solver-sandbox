"""
build_fixture.py - baut die festen Ausgangsdateien fuer test_fixed_cross_parent_instance_nested.py
EINMALIG (Fixtur-Methodik siehe test_fixed_double_nested_flex/build_fixture.py): Sub, MidA, MidB,
Grand - Sub steckt in MidA UND MidB, beide sind in Grand eingebaut.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_fixed_cross_parent_instance_nested/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402
from test_fixed_cross_parent_instance_nested import (  # noqa: E402
    INNER_PLC1, INNER_PLC2, MIDA_PLC1, MIDA_PLC2, MIDB_PLC1, MIDB_PLC2,
    OUTER_A_PLC1, OUTER_A_PLC2, OUTER_B_PLC1, OUTER_B_PLC2, fixture_paths,
)


def label_joint_group(asm):
    for child in asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"


def build_mid(name, sub_asm, sub_path, mid_path, box_label, plc1, plc2, subB_label):
    mid_doc, mid_asm, box, sublink = ntu.new_grand_assembly_with_sublink(
        name, sub_asm, sub_path, mid_path, box_name=box_label, link_name="SubLink"
    )
    mirror_subB = ntu.get_mirror(sublink, subB_label)
    assert mirror_subB is not None, f"Spiegel von SubB in {name} nicht gefunden"
    jtu.make_joint(mid_asm, 0, box, mirror_subB, plc1, plc2).Label = "Joint"
    label_joint_group(mid_asm)
    mid_doc.recompute()
    return mid_doc, mid_asm, box


def main():
    paths = fixture_paths()
    os.makedirs(os.path.dirname(paths["sub"]), exist_ok=True)

    sub_doc, sub_asm, subA, subB = ntu.new_sub_assembly_doc(
        "CrossParentSub", paths["sub"], box_a_name="SubA", box_b_name="SubB"
    )
    jtu.make_joint(sub_asm, 0, subA, subB, INNER_PLC1, INNER_PLC2)
    subB.Placement = subA.Placement.multiply(INNER_PLC1).multiply(INNER_PLC2.inverse())
    sub_doc.recompute()

    mida_doc, mida_asm, boxD = build_mid(
        "CrossParentMidA", sub_asm, paths["sub"], paths["mida"], "BoxD", MIDA_PLC1, MIDA_PLC2, subB.Label
    )
    midb_doc, midb_asm, boxE = build_mid(
        "CrossParentMidB", sub_asm, paths["sub"], paths["midb"], "BoxD", MIDB_PLC1, MIDB_PLC2, subB.Label
    )
    mida_doc.save()
    midb_doc.save()

    grand_doc, grand_asm, boxC, mida_link = ntu.new_grand_assembly_with_sublink(
        "CrossParentGrand", mida_asm, paths["mida"], paths["grand"], box_name="BoxC", link_name="MidALink"
    )
    midb_link = grand_asm.newObject("Assembly::AssemblyLink", midb_asm.Label)
    midb_link.LinkedObject = midb_asm
    midb_link.Rigid = False
    midb_link.Label = "MidBLink"
    grand_asm.addObject(midb_link)
    grand_doc.recompute()

    boxD_mirror = ntu.get_mirror(mida_link, boxD.Label)
    boxE_mirror = ntu.get_mirror(midb_link, boxE.Label)
    assert boxD_mirror is not None and boxE_mirror is not None, "Box-Spiegel in Grand nicht gefunden"
    jtu.make_joint(grand_asm, 0, boxC, boxD_mirror, OUTER_A_PLC1, OUTER_A_PLC2).Label = "Joint"
    jtu.make_joint(grand_asm, 0, boxC, boxE_mirror, OUTER_B_PLC1, OUTER_B_PLC2)
    label_joint_group(grand_asm)
    grand_doc.recompute()

    grand_doc.save()
    mida_doc.save()
    midb_doc.save()
    sub_doc.save()
    for p in paths.values():
        print(f"Fixture geschrieben: {p}")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
