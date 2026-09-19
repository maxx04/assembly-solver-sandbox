"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_fixed_duplicate_instance_nested.py
EINMALIG (siehe test_fixed_double_nested_flex/build_fixture.py fuer die volle Begruendung der
3-Dokumente-Fixture-Methodik, und test_fixed_duplicate_instance_flat/build_fixture.py fuer die
manuelle Zweitinstanz-Einfuegung - hier beides kombiniert: die MITTLERE Ebene ("Mid") wird
zweimal in GrandTop eingefuegt).

Aufruf:
    ./run-test.sh /home/maxx/freecad-sandbox/install test_fixed_duplicate_instance_nested/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu
from test_fixed_duplicate_instance_nested import (
    INNER_PLC1, INNER_PLC2, MIDDLE_PLC1, MIDDLE_PLC2, OUTER_PLC1, OUTER_PLC2, _fixture_paths,
)


def main():
    sub_path, mid_path, grand_path = _fixture_paths()
    for p in (sub_path, mid_path, grand_path):
        os.makedirs(os.path.dirname(p), exist_ok=True)

    # --- Sub: SubA (geerdet) -[innerster Fixed-Joint]- SubB ---
    sub_doc, sub_asm, subA, subB = ntu.new_sub_assembly_doc(
        "DupInstNestedSub", sub_path, box_a_name="SubA", box_b_name="SubB"
    )
    jtu.make_joint(sub_asm, 0, subA, subB, INNER_PLC1, INNER_PLC2)
    subB.Placement = subA.Placement.multiply(INNER_PLC1).multiply(INNER_PLC2.inverse())
    sub_doc.recompute()

    # --- Mid: BoxD (geerdet) + SubLink(Sub) -[mittlerer Fixed-Joint]- SubLink.SubB (Spiegel) ---
    mid_doc, mid_asm, boxD, sublink = ntu.new_grand_assembly_with_sublink(
        "DupInstNestedMid", sub_asm, sub_path, mid_path, box_name="BoxD", link_name="SubLink"
    )
    mirror_subB_mid = ntu.get_mirror(sublink, subB.Label)
    assert mirror_subB_mid is not None, "Spiegel von SubB nicht in SubLink.Group gefunden"
    middle_joint = jtu.make_joint(mid_asm, 0, boxD, mirror_subB_mid, MIDDLE_PLC1, MIDDLE_PLC2)
    middle_joint.Label = "Joint"
    for child in mid_asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"
    mid_doc.recompute()

    # --- GrandTop: BoxC (geerdet) + MidLink1(Mid) -[aeusserer Joint 1]- MidLink1.SubLink.SubB ---
    grand_doc, grand_asm, boxC, midlink1 = ntu.new_grand_assembly_with_sublink(
        "DupInstNestedGrand", mid_asm, mid_path, grand_path, box_name="BoxC", link_name="MidLink1"
    )
    boxD_mirror1 = ntu.get_mirror(midlink1, boxD.Label)
    sublink_mirror1 = ntu.get_mirror(midlink1, sublink.Label)
    assert boxD_mirror1 is not None, "Spiegel von BoxD (Instanz 1) nicht in MidLink1.Group gefunden"
    assert sublink_mirror1 is not None, "Spiegel von SubLink (Instanz 1) nicht in MidLink1.Group gefunden"
    mirror_subB_grand1 = ntu.get_mirror(sublink_mirror1, subB.Label)
    assert mirror_subB_grand1 is not None, "Doppelt gespiegeltes SubB (Instanz 1) nicht gefunden"

    outer_joint1 = jtu.make_joint(grand_asm, 0, boxC, mirror_subB_grand1, OUTER_PLC1, OUTER_PLC2)
    outer_joint1.Label = "Joint"
    for child in grand_asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"
    grand_doc.recompute()

    # --- Zweite Instanz von Mid einfuegen - EXAKT wie CommandInsertLink.py::onItemClicked()
    # das fuer den Nutzer tut (siehe test_fixed_duplicate_instance_flat/build_fixture.py fuer
    # dieselbe Begruendung). Der Wunschname "Assembly" (mid_asm.Label) kollidiert bewusst mit
    # der ersten Instanz - genau das loest FreeCADs automatische Umbenennung aus.
    midlink2 = grand_asm.newObject("Assembly::AssemblyLink", mid_asm.Label)
    midlink2.LinkedObject = mid_asm
    midlink2.Rigid = False
    midlink2.Label = "MidLink2"
    grand_asm.addObject(midlink2)
    grand_doc.recompute()

    boxD_mirror2 = ntu.get_mirror(midlink2, boxD.Label)
    sublink_mirror2 = ntu.get_mirror(midlink2, sublink.Label)
    assert boxD_mirror2 is not None, "Spiegel von BoxD (Instanz 2) nicht in MidLink2.Group gefunden"
    assert sublink_mirror2 is not None, "Spiegel von SubLink (Instanz 2) nicht in MidLink2.Group gefunden"
    mirror_subB_grand2 = ntu.get_mirror(sublink_mirror2, subB.Label)
    assert mirror_subB_grand2 is not None, "Doppelt gespiegeltes SubB (Instanz 2) nicht gefunden"

    # ZWEITER aeusserer Joint, mit DENSELBEN Placement-Werten wie der erste (physikalisch
    # konsistentes Duplikat, siehe test_fixed_duplicate_instance_flat.py fuer die Begruendung).
    outer_joint2 = jtu.make_joint(grand_asm, 0, boxC, mirror_subB_grand2, OUTER_PLC1, OUTER_PLC2)
    grand_doc.recompute()

    # Absichtlich falsche Position NACH dem Aufbau setzen (siehe test_fixed_double_nested_flex/
    # build_fixture.py fuer die volle Begruendung des Seeding-Hebels) - ueber das ECHTE
    # Sub#SubB, das wirkt sich auf BEIDE Instanzen gleichermassen aus (es gibt nur ein
    # einziges reales Sub#SubB).
    target_plc = boxC.Placement.multiply(OUTER_PLC1).multiply(OUTER_PLC2.inverse())
    start_rotation = target_plc.Rotation.multiply(App.Rotation(App.Vector(1, 1, 1), 30))
    subB.Placement = App.Placement(App.Vector(999, -999, 999), start_rotation)
    sub_doc.recompute()

    grand_doc.save()
    mid_doc.save()
    sub_doc.save()

    print(f"Fixture geschrieben: {sub_path}")
    print(f"Fixture geschrieben: {mid_path}")
    print(f"Fixture geschrieben: {grand_path}")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
