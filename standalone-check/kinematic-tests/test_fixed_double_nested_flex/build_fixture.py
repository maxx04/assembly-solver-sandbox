"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_fixed_double_nested_flex.py EINMALIG
und speichert sie git-getrackt unter fixtures/ (drei Dokumente: sub/mid/grand).

Siehe test_fixed_nested_flex/build_fixture.py fuer die volle Begruendung der Fixture-Methodik
(byte-identischer Ausgangszustand fuer patched UND vanilla) - hier nur das fuer die DRITTE
Ebene Neue dokumentiert (siehe auch test_fixed_double_nested_flex.py's Moduldocstring).

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_fixed_double_nested_flex/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu
from test_fixed_double_nested_flex import (
    INNER_PLC1, INNER_PLC2, MIDDLE_PLC1, MIDDLE_PLC2, OUTER_PLC1, OUTER_PLC2, _fixture_paths,
)


def main():
    sub_path, mid_path, grand_path = _fixture_paths()
    for p in (sub_path, mid_path, grand_path):
        os.makedirs(os.path.dirname(p), exist_ok=True)

    # --- Sub: BoxA (geerdet) -[innerster Fixed-Joint]- BoxB ---
    sub_doc, sub_asm, subA, subB = ntu.new_sub_assembly_doc("FixedDoubleNestedFlexSub")
    jtu.make_joint(sub_asm, 0, subA, subB, INNER_PLC1, INNER_PLC2)
    # subB analytisch korrekt relativ zu subA vorbelegen (reiner interner Sub-Aufbau, kein
    # Treiber-Testfall - siehe test_fixed_nested_flex/build_fixture.py fuer die Begruendung,
    # warum das HIER unproblematisch ist, im Gegensatz zur bewusst falschen Ueberschreibung
    # weiter unten).
    subB.Placement = subA.Placement.multiply(INNER_PLC1).multiply(INNER_PLC2.inverse())
    sub_doc.recompute()

    # --- Mid: BoxD (geerdet) + SubLink(Sub) -[mittlerer Fixed-Joint]- SubLink.BoxB (Spiegel) ---
    # box_name="BoxD" (NICHT der Default "BoxC") - vermeidet die Namenskollision beim
    # spaeteren Einbetten von Mid in GrandTop, siehe Moduldocstring von
    # test_fixed_double_nested_flex.py fuer die volle Begruendung.
    mid_doc, mid_asm, boxD, sublink = ntu.new_grand_assembly_with_sublink(
        "FixedDoubleNestedFlexMid", sub_asm, sub_path, mid_path, box_name="BoxD", link_name="SubLink"
    )
    mirror_boxB_mid = ntu.get_mirror(sublink, subB.Label)
    assert mirror_boxB_mid is not None, "Spiegel von BoxB nicht in SubLink.Group gefunden"
    middle_joint = jtu.make_joint(mid_asm, 0, boxD, mirror_boxB_mid, MIDDLE_PLC1, MIDDLE_PLC2)
    middle_joint.Label = "Joint"
    for child in mid_asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"
    mid_doc.recompute()

    # --- GrandTop: BoxC (geerdet) + MidLink(Mid) -[aeusserster Fixed-Joint]- MidLink.SubLink.BoxB (doppelt gespiegelt) ---
    # link_name="MidLink" (NICHT der Default "SubLink") - Mid selbst hat schon eine eigene
    # "SubLink"; wuerde GrandTops eigener Link ebenfalls "SubLink" heissen, wuerde FreeCAD die
    # gespiegelte Kopie von Mids "SubLink" beim Einbetten still zu "SubLink001" umbenennen
    # (Namenskollision), was get_mirror() unbrauchbar machen wuerde (live gefunden, siehe
    # Moduldocstring).
    grand_doc, grand_asm, boxC, midlink = ntu.new_grand_assembly_with_sublink(
        "FixedDoubleNestedFlexGrand", mid_asm, mid_path, grand_path, box_name="BoxC", link_name="MidLink"
    )
    boxD_mirror = ntu.get_mirror(midlink, boxD.Label)
    sublink_mirror = ntu.get_mirror(midlink, sublink.Label)
    assert boxD_mirror is not None, "Spiegel von BoxD nicht in MidLink.Group gefunden"
    assert sublink_mirror is not None, "Spiegel von SubLink nicht in MidLink.Group gefunden"
    mirror_boxB_grand = ntu.get_mirror(sublink_mirror, subB.Label)
    assert mirror_boxB_grand is not None, "Doppelt gespiegeltes BoxB nicht gefunden"

    outer_joint = jtu.make_joint(grand_asm, 0, boxC, mirror_boxB_grand, OUTER_PLC1, OUTER_PLC2)
    outer_joint.Label = "Joint"
    for child in grand_asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"
    grand_doc.recompute()

    # Absichtlich falsche Startposition NACH dem Aufbau setzen (simuliert einen "Treiber"-
    # Versuch/Drag) - WIRD NICHT MEHR KORRIGIERT, bevor gespeichert wird. Das ist der einzige
    # freie Freiheitsgrad im gesamten 3-Ebenen-System (siehe Moduldocstring von
    # test_fixed_double_nested_flex.py: BoxD und BoxC sind beide geerdet, bewegen sich nie -
    # NUR subB ist wirklich frei).
    #
    # Seeding-Hebel (siehe Moduldocstring fuer die volle Herleitung/Verifikation): subB.Placement
    # (das ECHTE, kanonische Objekt in Subs eigenem Dokument) nahe am AEUSSERSTEN
    # (GrandTop-)Ziel vorbelegen - NICHT am inneren oder mittleren Ziel, und NICHT
    # mirror_boxB_grand.Placement (waere wirkungslos, siehe
    # AssemblyObject::syncLocalMirrorPlacement()).
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
