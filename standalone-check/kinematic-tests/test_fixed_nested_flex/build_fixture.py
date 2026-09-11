"""
build_fixture.py - baut die feste Ausgangsdatei fuer test_fixed_nested_flex.py EINMALIG und
speichert sie git-getrackt unter fixtures/{sub,grand}.FCStd (relativ zu diesem eigenen
Testordner - seit der Ordner-Umstrukturierung 2026-09-09 liegt jeder Testfall in seinem
eigenen Unterordner).

NICHT Teil des automatisierten Testlaufs (run-all-tests.sh/run-test.sh ruft dieses Skript nie
auf) - nur manuell ausfuehren, wenn die Fixture-Struktur sich aendern soll (z.B. andere
Placement-Werte, ein weiterer Joint). Nutzerauftrag 2026-09-08: die Ausgangsdatei muss fuer
patched UND vanilla BYTE-IDENTISCH sein, nicht bei jedem Testlauf per Skript unter der jeweils
getesteten FreeCAD-Version neu gebaut werden - siehe nested_test_utils.py's Moduldocstring fuer
die volle Begruendung. Nach dem Bauen: `git add test_fixed_nested_flex/fixtures/`
(Binaerdateien, aber bewusst getrackt - Praezedenzfall in diesem Projekt: die frueheren
"MinimalReproGrandTop.FCStd"-Bugreport-Fixtures, siehe docs/JOURNAL.md).

Aufruf (mit einer beliebigen FreeCAD-Binary via run-test.sh - die Wahl der Installation ist
hier bewusst irrelevant, siehe unten):
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_fixed_nested_flex/build_fixture.py

**Warum die Wahl der Installation hier egal ist:** dieses Skript baut nur die Dokumentstruktur
(Objekte, Joints, Placement1/2, Referenzen) - es ruft KEINEN finalen korrigierenden solve() auf,
bevor es speichert (die absichtlich falsche BoxB-Position bleibt bestehen, siehe unten). Die
einzige Solver-Aktivitaet, die hierbei ueberhaupt laeuft, ist der automatische Presolve beim
Erstellen eines Fixed-Joints (JointUsingPreSolve) - der bringt BoxB zunaechst kurz auf die
korrekte Position, die aber gleich danach wieder ueberschrieben wird. Trotzdem: aus Konvention
mit der gepatchten Sandbox gebaut (nicht vanilla), damit ein spaeterer Nachbau bei Bedarf
denselben, bekannten Stand reproduziert.
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu
from test_fixed_nested_flex import INNER_PLC1, INNER_PLC2, OUTER_PLC1, OUTER_PLC2


def main():
    sub_path, grand_path = ntu.fixture_paths(THIS_DIR)
    for p in (sub_path, grand_path):
        os.makedirs(os.path.dirname(p), exist_ok=True)

    sub_doc, sub_asm, subA, subB = ntu.new_sub_assembly_doc("FixedNestedFlexSub")
    jtu.make_joint(sub_asm, 0, subA, subB, INNER_PLC1, INNER_PLC2)  # Fixed
    # WICHTIG (Nutzerauftrag 2026-09-09, siehe test_fixed_joint_flat.py fuer die volle
    # Begruendung): der INNERE Joint hat ebenfalls entgegengesetzte JCS-Z-Achsen
    # (INNER_PLC1/INNER_PLC2, "flush" Flaeche-auf-Flaeche) und damit dieselbe 180-Grad-
    # Mehrdeutigkeit wie der aeussere Joint. subB steht hier noch auf seiner Part::Box-
    # Standardplatzierung (Identitaet) - anders als bei mirror_boxB unten ist das HIER aber kein
    # bewusster "Treiber/Drag"-Testfall, sondern nur interner Aufbau der Sub-Fixture, die
    # korrekt aufgebaut sein soll. Deshalb direkt mit der analytisch korrekten Zielplatzierung
    # vorbelegen (subA ist geerdet/Identitaet), statt dem Zufall zu ueberlassen, welche Wurzel
    # der automatische Presolve/Solve beim recompute() trifft.
    subB.Placement = subA.Placement.multiply(INNER_PLC1).multiply(INNER_PLC2.inverse())
    sub_doc.recompute()

    grand_doc, grand_asm, boxC, sublink = ntu.new_grand_assembly_with_sublink(
        "FixedNestedFlexGrand", sub_asm, sub_path, grand_path
    )

    mirror_boxA = ntu.get_mirror(sublink, subA.Name)
    mirror_boxB = ntu.get_mirror(sublink, subB.Name)
    assert mirror_boxA is not None, "Spiegel von BoxA nicht in SubLink.Group gefunden"
    assert mirror_boxB is not None, "Spiegel von BoxB nicht in SubLink.Group gefunden"

    outer_joint = jtu.make_joint(grand_asm, 0, boxC, mirror_boxB, OUTER_PLC1, OUTER_PLC2)  # Fixed
    # Siehe test_fixed_nested_flex.py's aeltere Fassung fuer die volle Begruendung der
    # Label-Kollision (AssemblyLink::updateContents() spiegelt Sub's Joint/JointGroup als echte
    # Kopien mit denselben Labels "Joint"/"Joints" INS GrandTop-Dokument) - beide sollen
    # trotzdem "Joint"/"Joints" heissen duerfen (Nutzerauftrag 2026-09-08).
    outer_joint.Label = "Joint"
    for child in grand_asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"
    grand_doc.recompute()

    # Absichtlich falsche Position NACH dem (automatischen Presolve-)Aufbau setzen - simuliert
    # einen "Treiber"-Versuch/Drag, der die Joint-Bedingung noch nicht erfuellt (analog zu
    # test_fixed_joint_flat.py). WIRD NICHT MEHR KORRIGIERT, bevor gespeichert wird - genau
    # dieser "kaputte" Zustand ist die feste Ausgangsdatei, die patched UND vanilla
    # gleichermassen bekommen (siehe Moduldocstring/nested_test_utils.py).
    #
    # WICHTIG (Nutzerauftrag 2026-09-09, siehe Projekt-Memory
    # "reference-fixed-joint-180-degree-bug-rootcause" fuer die volle Begruendung): der
    # AEUSSERE Joint ist ein Fixed-Joint mit entgegengesetzten JCS-Z-Achsen (OUTER_PLC1/2 oben,
    # "flush" Flaeche-auf-Flaeche) - das hat im OndselSolver eine ECHTE 180-Grad-
    # Rotationsmehrdeutigkeit (Bugreport https://github.com/FreeCAD/FreeCAD/issues/32477).
    # Startrotation deshalb bewusst NAHE an der analytisch korrekten Zielrotation gewaehlt.
    # WICHTIG (Nutzerauftrag 2026-09-09, siehe Projekt-Memory
    # "todo-nested-fixed-mirror-placement-quirk" fuer die volle Herleitung): der SICHERE Hebel
    # ist subB.Placement (das ECHTE, kanonische Objekt in Sub's eigenem Dokument) - NICHT
    # mirror_boxB.Placement (nur eine kosmetische Kopie, die vom Solver bei jedem Solve/
    # Recompute aus dem echten Objekt ueberschrieben wird, siehe
    # AssemblyObject::syncLocalMirrorPlacement() - eine Zuweisung an den Spiegel wird beim
    # naechsten Solve komplett ignoriert, live per Debug bestaetigt).
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
