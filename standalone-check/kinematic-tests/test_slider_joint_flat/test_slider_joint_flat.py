"""
Stufe 1, Testfall 4: Slider-Joint zwischen zwei NICHT verschachtelten Teilen.

Slider = 5 Freiheitsgrade entzogen, 1 frei: Translation NUR entlang der lokalen Z-Achse ist
frei, die dazu senkrechte Ursprungs-Komponente muss 0 sein UND es ist KEINERLEI Rotation
erlaubt (im Gegensatz zu Cylindrical, das dieselbe Translationsfreiheit hat, aber zusaetzlich
Rotation um Z zulaesst). Der konkrete Z-Versatz, den der Solver waehlt, ist nicht
solver-unabhaengig vorhersagbar - geprueft wird die Zwangsbedingung selbst (siehe
joint_test_utils.py fuer die volle Begruendung).

**WICHTIG (Nutzerauftrag 2026-09-08):** die Ausgangsdatei (inkl. absichtlich falscher
BoxB-Startposition) wird NICHT hier gebaut, sondern ist eine feste, git-getrackte Fixture
unter fixtures/test_slider_joint_flat/model.FCStd - siehe build_all_fixtures.py und
joint_test_utils.py's Moduldocstring fuer die Begruendung.

Aufruf (mit der jeweiligen FreeCAD-Binary via run-test.sh):
    ./run-test.sh <freecad-install-dir> test_slider_joint_flat.py
Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu

TEST_NAME = "test_slider_joint_flat"
JOINT_TYPE_INDEX = 3  # JointObject.JointTypes[3] == "Slider"

# Echte, aus der Edge9-Geometrie abgeleitete JCS-Placements (Nutzerkorrektur 2026-09-09: fuer
# eine Schiebeachse nimmt man eine Achse/Kante, keine Flaeche - eine Flaeche hat keine
# ausgezeichnete Achse, eine Kante schon). PLC1 (BoxA, Referenz-/Ankerseite) nutzt die
# natuerliche Kantenrichtung; PLC2 (BoxB, andockende Seite) nutzt dieselbe Kante GEFLIPPT (siehe
# joint_test_utils.py::edge_jcs_placement()) - analog zu einer realen Fuehrungsschiene.
# extra_z_rotation_deg dreht nur UM die Achse - physikalisch bedeutungslos, aber zulaessig frei.
PLC1 = jtu.edge_jcs_placement("Edge9", flip=False, extra_z_rotation_deg=30)
PLC2 = jtu.edge_jcs_placement("Edge9", flip=True, extra_z_rotation_deg=15, standoff=2)


def build_fixture():
    """Baut die feste Ausgangsdatei EINMALIG (aufgerufen von build_all_fixtures.py)."""
    fixture_file = jtu.fixture_path(THIS_DIR)
    doc, assembly, boxA, boxB = jtu.new_flat_two_box_assembly(TEST_NAME, fixture_file)
    jtu.make_joint(assembly, JOINT_TYPE_INDEX, boxA, boxB, PLC1, PLC2, elem1="Edge9", elem2="Edge9")

    # Absichtlich falsche Startposition MIT Rotation (siehe test_revolute_joint_flat.py) - ein
    # Slider muss diese Rotation komplett entfernen (im Gegensatz zu Revolute/Cylindrical, wo
    # eine Drehung um Z erlaubt bliebe), das ist gerade der interessante Unterschied dieses
    # Testfalls.
    #
    # WICHTIG (Nutzerauftrag 2026-09-09, siehe test_fixed_joint_flat.py fuer die volle
    # Begruendung): Startrotation bewusst NAHE an der analytisch korrekten Zielrotation gewaehlt
    # (empirisch als sicher bis mindestens 90 Grad Abweichung verifiziert, siehe
    # common/verify_safe_rotation_revolute_cylindrical_slider.py), statt einer willkuerlichen
    # Rotation, die zufaellig auf der falschen Seite der bekannten OndselSolver-Mehrdeutigkeit
    # haette liegen koennen.
    target_plc = boxA.Placement.multiply(PLC1).multiply(PLC2.inverse())
    start_rotation = target_plc.Rotation.multiply(App.Rotation(App.Vector(1, 1, 1), 30))
    boxB.Placement = App.Placement(App.Vector(999, -999, 999), start_rotation)
    doc.recompute()

    doc.save()
    print("Fixture geschrieben:", fixture_file)
    return doc


def load_fixture_and_solve():
    fixture_file = jtu.fixture_path(THIS_DIR)
    out_path = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD", "/tmp/kinematic_test_reload_check.FCStd")
    jtu.copy_fixture_to_output(fixture_file, out_path)

    doc = App.openDocument(out_path)
    boxA = jtu.get_by_label(doc, "BoxA")
    boxB = jtu.get_by_label(doc, "BoxB")
    assembly = doc.getObject("Assembly")

    doc.recompute()
    assembly.solve(False)
    doc.recompute()

    return doc, boxA, boxB, out_path


def check(label, boxA, boxB):
    jcs1 = jtu.jcs_world(boxA, PLC1)
    jcs2 = jtu.jcs_world(boxB, PLC2)
    rel = jtu.report(label, jcs1, jcs2)
    return jtu.check_slider(rel)


def main():
    doc, boxA, boxB, out_path = load_fixture_and_solve()

    moved = (boxB.Placement.Base - App.Vector(999, -999, 999)).Length > 1.0
    if not moved:
        print("RESULT: FAIL (BoxB wurde vom Solver nicht bewegt - Joint wirkungslos?)")
        return 1

    ok_initial = check("Direkt nach dem Solve", boxA, boxB)

    doc2, boxB2, reload_exc = jtu.save_close_reopen_recompute(doc, out_path)
    if reload_exc is not None:
        print(f"RESULT: FAIL (Exception beim Neuladen+Recompute: {reload_exc!r})")
        return 1

    boxA2 = jtu.get_by_label(doc2, "BoxA")
    ok_reload = check("Nach Speichern+Schliessen+Neuladen+Recompute", boxA2, boxB2)

    if ok_initial and ok_reload:
        print("RESULT: PASS")
        return 0
    else:
        print("RESULT: FAIL (Slider-Zwangsbedingung nach dem Solve verletzt)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
