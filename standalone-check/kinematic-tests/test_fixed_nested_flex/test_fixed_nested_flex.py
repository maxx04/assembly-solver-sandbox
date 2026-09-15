"""
Stufe 2, Testfall 1: EINE Ebene Verschachtelung ueber eine FLEXIBLE Assembly::AssemblyLink,
mit je einem Fixed-Joint innen und aussen.

Struktur (siehe nested_test_utils.py fuer die volle Herleitung/Nomenklatur):
    Sub (eigenes Dokument):     BoxA (GEERDET, interner Referenzpunkt) -[innerer Fixed-Joint]- BoxB
    GrandTop (eigenes Dokument): BoxC (geerdet) -[aeusserer Fixed-Joint]- SubLink.BoxB (Spiegel)

BoxA's Erdung IN Sub ist normale Konstrukteurspraxis (Nutzer-Korrektur 2026-09-08), kein
Widerspruch zum aeusseren Joint - siehe nested_test_utils.py's Moduldocstring fuer die volle
Begruendung samt Live-Verifikation (mit UND ohne Erdung getestet: die reine Positionsberechnung
ist in beiden Faellen bei patched UND vanilla identisch korrekt).

Das ist bewusst der einfachste moegliche Verschachtelungsfall (0 Freiheitsgrade auf beiden
Ebenen, damit weiterhin eine vollstaendig geschlossene analytische Referenz existiert, exakt wie
bei den flachen Fixed-Joint-Tests) - geprueft werden sowohl die Kernberechnung (Placement-Kette
ueber die Verschachtelungsgrenze hinweg) ALS AUCH die Synchronisation zwischen Sub's eigenem
Dokument und seinem Spiegel in GrandTop (`syncLocalMirrorPlacement()`, siehe
docs/ARCHITECTURE.md Abschnitt 3, Bug 8). **Ergebnis (2026-09-08, nach Fixture-Methodik-Fix,
siehe nested_test_utils.py's Moduldocstring):** fuer DIESEN einfachsten Fall zeigt sich AKTUELL
KEIN Unterschied zwischen gepatchtem und vanilla FreeCAD - beide bestehen alle Pruefungen exakt
(ein frueher gefundener scheinbarer Vanilla-Sync-Bug erwies sich als Artefakt der damaligen,
fehlerhaften Testkonstruktion, nicht als echter Unterschied - siehe README.md
"Methodik-Korrektur").

**WICHTIG (Nutzerauftrag 2026-09-08):** die Ausgangs-.FCStd-Dateien (Sub+GrandTop, inkl. einer
absichtlich falschen BoxB-Startposition) werden NICHT hier gebaut, sondern sind eine feste,
git-getrackte Fixture unter fixtures/test_fixed_nested_flex/ - siehe
build_fixture_test_fixed_nested_flex.py fuer den (einmaligen) Bauschritt und
nested_test_utils.py's Moduldocstring fuer die Begruendung ("gleicher Ausgangszustand fuer
patched UND vanilla, um wirklich nur das Solver-Verhalten zu vergleichen, nicht ob das
Konstruktions-Skript unter beiden Versionen dasselbe Dokument erzeugt").

Analytisch erwartet (reine Placement-Algebra, Solver-unabhaengig):
    BoxB_erwartet = BoxC.Placement * outerPlc1 * outerPlc2.inverse()
    BoxA_erwartet = BoxB_erwartet  * innerPlc2 * innerPlc1.inverse()
(die Placement1/2-Werte der beiden Joints stehen fest in der Fixture - siehe FIXTURE_* unten,
identisch zu den Werten, mit denen build_fixture_test_fixed_nested_flex.py sie erzeugt hat.)

Aufruf (mit der jeweiligen FreeCAD-Binary via run-test.sh):
    ./run-test.sh <freecad-install-dir> test_fixed_nested_flex.py
Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu

TEST_NAME = "test_fixed_nested_flex"

# Echte, aus der Face-Geometrie abgeleitete JCS-Placements (Nutzerauftrag 2026-09-08) - NICHT
# mehr frei erfunden, siehe joint_test_utils.py::face_jcs_placement() fuer die Begruendung.
# "Sandwich"-Struktur: BoxA -[Face1]- BoxB -[Face2]- BoxC, d.h. BoxB beruehrt BoxA an ihrer
# Face1 (innerer Joint) und BoxC an ihrer GEGENUEBERLIEGENDEN Face2 (aeusserer Joint) - eine
# physikalisch kohaerente Kette, keine willkuerlichen Zahlen. Muessen exakt den Werten
# entsprechen, mit denen build_fixture_test_fixed_nested_flex.py die Fixture gebaut hat (dort
# ist dies die Quelle der Wahrheit) - hier nur zur Berechnung der analytischen Erwartung
# dupliziert.
INNER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # BoxA
INNER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # BoxB
OUTER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=30)  # BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face2", flip=True, extra_z_rotation_deg=15)  # Spiegel von BoxB


def _output_paths():
    # Eigener Unterordner PRO INSTALLATION mit den ORIGINALEN Dateinamen der Fixture - NICHT
    # umbenannte Kopien im selben Ordner (Nutzerauftrag 2026-09-08, live gefunden: grand.FCStd
    # verweist auf Sub intern per BLOSSEM Dateinamen, relativ zum eigenen Verzeichnis - eine
    # Kopie mit ANDEREM Namen macht daraus einen GEBROCHENEN Link ("Link broken! Object: BoxB
    # File: ..."), wodurch der innere Joint beim Solve komplett unbemerkt aus dem System faellt
    # - keine Exception, nur ein falsches Ergebnis). Deshalb: Verzeichnis nach Installationsname
    # benennen, Dateien darin behalten exakt dieselben Namen wie die Fixture (seit 2026-09-09
    # testname-praefigiert statt "sub.FCStd"/"grand.FCStd", siehe
    # nested_test_utils.py::fixture_paths() - der Praefix muss hier UND dort identisch sein).
    fixture_sub, fixture_grand = ntu.fixture_paths(THIS_DIR)
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    if base:
        install_dir = os.path.splitext(base)[0]  # .../fcstd-output/test_fixed_nested_flex/<install-tag>
    else:
        install_dir = os.path.join(THIS_DIR, "fcstd-output", "default")
    os.makedirs(install_dir, exist_ok=True)
    return (
        os.path.join(install_dir, os.path.basename(fixture_sub)),
        os.path.join(install_dir, os.path.basename(fixture_grand)),
    )


def load_fixture_and_solve():
    """Kopiert die feste Fixture in den eigenen Ausgabepfad, oeffnet die Kopie und loest sie -
    das ist der einzige Teil, der sich zwischen einem patched- und einem vanilla-Lauf
    unterscheiden kann (siehe Moduldocstring)."""
    fixture_sub, fixture_grand = ntu.fixture_paths(THIS_DIR)
    sub_path, grand_path = _output_paths()
    ntu.copy_fixture_to_output(fixture_sub, fixture_grand, sub_path, grand_path)

    jtu.open_other_documents_in_dir(os.path.dirname(grand_path), skip_path=grand_path)
    grand_doc = App.openDocument(grand_path)
    grand_asm = grand_doc.getObject("Assembly")
    boxC = jtu.get_by_label(grand_doc, "BoxC")
    sublink = jtu.get_by_label(grand_doc, "SubLink")

    # Die Fixture enthaelt BoxB (Spiegel) absichtlich an einer falschen Position (siehe
    # build_fixture_test_fixed_nested_flex.py) - simuliert einen "Treiber"-Versuch/Drag, der
    # die Joint-Bedingung noch nicht erfuellt (analog zu test_fixed_joint_flat.py). Erst hier,
    # im eigentlichen Testlauf unter der jeweiligen FreeCAD-Version, wird tatsaechlich geloest.
    grand_doc.recompute()
    grand_asm.solve(False)
    grand_doc.recompute()

    mirror_boxB = ntu.get_mirror(sublink, "BoxB")
    mirror_boxA = ntu.get_mirror(sublink, "BoxA")
    assert mirror_boxA is not None, "Spiegel von BoxA nicht in SubLink.Group gefunden"
    assert mirror_boxB is not None, "Spiegel von BoxB nicht in SubLink.Group gefunden"

    return grand_doc, grand_asm, boxC, sublink, mirror_boxA, mirror_boxB, sub_path, grand_path


def check(label, boxC_plc, mirror_boxA_plc, mirror_boxB_plc, tol_pos=1e-6, tol_rot=1e-4):
    boxB_expected = boxC_plc.multiply(OUTER_PLC1).multiply(OUTER_PLC2.inverse())
    boxA_expected = boxB_expected.multiply(INNER_PLC2).multiply(INNER_PLC1.inverse())

    def diff(expected, actual):
        pos_diff = (expected.Base - actual.Base).Length
        rot_diff_deg = abs(
            expected.Rotation.multiply(actual.Rotation.inverted()).Angle
        ) * 180.0 / 3.141592653589793
        return pos_diff, rot_diff_deg

    posB, rotB = diff(boxB_expected, mirror_boxB_plc)
    posA, rotA = diff(boxA_expected, mirror_boxA_plc)

    print(f"--- {label} ---")
    print("BoxB (analytisch erwartet):", boxB_expected)
    print("BoxB (tatsaechlich):       ", mirror_boxB_plc)
    print(f"BoxB Positions-/Rotations-Differenz: {posB:.9f} mm / {rotB:.9f} deg")
    print("BoxA (analytisch erwartet):", boxA_expected)
    print("BoxA (tatsaechlich):       ", mirror_boxA_plc)
    print(f"BoxA Positions-/Rotations-Differenz: {posA:.9f} mm / {rotA:.9f} deg")

    return posB <= tol_pos and rotB <= tol_rot and posA <= tol_pos and rotA <= tol_rot


def main():
    (
        grand_doc,
        grand_asm,
        boxC,
        sublink,
        mirror_boxA,
        mirror_boxB,
        sub_path,
        grand_path,
    ) = load_fixture_and_solve()

    # Sanity-Check: der Solver muss BoxB tatsaechlich von der in der Fixture hinterlegten
    # falschen Startposition wegbewegt haben (siehe die anderen Testfaelle fuer dasselbe Muster).
    moved = (mirror_boxB.Placement.Base - App.Vector(999, -999, 999)).Length > 1.0
    if not moved:
        print("RESULT: FAIL (BoxB wurde vom Solver nicht bewegt - Joint wirkungslos?)")
        return 1

    ok_initial = check("Direkt nach dem Solve", boxC.Placement, mirror_boxA.Placement, mirror_boxB.Placement)

    # Zusaetzliche, verschachtelungsspezifische Pruefung: Sub's EIGENES Dokumentobjekt muss mit
    # dem Spiegel in GrandTop synchron sein (`syncLocalMirrorPlacement()`, docs/ARCHITECTURE.md
    # Abschnitt 3, Bug 8). Stand 2026-09-08 (nach Fixture-Methodik-Fix): patched UND vanilla
    # bestehen diese Pruefung beide - kein nachweisbarer Unterschied fuer diesen einfachsten
    # Verschachtelungsfall (siehe Moduldocstring/README.md fuer die Chronologie eines frueheren,
    # sich als Testartefakt herausstellenden Befunds).
    sub_doc = App.getDocument(mirror_boxA.LinkedObject.Document.Name)
    subA = mirror_boxA.LinkedObject
    subB = mirror_boxB.LinkedObject
    sync_diff_A = (subA.Placement.Base - mirror_boxA.Placement.Base).Length
    sync_diff_B = (subB.Placement.Base - mirror_boxB.Placement.Base).Length
    print(f"\nSub<->Spiegel-Sync-Differenz BoxA: {sync_diff_A:.9f} mm")
    print(f"Sub<->Spiegel-Sync-Differenz BoxB: {sync_diff_B:.9f} mm")
    ok_sync = sync_diff_A <= 1e-6 and sync_diff_B <= 1e-6

    # Namen VOR dem Schliessen cachen - nach App.closeDocument() sind die alten Python-Objekte
    # (mirror_boxA/mirror_boxB) tote Referenzen (ReferenceError: "Cannot access attribute 'Name'
    # of deleted object"), live so gefunden.
    mirror_boxA_name = mirror_boxA.Label
    mirror_boxB_name = mirror_boxB.Label

    grand_doc.save()
    print("\nGrandTop gespeichert:", grand_path)
    print("Sub gespeichert:", sub_path)

    grand_doc2, boxC2, sublink2, reload_exc = ntu.save_close_reopen_recompute_nested(
        grand_doc, grand_path
    )
    if reload_exc is not None:
        print(f"RESULT: FAIL (Exception beim Neuladen+Recompute: {reload_exc!r})")
        return 1

    mirror_boxA2 = ntu.get_mirror(sublink2, mirror_boxA_name)
    mirror_boxB2 = ntu.get_mirror(sublink2, mirror_boxB_name)
    if mirror_boxA2 is None or mirror_boxB2 is None:
        print("RESULT: FAIL (Spiegel-Objekte nach Neuladen nicht mehr auffindbar)")
        return 1

    # ZWEITE Sync-Pruefung, NACH dem Neuladen - das genaue Szenario, in dem der Sync-Bug
    # urspruenglich gefunden wurde (Reload + NUR recompute(), OHNE erneuten expliziten
    # solve()-Aufruf). Die erste Sync-Pruefung oben laeuft NACH einem expliziten
    # assembly.solve(False) (noetig, um die absichtlich falsche Fixture-Startposition zu
    # korrigieren) - moeglich, dass dieser explizite solve()-Aufruf selbst schon fuer Sync
    # sorgt, unabhaengig vom Patch. Diese zweite Pruefung, rein ueber recompute() nach dem
    # Neuladen, ist die schaerfere Kontrolle.
    sub_doc2 = App.getDocument(mirror_boxA2.LinkedObject.Document.Name)
    subA2 = mirror_boxA2.LinkedObject
    subB2 = mirror_boxB2.LinkedObject
    sync_diff_A2 = (subA2.Placement.Base - mirror_boxA2.Placement.Base).Length
    sync_diff_B2 = (subB2.Placement.Base - mirror_boxB2.Placement.Base).Length
    print(f"\nSub<->Spiegel-Sync-Differenz BoxA (nach Neuladen, nur recompute()): {sync_diff_A2:.9f} mm")
    print(f"Sub<->Spiegel-Sync-Differenz BoxB (nach Neuladen, nur recompute()): {sync_diff_B2:.9f} mm")
    ok_sync_reload = sync_diff_A2 <= 1e-6 and sync_diff_B2 <= 1e-6

    ok_reload = check(
        "Nach Speichern+Schliessen+Neuladen+Recompute",
        boxC2.Placement,
        mirror_boxA2.Placement,
        mirror_boxB2.Placement,
    )

    if ok_initial and ok_sync and ok_sync_reload and ok_reload:
        print("RESULT: PASS")
        return 0
    else:
        reasons = []
        if not ok_initial:
            reasons.append("Placement-Kette direkt nach dem Solve weicht ab")
        if not ok_sync:
            reasons.append("Sub-Dokument und Spiegel in GrandTop nicht synchron (vor Neuladen)")
        if not ok_sync_reload:
            reasons.append("Sub-Dokument und Spiegel in GrandTop nicht synchron (nach Neuladen)")
        if not ok_reload:
            reasons.append("Placement-Kette nach Neuladen weicht ab")
        print(f"RESULT: FAIL ({'; '.join(reasons)})")
        return 1


if __name__ == "__main__":
    sys.exit(main())
