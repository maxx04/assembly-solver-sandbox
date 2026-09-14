"""
Stufe 2, Testfall 3: EINE Ebene Verschachtelung ueber eine FLEXIBLE Assembly::AssemblyLink,
mit Revolute als INNEREM und Fixed als AEUSSEREM Joint (aeusserer Joint ist per Konvention immer
Fixed - definiert die eindeutige Verankerung, siehe test_fixed_nested_flex.py).

Struktur (siehe nested_test_utils.py fuer die volle Herleitung/Nomenklatur):
    Sub (eigenes Dokument):     BoxA (GEERDET, interner Referenzpunkt) -[innerer Revolute-Joint]- BoxB
    GrandTop (eigenes Dokument): BoxC (geerdet) -[aeusserer Fixed-Joint]- SubLink.BoxB (Spiegel)

Der innere Revolute-Joint hat 1 freien Freiheitsgrad (Drehung um die Kantenachse) - wie
beim flachen Revolute-Test wird dafuer NICHT die exakte Placement-Kette geprueft (waere keine
solver-unabhaengige analytische Referenz mehr, siehe joint_test_utils.py), sondern die
Zwangsbedingung selbst (`jtu.check_revolute()`). Der aeussere Fixed-Joint hat 0 Freiheitsgrade -
dort WIRD die exakte Placement-Kette geprueft, wie bei test_fixed_nested_flex.py.

**Bekannter, bewusst zurueckgestellter Rest-Befund (Nutzerauftrag 2026-09-09, siehe Projekt-
Memory "todo-nested-fixed-mirror-placement-quirk"):** dieselbe 180-Grad-Solver-Mehrdeutigkeit,
die den flachen Fixed/Revolute/Cylindrical/Slider-Tests urspruenglich zu schaffen machte,
zeigt sich in einem GEKOPPELTEN (innerer+aeusserer Joint gemeinsam geloest) System selbst dann
noch, wenn der Startwert exakt auf den korrekten, gemeinsamen Zielwert vorbelegt wird - siehe
test_fixed_nested_flex.py fuer die volle Analyse. Der aeussere Joint (BoxB) laesst sich
zuverlaessig reparieren (per subB.Placement-Vorbelegung, NICHT mirror_boxB.Placement - siehe
build_fixture.py), der innere bleibt ein bekanntes Risiko - auf Nutzerwunsch NICHT weiter
verfolgt, dieser Testfall dient trotzdem als Vorlage fuer die uebrigen Jointtypen.

Aufruf (mit der jeweiligen FreeCAD-Binary via run-test.sh):
    ./common/run-test.sh <freecad-install-dir> test_revolute_nested_flex/test_revolute_nested_flex.py
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

TEST_NAME = "test_revolute_nested_flex"

# Echte, aus der Edge9-Geometrie abgeleitete JCS-Placements fuer den INNEREN Revolute-Joint
# (Nutzerkorrektur 2026-09-09: "fuer eine Schiebeachse nimmt man eine Achse/Kante, keine
# Flaeche" - siehe test_slider_joint_flat.py). Der AEUSSERE Joint bleibt Face-basiert (Fixed,
# Flaeche-auf-Flaeche ist dort der reale Anwendungsfall, siehe test_fixed_nested_flex.py).
INNER_PLC1 = jtu.edge_jcs_placement("Edge9", flip=False, extra_z_rotation_deg=20)  # BoxA
INNER_PLC2 = jtu.edge_jcs_placement("Edge9", flip=True, extra_z_rotation_deg=10)  # BoxB
OUTER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=30)  # BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face2", flip=True, extra_z_rotation_deg=15)  # Spiegel von BoxB


def _output_paths():
    fixture_sub, fixture_grand = ntu.fixture_paths(THIS_DIR)
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    if base:
        install_dir = os.path.splitext(base)[0]
    else:
        install_dir = os.path.join(THIS_DIR, "fcstd-output", "default")
    os.makedirs(install_dir, exist_ok=True)
    return (
        os.path.join(install_dir, os.path.basename(fixture_sub)),
        os.path.join(install_dir, os.path.basename(fixture_grand)),
    )


def load_fixture_and_solve():
    fixture_sub, fixture_grand = ntu.fixture_paths(THIS_DIR)
    sub_path, grand_path = _output_paths()
    ntu.copy_fixture_to_output(fixture_sub, fixture_grand, sub_path, grand_path)

    grand_doc = App.openDocument(grand_path)
    grand_asm = grand_doc.getObject("Assembly")
    boxC = jtu.get_by_label(grand_doc, "BoxC")
    sublink = jtu.get_by_label(grand_doc, "SubLink")

    grand_doc.recompute()
    grand_asm.solve(False)
    grand_doc.recompute()

    mirror_boxB = ntu.get_mirror(sublink, "BoxB")
    mirror_boxA = ntu.get_mirror(sublink, "BoxA")
    assert mirror_boxA is not None, "Spiegel von BoxA nicht in SubLink.Group gefunden"
    assert mirror_boxB is not None, "Spiegel von BoxB nicht in SubLink.Group gefunden"

    return grand_doc, grand_asm, boxC, sublink, mirror_boxA, mirror_boxB, sub_path, grand_path


def _as_obj(plc):
    """jtu.jcs_world() erwartet ein Objekt mit .Placement, nicht ein rohes Placement - kleiner
    Adapter, damit check() mit den bereits ausgelesenen .Placement-Werten arbeiten kann."""
    class _Wrapper:
        pass
    w = _Wrapper()
    w.Placement = plc
    return w


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

    moved = (mirror_boxB.Placement.Base - App.Vector(999, -999, 999)).Length > 1.0
    if not moved:
        print("RESULT: FAIL (BoxB wurde vom Solver nicht bewegt - Joint wirkungslos?)")
        return 1

    def full_check(label, boxC_plc, mirror_boxA_plc, mirror_boxB_plc):
        boxB_expected = boxC_plc.multiply(OUTER_PLC1).multiply(OUTER_PLC2.inverse())
        pos_diff = (boxB_expected.Base - mirror_boxB_plc.Base).Length
        rot_diff_deg = abs(
            boxB_expected.Rotation.multiply(mirror_boxB_plc.Rotation.inverted()).Angle
        ) * 180.0 / 3.141592653589793

        jcs1 = jtu.jcs_world(_as_obj(mirror_boxA_plc), INNER_PLC1)
        jcs2 = jtu.jcs_world(_as_obj(mirror_boxB_plc), INNER_PLC2)
        rel = jtu.report(f"{label} (innerer Revolute)", jcs1, jcs2)
        ok_inner = jtu.check_revolute(rel)

        print(f"--- {label} (aeusserer Fixed) ---")
        print("BoxB (analytisch erwartet):", boxB_expected)
        print("BoxB (tatsaechlich):       ", mirror_boxB_plc)
        print(f"BoxB Positions-/Rotations-Differenz: {pos_diff:.9f} mm / {rot_diff_deg:.9f} deg")

        ok_outer = pos_diff <= 1e-6 and rot_diff_deg <= 1e-4
        return ok_outer, ok_inner

    ok_outer, ok_inner = full_check(
        "Direkt nach dem Solve", boxC.Placement, mirror_boxA.Placement, mirror_boxB.Placement
    )

    sub_doc = App.getDocument(mirror_boxA.LinkedObject.Document.Name)
    subA = mirror_boxA.LinkedObject
    subB = mirror_boxB.LinkedObject
    sync_diff_A = (subA.Placement.Base - mirror_boxA.Placement.Base).Length
    sync_diff_B = (subB.Placement.Base - mirror_boxB.Placement.Base).Length
    print(f"\nSub<->Spiegel-Sync-Differenz BoxA: {sync_diff_A:.9f} mm")
    print(f"Sub<->Spiegel-Sync-Differenz BoxB: {sync_diff_B:.9f} mm")
    ok_sync = sync_diff_A <= 1e-6 and sync_diff_B <= 1e-6

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

    sub_doc2 = App.getDocument(mirror_boxA2.LinkedObject.Document.Name)
    subA2 = mirror_boxA2.LinkedObject
    subB2 = mirror_boxB2.LinkedObject
    sync_diff_A2 = (subA2.Placement.Base - mirror_boxA2.Placement.Base).Length
    sync_diff_B2 = (subB2.Placement.Base - mirror_boxB2.Placement.Base).Length
    print(f"\nSub<->Spiegel-Sync-Differenz BoxA (nach Neuladen, nur recompute()): {sync_diff_A2:.9f} mm")
    print(f"Sub<->Spiegel-Sync-Differenz BoxB (nach Neuladen, nur recompute()): {sync_diff_B2:.9f} mm")
    ok_sync_reload = sync_diff_A2 <= 1e-6 and sync_diff_B2 <= 1e-6

    ok_outer_reload, ok_inner_reload = full_check(
        "Nach Speichern+Schliessen+Neuladen+Recompute",
        boxC2.Placement, mirror_boxA2.Placement, mirror_boxB2.Placement,
    )

    if ok_outer and ok_inner and ok_sync and ok_sync_reload and ok_outer_reload and ok_inner_reload:
        print("RESULT: PASS")
        return 0
    else:
        reasons = []
        if not ok_outer:
            reasons.append("aeusserer Joint direkt nach dem Solve falsch")
        if not ok_inner:
            reasons.append("innerer Revolute-Joint direkt nach dem Solve verletzt")
        if not ok_sync:
            reasons.append("Sub-Dokument und Spiegel in GrandTop nicht synchron (vor Neuladen)")
        if not ok_sync_reload:
            reasons.append("Sub-Dokument und Spiegel in GrandTop nicht synchron (nach Neuladen)")
        if not ok_outer_reload:
            reasons.append("aeusserer Joint nach Neuladen falsch")
        if not ok_inner_reload:
            reasons.append("innerer Revolute-Joint nach Neuladen verletzt")
        print(f"RESULT: FAIL ({'; '.join(reasons)})")
        return 1


if __name__ == "__main__":
    sys.exit(main())
