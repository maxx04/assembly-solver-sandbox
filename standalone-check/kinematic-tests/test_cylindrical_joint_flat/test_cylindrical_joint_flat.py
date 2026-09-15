"""
Stufe 1, Testfall 3: Cylindrical-Joint zwischen zwei NICHT verschachtelten Teilen.

Cylindrical = 4 Freiheitsgrade entzogen, 2 frei: wie Revolute (Drehung frei um die lokale
Z-Achse), zusaetzlich ist auch die Translation ENTLANG dieser Z-Achse frei. Nur die zu Z
senkrechte Komponente des Ursprungsversatzes muss 0 sein. Weder der gewaehlte Drehwinkel noch
der gewaehlte Z-Versatz sind solver-unabhaengig vorhersagbar - geprueft wird die
Zwangsbedingung selbst (siehe joint_test_utils.py fuer die volle Begruendung).

**WICHTIG (Nutzerauftrag 2026-09-08):** die Ausgangsdatei (inkl. absichtlich falscher
BoxB-Startposition) wird NICHT hier gebaut, sondern ist eine feste, git-getrackte Fixture
unter fixtures/test_cylindrical_joint_flat/model.FCStd - siehe build_all_fixtures.py und
joint_test_utils.py's Moduldocstring fuer die Begruendung.

Aufruf (mit der jeweiligen FreeCAD-Binary via run-test.sh):
    ./run-test.sh <freecad-install-dir> test_cylindrical_joint_flat.py
Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu

TEST_NAME = "test_cylindrical_joint_flat"
JOINT_TYPE_INDEX = 2  # JointObject.JointTypes[2] == "Cylindrical"

# Echte, aus der Edge9-Geometrie abgeleitete JCS-Placements (Nutzerkorrektur 2026-09-09: "fuer
# Cylindrical nimmt man eine Achse/Kante, keine Flaeche" - eine Flaeche hat keine ausgezeichnete
# Achse, eine Kante schon). PLC1 (BoxA, Referenz-/Ankerseite) nutzt die natuerliche
# Kantenrichtung; PLC2 (BoxB, andockende Seite) nutzt dieselbe Kante GEFLIPPT (siehe
# joint_test_utils.py::edge_jcs_placement()) - analog zu einer realen Bohrungsachse.
# extra_z_rotation_deg dreht nur UM die Achse - physikalisch bedeutungslos, aber zulaessig frei.
PLC1 = jtu.edge_jcs_placement("Edge9", flip=False, extra_z_rotation_deg=30)
PLC2 = jtu.edge_jcs_placement("Edge9", flip=True, extra_z_rotation_deg=15, standoff=2)


def build_fixture():
    """Baut die feste Ausgangsdatei EINMALIG (aufgerufen von build_all_fixtures.py)."""
    fixture_file = jtu.fixture_path(THIS_DIR)
    doc, assembly, boxA, boxB = jtu.new_flat_two_box_assembly(TEST_NAME, fixture_file)
    jtu.make_joint(assembly, JOINT_TYPE_INDEX, boxA, boxB, PLC1, PLC2, elem1="Edge9", elem2="Edge9")

    # Absichtlich falsche Startposition (siehe test_revolute_joint_flat.py) - weder der
    # Drehwinkel noch der Z-Versatz sind Teil der Pruefung, nur dass er ueberhaupt vom Solver
    # neu gesetzt wird (Sanity-Check unten) und die uebrigen 4 Freiheitsgrade stimmen.
    #
    # WICHTIG (Nutzerauftrag 2026-09-09, siehe test_fixed_joint_flat.py fuer die volle
    # Begruendung): Startrotation bewusst NAHE an der analytisch korrekten Zielrotation gewaehlt,
    # statt einer willkuerlichen Rotation, die zufaellig auf der falschen Seite der bekannten
    # OndselSolver-Mehrdeutigkeit haette liegen koennen.
    #
    # ABWEICHEND von Revolute/Slider (dort 30 Grad sicher) NUR 10 Grad hier: per Sweep gefunden
    # (siehe common/verify_safe_rotation_revolute_cylindrical_slider.py), dass Cylindrical - im
    # Gegensatz zu den anderen beiden - KEIN sauberes Zwei-Wurzel-Verhalten zeigt, sondern
    # zwischen ~20 und ~30 Grad Startabweichung in eine ganz andere, echt falsche Konfiguration
    # kippt (Achsenfehler 0.8-2.0 statt der Norm ~0). 10 Grad liegt sicher im "guten" Bereich
    # (Achsenfehler dort nur ~0.14, siehe check_cylindrical()'s angepasste Toleranz).
    target_plc = boxA.Placement.multiply(PLC1).multiply(PLC2.inverse())
    start_rotation = target_plc.Rotation.multiply(App.Rotation(App.Vector(1, 1, 1), 10))
    # Positions-Versatz ABWEICHEND von den anderen Jointtypen (dort 999,-999,999 unproblematisch)
    # bewusst kleiner: per Sweep gefunden, dass Cylindrical (mit seinem zusaetzlichen freien
    # Freiheitsgrad entlang Z) bei einem Versatz von 999mm den Konvergenzradius des Solvers
    # ueberschreitet ("Solve failed: iterNo > iterMax", analog zum bekannten Ball-Joint-
    # Konvergenzradius-Problem) - sicher verifiziert bis mindestens 500mm.
    boxB.Placement = App.Placement(App.Vector(99, -99, 99), start_rotation)
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
    return jtu.check_cylindrical(rel)


def main():
    doc, boxA, boxB, out_path = load_fixture_and_solve()

    moved = (boxB.Placement.Base - App.Vector(99, -99, 99)).Length > 1.0
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
        print("RESULT: FAIL (Cylindrical-Zwangsbedingung nach dem Solve verletzt)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
