"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_cylindrical_nested_flex.py EINMALIG und
speichert sie git-getrackt unter fixtures/{sub,grand}.FCStd (relativ zu diesem eigenen
Testordner). Analog zu test_fixed_nested_flex/build_fixture.py, nur mit Cylindrical statt Fixed als
INNEREM Joint (Nutzerauftrag 2026-09-09: "aeusserer Joint ist IMMER Fixed" bleibt unveraendert,
nur der innere variiert je Jointtyp-Testfall).

NICHT Teil des automatisierten Testlaufs - nur manuell ausfuehren, wenn sich die Fixture-
Struktur aendern soll. Nach dem Bauen: `git add test_slider_nested_flex/fixtures/`.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_slider_nested_flex/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu
from test_cylindrical_nested_flex import INNER_PLC1, INNER_PLC2, OUTER_PLC1, OUTER_PLC2


def main():
    sub_path, grand_path = ntu.fixture_paths(THIS_DIR)
    for p in (sub_path, grand_path):
        os.makedirs(os.path.dirname(p), exist_ok=True)

    sub_doc, sub_asm, subA, subB = ntu.new_sub_assembly_doc("CylindricalNestedFlexSub", sub_path)
    jtu.make_joint(sub_asm, 2, subA, subB, INNER_PLC1, INNER_PLC2, elem1="Edge9", elem2="Edge9")  # Cylindrical
    # WICHTIG (Nutzerauftrag 2026-09-09, siehe test_fixed_nested_flex/build_fixture.py fuer die
    # volle Begruendung): subB wird hier NUR fuer den internen Aufbau der Sub-Fixture korrekt
    # vorbelegt (kein bewusster "Treiber/Drag"-Testfall) - Cylindrical hat 2 freie Freiheitsgrade
    # (Translation entlang der Kantenachse), die Vorbelegung waehlt bewusst den Punkt OHNE
    # zusaetzlichen Z-Versatz (denselben, den auch INNER_PLC1/2 selbst schon vorgeben).
    subB.Placement = subA.Placement.multiply(INNER_PLC1).multiply(INNER_PLC2.inverse())
    sub_doc.recompute()

    grand_doc, grand_asm, boxC, sublink = ntu.new_grand_assembly_with_sublink(
        "CylindricalNestedFlexGrand", sub_asm, sub_path, grand_path
    )

    mirror_boxA = ntu.get_mirror(sublink, subA.Label)
    mirror_boxB = ntu.get_mirror(sublink, subB.Label)
    assert mirror_boxA is not None, "Spiegel von BoxA nicht in SubLink.Group gefunden"
    assert mirror_boxB is not None, "Spiegel von BoxB nicht in SubLink.Group gefunden"

    outer_joint = jtu.make_joint(grand_asm, 0, boxC, mirror_boxB, OUTER_PLC1, OUTER_PLC2)  # Fixed
    outer_joint.Label = "Joint"
    for child in grand_asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"
    grand_doc.recompute()

    # WICHTIG (Nutzerauftrag 2026-09-09, siehe test_fixed_nested_flex/build_fixture.py fuer die
    # volle Begruendung): der SICHERE Hebel fuer die Startplatzierung ist subB.Placement (das
    # ECHTE, kanonische Objekt in Sub's eigenem Dokument), NICHT mirror_boxB.Placement (nur eine
    # kosmetische, vom Solver ueberschriebene Kopie - siehe
    # AssemblyObject::syncLocalMirrorPlacement()). Startrotation nahe am analytisch korrekten
    # Zielwert fuer den AEUSSEREN Joint.
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
