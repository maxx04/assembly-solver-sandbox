"""
Stufe 3, Testfall 1: ZWEI Ebenen Verschachtelung ueber flexible Assembly::AssemblyLinks
(Sub -> Mid -> GrandTop), mit je einem Fixed-Joint pro Ebene (3 Joints insgesamt).

Direkte Erweiterung von test_fixed_nested_flex.py (Stufe 2) um eine dritte Ebene - siehe
dortiges Moduldocstring/nested_test_utils.py fuer die Grundlagen (Fixture-Methodik,
Nomenklatur, Sub-Erdung als normale Konstrukteurspraxis). Hier nur das NEUE dokumentiert.

Struktur:
    Sub (eigenes Dokument):  BoxA (GEERDET)              -[innerster Fixed-Joint]-  BoxB
    Mid (eigenes Dokument):  BoxD (geerdet) + SubLink     -[mittlerer Fixed-Joint]-  SubLink.BoxB (Spiegel)
    GrandTop (eigenes Dok.): BoxC (geerdet) + MidLink     -[aeusserster Fixed-Joint]- MidLink.SubLink.BoxB (Spiegel des Spiegels)

**Namenskollision vermieden (Nutzerauftrag 2026-09-09, live gefunden):** wuerde Mid seine
eigene Ground-Box UND seinen eigenen Sub-Link genauso nennen wie GrandTop seine eigenen
("BoxC"/"SubLink", der Default von nested_test_utils.py::new_grand_assembly_with_sublink()),
wuerden die gespiegelten Kopien beim Einbetten in GrandTop STILL auf "BoxC001"/"SubLink001"
umbenannt (FreeCADs automatische Namenskollisions-Aufloesung) - Namens-basiertes get_mirror()
auf der aeusseren Ebene wuerde dadurch unbrauchbar. Deshalb bekommt Mid bewusst ANDERE Namen:
box_name="BoxD", GrandTop bekommt link_name="MidLink" (behaelt "BoxC" fuer die eigene
Ground-Box, da das mit "BoxD" nicht kollidiert). Damit bleiben ALLE Namen auf jeder Ebene
eindeutig und per get_mirror() direkt (ohne TypeId-Ausweichloesung) auffindbar.

**Seeding-Hebel, live verifiziert (2026-09-09) - direkte Erweiterung von
[[todo-nested-fixed-mirror-placement-quirk]]:** GENAU EIN echter Freiheitsgrad existiert im
gesamten 3-Ebenen-System - subB.Placement (das reale, kanonische Objekt in Subs eigenem
Dokument). BoxD und BoxC sind beide geerdet/ReadOnly, bewegen sich nie. Seedet man subB nahe
am AEUSSERSTEN (GrandTop-)Ziel (exakt dieselbe Methodik wie bei Stufe 2 - NICHT am innersten
oder mittleren Ziel), konvergieren AEUSSERER und MITTLERER Joint beide exakt korrekt
(0.000000 mm/deg, per Sondierungsskript bestaetigt) - nur der INNERSTE Joint zeigt weiterhin
den bekannten 180-Grad-Rest (siehe [[reference-fixed-joint-180-degree-bug-rootcause]]), exakt
wie bei Stufe 2 bereits akzeptiert und dokumentiert. Auf Nutzerauftrag ("180-Grad-Problem
ignorieren") NICHT weiter root-cause-analysiert.

Analytisch erwartet (reine Placement-Algebra):
    BoxB_erwartet (aeusserster Bezug) = BoxC.Placement * OUTER_PLC1  * OUTER_PLC2.inverse()
    BoxB_erwartet (mittlerer Bezug)   = BoxD.Placement * MIDDLE_PLC1 * MIDDLE_PLC2.inverse()
    BoxA_erwartet (innerster Bezug)   = BoxB_erwartet   * INNER_PLC2  * INNER_PLC1.inverse()
(INNER_PLC1/2 identisch zu test_fixed_nested_flex.py; MIDDLE_PLC1/2 identisch zu dessen
OUTER_PLC1/2 - diese dritte Ebene ist wortwoertlich "Stufe 2 plus eine weitere Ebene aussen".)

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_fixed_double_nested_flex/test_fixed_double_nested_flex.py
"""
import os
import sys
import shutil

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu

TEST_NAME = "test_fixed_double_nested_flex"
FIXTURES_DIR = os.path.join(THIS_DIR, "fixtures")

# Siehe Moduldocstring: INNER_PLC* identisch zu test_fixed_nested_flex.py, MIDDLE_PLC* identisch
# zu dessen OUTER_PLC* - diese Ebene erweitert Stufe 2 nur um eine weitere aeussere Schicht.
INNER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # BoxA
INNER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # BoxB
MIDDLE_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=30)  # BoxD
MIDDLE_PLC2 = jtu.face_jcs_placement("Face2", flip=True, extra_z_rotation_deg=15)  # Spiegel von BoxB (in Mid)
OUTER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=40)  # BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face2", flip=True, extra_z_rotation_deg=25)  # Spiegel des Spiegels von BoxB (in GrandTop)


def _fixture_paths():
    return (
        os.path.join(FIXTURES_DIR, f"{TEST_NAME}_sub.FCStd"),
        os.path.join(FIXTURES_DIR, f"{TEST_NAME}_mid.FCStd"),
        os.path.join(FIXTURES_DIR, f"{TEST_NAME}_grand.FCStd"),
    )


def _output_paths():
    fixture_sub, fixture_mid, fixture_grand = _fixture_paths()
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    if base:
        install_dir = os.path.splitext(base)[0]
    else:
        install_dir = os.path.join(THIS_DIR, "fcstd-output", "default")
    os.makedirs(install_dir, exist_ok=True)
    return (
        os.path.join(install_dir, os.path.basename(fixture_sub)),
        os.path.join(install_dir, os.path.basename(fixture_mid)),
        os.path.join(install_dir, os.path.basename(fixture_grand)),
    )


def load_fixture_and_solve():
    """Kopiert die feste 3-Dokumente-Fixture in den eigenen Ausgabepfad, oeffnet nur GrandTop
    (Mid+Sub haengen als externe Referenzen daran und werden automatisch mitgeladen) und loest
    sie."""
    fixture_sub, fixture_mid, fixture_grand = _fixture_paths()
    out_sub, out_mid, out_grand = _output_paths()
    for src, dst in ((fixture_sub, out_sub), (fixture_mid, out_mid), (fixture_grand, out_grand)):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)

    grand_doc = App.openDocument(out_grand)
    grand_asm = grand_doc.getObject("Assembly")
    boxC = jtu.get_by_label(grand_doc, "BoxC")
    midlink = jtu.get_by_label(grand_doc, "MidLink")

    grand_doc.recompute()
    grand_asm.solve(False)
    grand_doc.recompute()

    boxD_mirror = ntu.get_mirror(midlink, "BoxD")
    sublink_mirror = ntu.get_mirror(midlink, "SubLink")
    assert boxD_mirror is not None, "Spiegel von BoxD nicht in MidLink.Group gefunden"
    assert sublink_mirror is not None, "Spiegel von SubLink nicht in MidLink.Group gefunden"
    mirror_boxB = ntu.get_mirror(sublink_mirror, "BoxB")
    mirror_boxA = ntu.get_mirror(sublink_mirror, "BoxA")
    assert mirror_boxB is not None, "Doppelt gespiegeltes BoxB nicht gefunden"
    assert mirror_boxA is not None, "Doppelt gespiegeltes BoxA nicht gefunden"

    return (
        grand_doc, grand_asm, boxC, midlink, boxD_mirror, sublink_mirror,
        mirror_boxA, mirror_boxB, out_sub, out_mid, out_grand,
    )


def check(label, boxC_plc, boxD_mirror_plc, mirror_boxA_plc, mirror_boxB_plc, tol_pos=1e-6, tol_rot=1e-4):
    """Prueft alle 3 Joints unabhaengig gegen ihre jeweils eigene analytische Erwartung (siehe
    Moduldocstring). Der innerste Joint ist bekannt fehlschlagend (180-Grad-Rest, siehe
    Moduldocstring) - wird trotzdem berechnet/ausgegeben (Transparenz), aber NICHT ins
    Gesamt-PASS/FAIL eingerechnet (dasselbe Muster wie test_*_nested_flex.py's inneren Joint,
    ausser Ball)."""

    def diff(expected, actual):
        pos_diff = (expected.Base - actual.Base).Length
        rot_diff_deg = abs(
            expected.Rotation.multiply(actual.Rotation.inverted()).Angle
        ) * 180.0 / 3.141592653589793
        return pos_diff, rot_diff_deg

    outer_expected = boxC_plc.multiply(OUTER_PLC1).multiply(OUTER_PLC2.inverse())
    posO, rotO = diff(outer_expected, mirror_boxB_plc)

    middle_expected = boxD_mirror_plc.multiply(MIDDLE_PLC1).multiply(MIDDLE_PLC2.inverse())
    posM, rotM = diff(middle_expected, mirror_boxB_plc)

    inner_expected = mirror_boxB_plc.multiply(INNER_PLC2).multiply(INNER_PLC1.inverse())
    posI, rotI = diff(inner_expected, mirror_boxA_plc)

    print(f"--- {label} ---")
    print(f"AEUSSERER Joint (BoxC-BoxB, doppelt gespiegelt):  pos={posO:.9f} mm  rot={rotO:.9f} deg")
    print(f"MITTLERER Joint (BoxD-BoxB, einfach gespiegelt):  pos={posM:.9f} mm  rot={rotM:.9f} deg")
    print(f"INNERSTER Joint (BoxA-BoxB, bekannt 180-Grad-Rest): pos={posI:.9f} mm  rot={rotI:.9f} deg")

    ok_outer = posO <= tol_pos and rotO <= tol_rot
    ok_middle = posM <= tol_pos and rotM <= tol_rot
    return ok_outer, ok_middle


def main():
    (
        grand_doc, grand_asm, boxC, midlink, boxD_mirror, sublink_mirror,
        mirror_boxA, mirror_boxB, out_sub, out_mid, out_grand,
    ) = load_fixture_and_solve()

    moved = (mirror_boxB.Placement.Base - App.Vector(999, -999, 999)).Length > 1.0
    if not moved:
        print("RESULT: FAIL (BoxB wurde vom Solver nicht bewegt - Joint(s) wirkungslos?)")
        return 1

    ok_outer, ok_middle = check(
        "Direkt nach dem Solve", boxC.Placement, boxD_mirror.Placement, mirror_boxA.Placement, mirror_boxB.Placement
    )

    mirror_boxA_name = mirror_boxA.Label
    mirror_boxB_name = mirror_boxB.Label
    boxD_mirror_name = boxD_mirror.Label

    grand_doc.save()
    print("\nGespeichert:", out_sub, out_mid, out_grand)

    grand_doc2, boxC2, midlink2, reload_exc = ntu.save_close_reopen_recompute_nested(
        grand_doc, out_grand, link_name="MidLink"
    )
    if reload_exc is not None:
        print(f"RESULT: FAIL (Exception beim Neuladen+Recompute: {reload_exc!r})")
        return 1

    boxD_mirror2 = ntu.get_mirror(midlink2, boxD_mirror_name)
    sublink_mirror2 = ntu.get_mirror(midlink2, "SubLink")
    if boxD_mirror2 is None or sublink_mirror2 is None:
        print("RESULT: FAIL (Spiegel-Objekte nach Neuladen nicht mehr auffindbar - Mid-Ebene)")
        return 1
    mirror_boxA2 = ntu.get_mirror(sublink_mirror2, mirror_boxA_name)
    mirror_boxB2 = ntu.get_mirror(sublink_mirror2, mirror_boxB_name)
    if mirror_boxA2 is None or mirror_boxB2 is None:
        print("RESULT: FAIL (Spiegel-Objekte nach Neuladen nicht mehr auffindbar - Sub-Ebene)")
        return 1

    ok_outer2, ok_middle2 = check(
        "Nach Speichern+Schliessen+Neuladen+Recompute",
        boxC2.Placement, boxD_mirror2.Placement, mirror_boxA2.Placement, mirror_boxB2.Placement,
    )

    if ok_outer and ok_middle and ok_outer2 and ok_middle2:
        print("RESULT: PASS")
        return 0
    else:
        reasons = []
        if not ok_outer:
            reasons.append("aeusserer Joint direkt nach dem Solve falsch")
        if not ok_middle:
            reasons.append("mittlerer Joint direkt nach dem Solve falsch")
        if not ok_outer2:
            reasons.append("aeusserer Joint nach Neuladen falsch")
        if not ok_middle2:
            reasons.append("mittlerer Joint nach Neuladen falsch")
        print(f"RESULT: FAIL ({'; '.join(reasons)})")
        return 1


if __name__ == "__main__":
    sys.exit(main())
