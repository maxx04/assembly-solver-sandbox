"""
build_fixture.py - baut die festen Ausgangsdateien fuer test_nested_insert_start_state.py
EINMALIG (Fixtur-Methodik siehe test_fixed_double_nested_flex/build_fixture.py): SubF, SubR, Mid
(SubF flexibel, SubR starr, nach dem Solve gespeichert) und Grand (nur BoxC, Mid wird erst im
Test eingefuegt).

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_nested_insert_start_state/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402
from test_nested_insert_start_state import (  # noqa: E402
    INNER_F_PLC1, INNER_F_PLC2, INNER_R_PLC1, INNER_R_PLC2, MID_F_PLC1, MID_F_PLC2,
    MID_R_PLC1, MID_R_PLC2, fixture_paths,
)


def label_joint_group(asm):
    for child in asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"


def build_sub(name, path, label_a, label_b, plc1, plc2):
    doc, asm, boxA, boxB = ntu.new_sub_assembly_doc(name, path, box_a_name=label_a, box_b_name=label_b)
    jtu.make_joint(asm, 0, boxA, boxB, plc1, plc2)
    boxB.Placement = boxA.Placement.multiply(plc1).multiply(plc2.inverse())
    label_joint_group(asm)
    doc.recompute()
    return doc, asm


def joint_on_link(asm, boxD, link, mirror, plc1, plc2):
    """Fixed-Joint BoxD <-> Teil in link, Referenz wie aus der GUI: Wurzel ist der Link der
    Unterbaugruppe, das Teil steht im Unterpfad (z.B. SubRLink + "RB.Face1")."""
    joint = jtu.make_joint(asm, 0, boxD, mirror, plc1, plc2)
    sub = f"{mirror.Name}.Face1"
    joint.Reference2 = (link, [sub, sub])
    return joint


def main():
    paths = fixture_paths()
    os.makedirs(os.path.dirname(paths["subf"]), exist_ok=True)

    subf_doc, subf_asm = build_sub("InsertStartSubF", paths["subf"], "FA", "FB", INNER_F_PLC1, INNER_F_PLC2)
    subr_doc, subr_asm = build_sub("InsertStartSubR", paths["subr"], "RA", "RB", INNER_R_PLC1, INNER_R_PLC2)

    mid_doc, mid_asm, boxD, subf_link = ntu.new_grand_assembly_with_sublink(
        "InsertStartMid", subf_asm, paths["subf"], paths["mid"], box_name="BoxD", link_name="SubFLink"
    )
    subr_doc.saveAs(paths["subr"])
    subr_link = mid_asm.newObject("Assembly::AssemblyLink", subr_asm.Label)
    subr_link.LinkedObject = subr_asm
    subr_link.Rigid = True
    subr_link.Label = "SubRLink"
    mid_asm.addObject(subr_link)
    mid_doc.recompute()

    fb = ntu.get_mirror(subf_link, "FB")
    rb = ntu.get_mirror(subr_link, "RB")
    assert fb is not None and rb is not None, "Spiegel FB/RB in Mid nicht gefunden"
    joint_on_link(mid_asm, boxD, subf_link, fb, MID_F_PLC1, MID_F_PLC2).Label = "JointF"
    joint_on_link(mid_asm, boxD, subr_link, rb, MID_R_PLC1, MID_R_PLC2).Label = "JointR"
    label_joint_group(mid_asm)
    mid_doc.recompute()
    rc = mid_asm.solve()
    mid_asm.solve()
    print(f"Mid solve rc={rc}")

    # Die Spiegel in Mid muessen nach dem Solve von den Originalen abweichen, sonst prueft der
    # Test die Startlage nicht.
    fa_orig = [o for o in subf_doc.Objects if o.Label == "FB"][0]
    shift = (fb.Placement.Base - fa_orig.Placement.Base).Length
    print(f"FB-Spiegel in Mid weicht {shift:.3f} mm vom Original ab")
    assert shift > 1.0, "Spiegel liegt noch auf dem Original - Fixtur ungeeignet"

    grand_doc = App.newDocument("InsertStartGrand")
    grand_doc.saveAs(paths["grand"])
    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(paths["grand"]), ["BoxC"])
    boxC = jtu.link_external_box(grand_doc, "BoxC", box_paths["BoxC"])
    grand_doc.recompute()
    grand_asm = grand_doc.addObject("Assembly::AssemblyObject", "Assembly")
    grand_asm.addObject(boxC)
    grand_doc.recompute()
    jtu.ground_object(boxC)
    label_joint_group(grand_asm)
    grand_doc.recompute()

    for doc in (subf_doc, subr_doc, mid_doc, grand_doc):
        doc.save()
    for p in paths.values():
        print(f"Fixture geschrieben: {p}")
    print("RESULT: PASS" if rc == 0 else "RESULT: FAIL (Mid-Solve)")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
