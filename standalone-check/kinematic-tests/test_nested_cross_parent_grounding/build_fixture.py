"""
build_fixture.py - baut die festen Ausgangsdateien fuer test_nested_cross_parent_grounding.py
EINMALIG: Sub, MidA und MidB (beide mit Sub flexibel und geerdeter SubA-Instanz), Grand mit MidA
angebunden und MidB lose, versetzt abgelegt.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_nested_cross_parent_grounding/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402
from test_nested_cross_parent_grounding import (  # noqa: E402
    INNER_PLC1, INNER_PLC2, MID_PLC1, MID_PLC2, MIDB_DROP, OUTER_PLC1, OUTER_PLC2, fixture_paths,
)


def build_mid(name, sub_asm, sub_path, path):
    import UtilsAssembly
    doc = App.newDocument(name)
    doc.saveAs(path)
    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(path), ["BoxD"])
    boxD = jtu.link_external_box(doc, "BoxD", box_paths["BoxD"])
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    asm.addObject(boxD)
    UtilsAssembly.getJointGroup(asm)
    link = asm.newObject("Assembly::AssemblyLink", sub_asm.Label)
    link.LinkedObject = sub_asm
    link.Rigid = False
    link.Label = "SubLink"
    doc.recompute()
    subA = ntu.get_mirror(link, "SubA")
    subB = ntu.get_mirror(link, "SubB")
    jtu.ground_object(subA)
    jtu.make_joint(asm, 0, subB, boxD, MID_PLC1, MID_PLC2, elem1="Face2", elem2="Face3")
    boxD.Placement = subB.Placement.multiply(MID_PLC1).multiply(MID_PLC2.inverse())
    doc.recompute()
    asm.solve()
    return doc, asm


def main():
    paths = fixture_paths()
    os.makedirs(os.path.dirname(paths["sub"]), exist_ok=True)
    sub_doc, sub_asm, subA, subB = ntu.new_sub_assembly_doc(
        "CrossGroundSub", paths["sub"], box_a_name="SubA", box_b_name="SubB"
    )
    # Sub hat KEINE eigene Erdung - ntu.new_sub_assembly_doc erdet SubA; hier wieder entfernen,
    # damit die Erdung ausschliesslich aus MidA/MidB kommt (wie BG73/BG75 den Stator erden).
    for obj in list(sub_doc.Objects):
        if hasattr(obj, "ObjectToGround"):
            sub_doc.removeObject(obj.Name)
    subA.setPropertyStatus("Placement", "-ReadOnly")
    jtu.make_joint(sub_asm, 0, subA, subB, INNER_PLC1, INNER_PLC2)
    subB.Placement = subA.Placement.multiply(INNER_PLC1).multiply(INNER_PLC2.inverse())
    sub_doc.recompute()
    sub_doc.save()

    mida_doc, mida_asm = build_mid("CrossGroundMidA", sub_asm, paths["sub"], paths["mida"])
    midb_doc, midb_asm = build_mid("CrossGroundMidB", sub_asm, paths["sub"], paths["midb"])
    mida_doc.save()
    midb_doc.save()

    grand_doc, grand_asm, boxC, mida_link = ntu.new_grand_assembly_with_sublink(
        "CrossGroundGrand", mida_asm, paths["mida"], paths["grand"], box_name="BoxC", link_name="MidALink"
    )
    midb_link = grand_asm.newObject("Assembly::AssemblyLink", midb_asm.Label)
    midb_link.LinkedObject = midb_asm
    midb_link.Rigid = False
    midb_link.Label = "MidBLink"
    midb_link.Placement = MIDB_DROP
    grand_doc.recompute()
    boxD_a = ntu.get_mirror(mida_link, "BoxD")
    jtu.make_joint(grand_asm, 0, boxC, boxD_a, OUTER_PLC1, OUTER_PLC2, elem1="Face4", elem2="Face5")
    grand_doc.recompute()
    rc = grand_asm.solve()
    print(f"Grand solve rc={rc}")
    for doc in (sub_doc, mida_doc, midb_doc, grand_doc):
        doc.save()
    print("RESULT: PASS" if rc == 0 else "RESULT: FAIL")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
