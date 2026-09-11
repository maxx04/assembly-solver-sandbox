"""
Stufe 1, Testfall 1: Fixed-Joint zwischen zwei NICHT verschachtelten Teilen.

Methodik (Nutzerauftrag 2026-09-06): erst analytisch berechnen, wie das Ergebnis aussehen
MUSS, dann gegen "normales" (unmodifiziertes) FreeCAD und gegen unseren gepatchten Stand
vergleichen - alles headless/autonom, keine manuelle Interaktion.

Fixed-Joint = 0 Freiheitsgrade: die JCS-Ausrichtung erzwingt eindeutig
    BoxA.Placement * Placement1 == BoxB.Placement * Placement2
    <=> BoxB.Placement = BoxA.Placement * Placement1 * Placement2.inverse()
Das ist reine Placement-Algebra, unabhaengig vom Solver - die "analytisch erwartete"
Referenz fuer diesen Testfall.

**WICHTIG (Nutzerauftrag 2026-09-08):** die Ausgangsdatei (inkl. absichtlich falscher
BoxB-Startposition) wird NICHT hier gebaut, sondern ist eine feste, git-getrackte Fixture
unter fixtures/test_fixed_joint_flat/model.FCStd - siehe build_all_fixtures.py fuer den
(einmaligen) Bauschritt und joint_test_utils.py's Moduldocstring fuer die Begruendung
("gleicher Ausgangszustand fuer patched UND vanilla, um wirklich nur das Solver-Verhalten zu
vergleichen, nicht ob das Konstruktions-Skript unter beiden Versionen dasselbe Dokument
erzeugt").

Aufruf (mit der jeweiligen FreeCAD-Binary via run-test.sh):
    ./run-test.sh <freecad-install-dir> test_fixed_joint_flat.py
Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus (fuer Skript-
Auswertung von aussen), plus die berechneten/gemessenen Placements zur Diagnose.
"""
import os
import sys
import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu

TEST_NAME = "test_fixed_joint_flat"

# Muessen exakt den Werten entsprechen, mit denen build_all_fixtures.py die Fixture gebaut hat
# (dort ist dies die Quelle der Wahrheit) - hier nur zur Berechnung der analytischen Erwartung
# dupliziert.
# Echte, aus der Face1-Geometrie abgeleitete JCS-Placements (Nutzerauftrag 2026-09-08) -
# NICHT mehr frei erfunden. PLC1 (BoxA, Referenz-/Ankerseite) nutzt die natuerliche
# AUSWAERTIGE Flaechennormale; PLC2 (BoxB, andockende Seite) nutzt dieselbe Flaeche GEFLIPPT
# (siehe joint_test_utils.py::face_jcs_placement() - identische Logik wie FreeCADs eigenes
# UtilsAssembly.flipPlacement()/arePlacementSameDir()) - dadurch landen BoxA und BoxB nach dem
# Solve physikalisch sinnvoll Flaeche-auf-Flaeche zueinander (nicht ineinander verschachtelt).
# extra_z_rotation_deg dreht nur UM die Achse (aendert nicht wohin die Flaeche zeigt) - das ist
# die einzige noch "frei" gewaehlte Groesse.
PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=30)
PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=15, standoff=2)


def build_fixture():
    """Baut die feste Ausgangsdatei EINMALIG (aufgerufen von build_all_fixtures.py) - baut die
    Struktur exakt wie zuvor, laesst aber BoxB an der absichtlich falschen Startposition
    stehen (kein abschliessender solve()), damit patched UND vanilla exakt denselben,
    unkorrigierten Ausgangszustand bekommen."""
    doc = App.newDocument("FixedJointFlatTest")

    boxA = doc.addObject("Part::Box", "BoxA")
    boxB = doc.addObject("Part::Box", "BoxB")
    doc.recompute()

    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    assembly.addObject(boxA)
    assembly.addObject(boxB)
    doc.recompute()

    # BoxA erden: Placement schreibgeschuetzt setzen, dann syncGroundedJoints() (via solve())
    # legt automatisch das GroundedJoint-Objekt an (siehe docs/ARCHITECTURE.md, Abschnitt 1.1).
    boxA.setPropertyStatus("Placement", "ReadOnly")

    import UtilsAssembly
    import JointObject

    joint_group = UtilsAssembly.getJointGroup(assembly)

    joint = joint_group.newObject("App::FeaturePython", "Joint")
    JointObject.Joint(joint, 0)  # 0 = "Fixed", siehe JointObject.JointTypes
    if App.GuiUp:
        # Ohne diesen Proxy crasht redrawJointPlacements() beim naechsten Solve NACH einem
        # Speichern+Neuladen (Document.cpp: "Unknown exception" / AttributeError: 'int' object
        # has no attribute 'redrawJointPlacements') - im laufenden Erstellungs-Prozess faellt
        # das nicht auf, weil ViewObject dort zu diesem Zeitpunkt noch None ist. Analog zu
        # CommandCreateJoint.py's Muster (JointObject.Joint(...) + ViewProviderJoint(...)).
        JointObject.ViewProviderJoint(joint.ViewObject)
    # Sub-Element-Name DOPPELT noetig, nicht nur einmal (live entdeckt: der Bearbeiten-Dialog
    # zeigte trotz des Face1-Fixes weiterhin "invalid Reference" und keine Referenz-Anzeige) -
    # siehe Projekt-Memory "reference-joint-double-subelement". Detach1/2=True sorgt trotzdem
    # dafuer, dass Placement1/2 NICHT aus dieser Flaeche neu berechnet werden, sondern unsere
    # hart gesetzten Werte unten gelten.
    joint.Reference1 = (boxA, ["Face1", "Face1"])
    joint.Reference2 = (boxB, ["Face1", "Face1"])
    joint.Detach1 = True  # verhindert, dass updateJCSPlacements() unsere Placement1/2 ueberschreibt
    joint.Detach2 = True
    joint.Placement1 = PLC1
    joint.Placement2 = PLC2

    # Absichtlich falsche Startposition fuer BoxB - simuliert einen "Treiber"-Versuch/Drag, der
    # die Joint-Bedingung noch nicht erfuellt. WIRD NICHT MEHR KORRIGIERT, bevor gespeichert
    # wird - genau dieser "kaputte" Zustand ist die feste Ausgangsdatei fuer beide Installationen.
    #
    # WICHTIG (Nutzerauftrag 2026-09-09, siehe Projekt-Memory
    # "reference-fixed-joint-180-degree-bug-rootcause"): ein Fixed-Joint mit entgegengesetzten
    # JCS-Z-Achsen (genau unsere PLC1/PLC2-Konfiguration oben, "flush" Flaeche-auf-Flaeche) hat
    # im OndselSolver eine ECHTE, reproduzierbare Mehrdeutigkeit - die 3 Orthogonalitaets-
    # bedingungen erfuellen sowohl die richtige Rotation als auch eine um 180 Grad verdrehte
    # falsch-aber-gueltige Alternativloesung gleichermassen. Newton konvergiert zu welcher der
    # beiden Wurzeln naeher an der STARTROTATION liegt (Position ist irrelevant - empirisch
    # bestaetigt von 5mm bis 999mm Versatz, identisches Ergebnis). Eine Startrotation von
    # App.Rotation() (Identitaet) lag zufaellig naeher an der FALSCHEN Wurzel -> Test schlug mit
    # exakt 180 Grad Rotationsabweichung fehl (kein Test-Bug, sondern der reale, gemeldete
    # Solver-Bug, siehe https://github.com/FreeCAD/FreeCAD/issues/32477).
    #
    # Fix (analog zur bereits geloesten Ball-Joint-Konvergenzradius-Frage in
    # test_ball_joint_flat.py): Startrotation bewusst NAHE an der analytisch korrekten
    # Zielrotation waehlen (empirisch verifiziert unten per Sweep in
    # verify_fixed_joint_flat_safe_rotation.py, siehe dort), statt bei Identitaet zu starten.
    # Das testet weiterhin echt, dass der Solver eine falsche Ausgangslage korrigiert (Position
    # UND ein deutlicher Rotations-Offset), landet aber verlaesslich auf der richtigen Wurzel.
    target_plc = boxA.Placement.multiply(PLC1).multiply(PLC2.inverse())
    start_rotation = target_plc.Rotation.multiply(App.Rotation(App.Vector(1, 1, 1), 30))
    boxB.Placement = App.Placement(App.Vector(999, -999, 999), start_rotation)
    doc.recompute()

    fixture_file = jtu.fixture_path(THIS_DIR)
    os.makedirs(os.path.dirname(fixture_file), exist_ok=True)
    doc.saveAs(fixture_file)
    print("Fixture geschrieben:", fixture_file)
    return doc


def load_fixture_and_solve():
    fixture_file = jtu.fixture_path(THIS_DIR)
    out_path = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD", "/tmp/kinematic_test_reload_check.FCStd")
    jtu.copy_fixture_to_output(fixture_file, out_path)

    doc = App.openDocument(out_path)
    boxA = doc.getObject("BoxA")
    boxB = doc.getObject("BoxB")
    assembly = doc.getObject("Assembly")

    doc.recompute()
    assembly.solve(False)
    doc.recompute()

    return doc, boxA.Placement, boxB.Placement, out_path


def check_placement(label, boxA_plc, boxB_plc_actual, tol_pos=1e-6, tol_rot=1e-4):
    boxB_plc_expected = boxA_plc.multiply(PLC1).multiply(PLC2.inverse())
    pos_diff = (boxB_plc_expected.Base - boxB_plc_actual.Base).Length
    rot_diff_deg = abs(
        boxB_plc_expected.Rotation.multiply(boxB_plc_actual.Rotation.inverted()).Angle
    ) * 180.0 / 3.141592653589793

    print(f"--- {label} ---")
    print("BoxB.Placement (analytisch erwartet):", boxB_plc_expected)
    print("BoxB.Placement (tatsaechlich):        ", boxB_plc_actual)
    print(f"Positions-Differenz: {pos_diff:.9f} mm (Toleranz {tol_pos})")
    print(f"Rotations-Differenz: {rot_diff_deg:.9f} deg (Toleranz {tol_rot})")

    return pos_diff <= tol_pos and rot_diff_deg <= tol_rot


def main():
    doc, boxA_plc, boxB_plc_actual, out_path = load_fixture_and_solve()
    ok_initial = check_placement("Direkt nach dem Solve", boxA_plc, boxB_plc_actual)

    print("Testdatei gespeichert:", out_path)

    # WICHTIG (2026-09-06, live vom Nutzer entdeckt): ein Ergebnis, das direkt nach dem
    # Erstellen stimmt, kann trotzdem beim Neuladen crashen oder ungueltig werden (fehlender
    # ViewProvider-Proxy fuehrte zu "list index out of range"/AttributeError erst NACH einem
    # Speichern+Schliessen+Neuladen-Zyklus - im laufenden Erstellungsprozess fiel das nicht
    # auf). Deshalb ab jetzt fester Bestandteil des PASS-Kriteriums: Dokument schliessen,
    # neu oeffnen, neu rechnen, Ergebnis erneut pruefen.
    docname = doc.Name
    doc.save()
    App.closeDocument(docname)
    doc2 = App.openDocument(out_path)
    boxA2 = doc2.getObject("BoxA")
    boxB2 = doc2.getObject("BoxB")
    reload_exception = None
    try:
        doc2.recompute()
    except Exception as exc:  # noqa: BLE001 - bewusst breit, das IST der Test
        reload_exception = exc

    if reload_exception is not None:
        print(f"RESULT: FAIL (Exception beim Neuladen+Recompute: {reload_exception!r})")
        return 1

    ok_reload = check_placement(
        "Nach Speichern+Schliessen+Neuladen+Recompute", boxA2.Placement, boxB2.Placement
    )

    if ok_initial and ok_reload:
        print("RESULT: PASS")
        return 0
    else:
        print("RESULT: FAIL (Solver-Ergebnis weicht von analytischer Erwartung ab)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
