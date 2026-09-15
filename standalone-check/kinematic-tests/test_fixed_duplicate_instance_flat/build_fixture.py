"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_fixed_duplicate_instance_flat.py
EINMALIG (siehe test_fixed_nested_flex/build_fixture.py fuer die volle Begruendung der
Fixture-Methodik - identisches Vorgehen hier, nur mit einer ZWEITEN AssemblyLink-Instanz).

Aufruf:
    ./run-test.sh /home/maxx/freecad-sandbox/install test_fixed_duplicate_instance_flat/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu
from test_fixed_duplicate_instance_flat import INNER_PLC1, INNER_PLC2, OUTER_PLC1, OUTER_PLC2


def main():
    sub_path, grand_path = ntu.fixture_paths(THIS_DIR)
    for p in (sub_path, grand_path):
        os.makedirs(os.path.dirname(p), exist_ok=True)

    sub_doc, sub_asm, subA, subB = ntu.new_sub_assembly_doc("DupInstFlatSub", sub_path)
    jtu.make_joint(sub_asm, 0, subA, subB, INNER_PLC1, INNER_PLC2)  # Fixed
    # Analytisch korrekte Zielplatzierung vorbelegen (siehe test_fixed_nested_flex/build_fixture.py
    # fuer die Begruendung, warum das HIER - anders als bei GrandTops absichtlich falscher
    # Startposition unten - kein bewusster Test-Faktor ist, sondern nur ein sauberer Fixture-Aufbau).
    subB.Placement = subA.Placement.multiply(INNER_PLC1).multiply(INNER_PLC2.inverse())
    sub_doc.recompute()

    grand_doc, grand_asm, boxC, sublink1 = ntu.new_grand_assembly_with_sublink(
        "DupInstFlatGrand", sub_asm, sub_path, grand_path
    )

    mirror_boxB1 = ntu.get_mirror(sublink1, subB.Label)
    assert mirror_boxB1 is not None, "Spiegel von BoxB (Instanz 1) nicht in SubLink.Group gefunden"

    outer_joint1 = jtu.make_joint(grand_asm, 0, boxC, mirror_boxB1, OUTER_PLC1, OUTER_PLC2)  # Fixed
    outer_joint1.Label = "Joint"
    for child in grand_asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"
    grand_doc.recompute()

    # Zweite Instanz derselben verlinkten Sub-Baugruppe einfuegen - EXAKT wie
    # CommandInsertLink.py::onItemClicked() das fuer den Nutzer tut (siehe docs/JOURNAL.md /
    # ARCHITECTURE.md fuer die Herleitung dieses Testfalls). Der Wunschname "SubLink" kollidiert
    # bewusst mit der ersten Instanz - genau das loest FreeCADs automatische Umbenennung
    # ("SubLink001") aus, die Bug A ausgeloest hat.
    sublink2 = grand_asm.newObject("Assembly::AssemblyLink", sub_asm.Label)
    sublink2.LinkedObject = sub_asm
    sublink2.Rigid = False
    sublink2.Label = sub_asm.Label
    grand_asm.addObject(sublink2)
    grand_doc.recompute()

    mirror_boxB2 = None
    for child in sublink2.Group:
        if child.LinkedObject == subB:
            mirror_boxB2 = child
            break
    assert mirror_boxB2 is not None, "Spiegel von BoxB (Instanz 2) nicht gefunden"

    # ZWEITER aeusserer Joint, mit DENSELBEN Placement-Werten wie der erste (physikalisch
    # konsistentes Duplikat, siehe Moduldocstring von test_fixed_duplicate_instance_flat.py).
    outer_joint2 = jtu.make_joint(grand_asm, 0, boxC, mirror_boxB2, OUTER_PLC1, OUTER_PLC2)  # Fixed
    grand_doc.recompute()

    # Absichtlich falsche Position NACH dem Aufbau setzen (siehe test_fixed_nested_flex/
    # build_fixture.py fuer die volle Begruendung) - ueber das ECHTE Sub#BoxB, das WIRKT sich auf
    # BEIDE Spiegel gleichermassen aus (es gibt nur ein einziges reales Sub#BoxB).
    target_plc = boxC.Placement.multiply(OUTER_PLC1).multiply(OUTER_PLC2.inverse())
    start_rotation = target_plc.Rotation.multiply(App.Rotation(App.Vector(1, 1, 1), 30))
    subB.Placement = App.Placement(App.Vector(999, -999, 999), start_rotation)
    sub_doc.recompute()

    grand_doc.save()
    sub_doc.save()

    print(f"Fixture geschrieben: {sub_path}")
    print(f"Fixture geschrieben: {grand_path}")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
