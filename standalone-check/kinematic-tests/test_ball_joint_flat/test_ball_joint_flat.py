"""
Stufe 1, Testfall 5: Ball-Joint (Kugelgelenk) zwischen zwei NICHT verschachtelten Teilen.

Ball = 3 Freiheitsgrade entzogen, 3 frei: nur der JCS-Ursprung muss deckungsgleich sein, die
Rotation ist komplett frei (alle 3 Rotations-Freiheitsgrade). Welche konkrete Rotation der
Solver waehlt, ist nicht solver-unabhaengig vorhersagbar - geprueft wird die Zwangsbedingung
selbst (siehe joint_test_utils.py fuer die volle Begruendung): nur die Ursprungs-Deckung.

**WICHTIG (Nutzerauftrag 2026-09-08):** die Ausgangsdatei (inkl. absichtlich falscher
BoxB-Startposition) wird NICHT hier gebaut, sondern ist eine feste, git-getrackte Fixture
unter fixtures/test_ball_joint_flat/model.FCStd - siehe build_all_fixtures.py und
joint_test_utils.py's Moduldocstring fuer die Begruendung.

Aufruf (mit der jeweiligen FreeCAD-Binary via run-test.sh):
    ./run-test.sh <freecad-install-dir> test_ball_joint_flat.py
Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu

TEST_NAME = "test_ball_joint_flat"
JOINT_TYPE_INDEX = 4  # JointObject.JointTypes[4] == "Ball"

# Echte, aus der Vertex2-Geometrie abgeleitete JCS-Placements (Nutzerkorrektur 2026-09-09: "fuer
# Ball-Joints nimmt man Punkte, keine Flaechen" - ein Kugelgelenk dreht sich um einen Punkt, eine
# Flaechennormale ist dafuer keine sinnvolle Referenz). Ursprung EXAKT auf Vertex2 (0,0,0) bei
# beiden Boxen (siehe joint_test_utils.py::vertex_jcs_placement()) - die Rotation ist fuer einen
# Ball-Joint ohnehin voellig frei (keine Orientierungs-Zwangsbedingung), die hier hinterlegten
# Rotationen dienen nur dazu, eine nicht-triviale Ausgangslage zu haben, ohne physikalische
# Bedeutung.
PLC1 = jtu.vertex_jcs_placement("Vertex2", extra_rotation_axis=App.Vector(0, 1, 0), extra_rotation_deg=30)
PLC2 = jtu.vertex_jcs_placement("Vertex2", extra_rotation_axis=App.Vector(1, 0, 0), extra_rotation_deg=15)

# Absichtlich deutlich kleinerer Versatz als bei den anderen Jointtypen (nicht (999,-999,999))
# - live festgestellt: bei einem Ball-Joint sind ALLE 3 Rotations-Freiheitsgrade frei, es gibt
# also keinerlei Rotationszwang, der das Newton-Verfahren in Richtung Loesung "fuehrt". Aus
# einer sehr weit entfernten Startposition heraus konvergiert der Solver dann nicht (Err:
# "Solve failed: iterNo > iterMax") - reine Konvergenzradius-Eigenschaft des Loesers, kein
# Test- oder Joint-Fehler.
#
# WICHTIG (Nutzerauftrag 2026-09-09, per Sweep neu vermessen, siehe
# test_ball_joint_flat/verify_safe_rotation.py): mit der jetzigen Vertex2-Geometrie (statt der
# fruehren Face1+Standoff-Konstruktion) ist das Konvergenzfenster deutlich GROESSER - der ganze
# getestete Bereich von 3mm bis 30mm konvergiert einwandfrei. Die frueher gefundene sehr enge
# Grenze (5-5.5mm) war offenbar spezifisch fuer die kuenstliche Face-Konstruktion. 4.5mm bleibt
# trotzdem als moderater, definitiv sicherer Wert bestehen.
INITIAL_BASE = App.Vector(10, -8, 6).normalize() * 4.5  # Laenge 4.5mm


def build_fixture():
    """Baut die feste Ausgangsdatei EINMALIG (aufgerufen von build_all_fixtures.py)."""
    fixture_file = jtu.fixture_path(THIS_DIR)
    doc, assembly, boxA, boxB = jtu.new_flat_two_box_assembly(TEST_NAME, fixture_file)
    jtu.make_joint(assembly, JOINT_TYPE_INDEX, boxA, boxB, PLC1, PLC2, elem1="Vertex2", elem2="Vertex2")

    boxB.Placement = App.Placement(INITIAL_BASE, App.Rotation(App.Vector(0, 1, 0), 77))
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
    return jtu.check_ball(rel)


def main():
    doc, boxA, boxB, out_path = load_fixture_and_solve()

    # Sanity-Check: Solver muss BoxB spuerbar von der absichtlich falschen Startposition weg
    # bewegt haben (Schwelle deutlich kleiner als die ~14mm Anfangsabweichung, aber gross genug
    # um numerisches Rauschen auszuschliessen).
    moved = (boxB.Placement.Base - INITIAL_BASE).Length > 1.0
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
        print("RESULT: FAIL (Ball-Zwangsbedingung nach dem Solve verletzt)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
