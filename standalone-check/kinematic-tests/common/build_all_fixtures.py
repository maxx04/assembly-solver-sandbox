"""
build_all_fixtures.py - baut die festen Ausgangsdateien ALLER flachen Testfaelle (Stufe 1)
EINMALIG und speichert sie git-getrackt unter test_<jointtyp>_flat/fixtures/model.FCStd (seit
der Ordner-Umstrukturierung 2026-09-09: je Testfall ein eigener Unterordner der kinematic-tests/
-Wurzel, nicht mehr zentral).

NICHT Teil des automatisierten Testlaufs (run-all-tests.sh/run-test.sh ruft dieses Skript nie
auf, da sein Dateiname nicht dem "test_*.py"-Glob entspricht) - nur manuell ausfuehren, wenn
sich die Fixture-Struktur eines Testfalls aendern soll (z.B. andere Placement-Werte). Nutzer-
auftrag 2026-09-08: die Ausgangsdatei muss fuer patched UND vanilla BYTE-IDENTISCH sein, nicht
bei jedem Testlauf per Skript unter der jeweils getesteten FreeCAD-Version neu gebaut werden -
siehe joint_test_utils.py's Moduldocstring fuer die volle Begruendung. Nach dem Bauen:
`git add test_*_flat/fixtures/` (Binaerdateien, aber bewusst getrackt - Praezedenzfall in
diesem Projekt: die frueheren "MinimalReproGrandTop.FCStd"-Bugreport-Fixtures, siehe
docs/JOURNAL.md).

Fuer den verschachtelten Testfall (Stufe 2) siehe stattdessen
test_fixed_nested_flex/build_fixture.py (eigenes Skript, da zwei Dokumente + dokument-
uebergreifender Link involviert sind).

Aufruf (mit einer beliebigen FreeCAD-Binary via run-test.sh - die Wahl der Installation ist
bewusst irrelevant, siehe Moduldocstring der einzelnen Testfaelle: es wird kein abschliessender
korrigierender solve() aufgerufen, bevor gespeichert wird):
    ./common/run-test.sh /home/maxx/freecad-sandbox/install common/build_all_fixtures.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
KINEMATIC_TESTS_ROOT = os.path.dirname(THIS_DIR)
sys.path.insert(0, THIS_DIR)
for _test_folder in (
    "test_fixed_joint_flat",
    "test_revolute_joint_flat",
    "test_cylindrical_joint_flat",
    "test_slider_joint_flat",
    "test_ball_joint_flat",
):
    sys.path.insert(0, os.path.join(KINEMATIC_TESTS_ROOT, _test_folder))

import test_fixed_joint_flat
import test_revolute_joint_flat
import test_cylindrical_joint_flat
import test_slider_joint_flat
import test_ball_joint_flat

BUILDERS = [
    test_fixed_joint_flat,
    test_revolute_joint_flat,
    test_cylindrical_joint_flat,
    test_slider_joint_flat,
    test_ball_joint_flat,
]


def main():
    for module in BUILDERS:
        print(f"=== {module.TEST_NAME} ===")
        module.build_fixture()
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
