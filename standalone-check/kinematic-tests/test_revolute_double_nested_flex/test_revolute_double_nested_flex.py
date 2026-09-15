"""
Stufe 3, Testfall 2: ZWEI Ebenen Verschachtelung (Sub -> Mid -> GrandTop), mit Revolute
als INNERSTEM Joint und Fixed als MITTLEREM+AEUSSERSTEM Joint (Konvention: mittlerer+aeusserer
Joint sind immer Fixed - definieren die eindeutige Verankerung, siehe
test_fixed_double_nested_flex.py).

Direkte Erweiterung von test_revolute_nested_flex.py (Stufe 2) um eine dritte Ebene - siehe
dessen Moduldocstring UND test_fixed_double_nested_flex.py's Moduldocstring fuer die vollen
Grundlagen (Namenskollisions-Fix, Seeding-Hebel, 180-Grad-Befund). Hier nur die
testfallspezifischen Werte.

Struktur:
    Sub (eigenes Dokument):  BoxA (GEERDET)          -[innerster Revolute-Joint]-  BoxB
    Mid (eigenes Dokument):  BoxD (geerdet) + SubLink -[mittlerer Fixed-Joint]-         SubLink.BoxB (Spiegel)
    GrandTop (eigenes Dok.): BoxC (geerdet) + MidLink -[aeusserster Fixed-Joint]-       MidLink.SubLink.BoxB (Spiegel des Spiegels)

Der innerste Revolute-Joint hat einen freien Freiheitsgrad - dafuer wird (wie beim flachen
und beim Stufe-2-Revolute-Test) NICHT die exakte Placement-Kette geprueft, sondern die
Zwangsbedingung selbst (`jtu.check_revolute()`). Mittlerer+aeusserer Joint haben 0 Freiheitsgrade -
dort WIRD die exakte Placement-Kette geprueft, wie bei test_fixed_double_nested_flex.py.

**Erwarteter Befund (siehe test_fixed_double_nested_flex.py fuer die volle Herleitung):**
Seedet man subB.Placement nahe am AEUSSERSTEN Ziel (der einzige echte Freiheitsgrad im
gesamten System - BoxD und BoxC sind beide geerdet), sollten mittlerer+aeusserer Joint exakt
korrekt konvergieren; der innerste Revolute-Joint kann denselben bekannten 180-Grad-Rest
zeigen wie bei Stufe 2 (auszer Ball, der keine Orientierungs-Zwangsbedingung hat). Auf
Nutzerauftrag ("180-Grad-Problem ignorieren") nicht weiter root-cause-analysiert.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_revolute_double_nested_flex/test_revolute_double_nested_flex.py
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

TEST_NAME = "test_revolute_double_nested_flex"
FIXTURES_DIR = os.path.join(THIS_DIR, "fixtures")

# INNER_PLC* identisch zu test_revolute_nested_flex.py (Stufe 2); MIDDLE_PLC*/OUTER_PLC*
# identisch zu test_fixed_double_nested_flex.py's MIDDLE_PLC*/OUTER_PLC* (mittlerer+aeusserer
# Joint sind bei JEDEM Stufe-3-Testfall gleich, nur der innerste Jointtyp variiert).
INNER_PLC1 = jtu.edge_jcs_placement("Edge9", flip=False, extra_z_rotation_deg=20)  # BoxA
INNER_PLC2 = jtu.edge_jcs_placement("Edge9", flip=True, extra_z_rotation_deg=10)  # BoxB
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
    fixture_sub, fixture_mid, fixture_grand = _fixture_paths()
    out_sub, out_mid, out_grand = _output_paths()
    for src, dst in ((fixture_sub, out_sub), (fixture_mid, out_mid), (fixture_grand, out_grand)):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
    # FCPROJECT-PATCH (Nutzerauftrag 2026-09-15): externe Koerper-Dateien (BoxA/BoxB/BoxC/BoxD,
    # siehe joint_test_utils.py::ensure_box_bodies_in()) liegen in FIXTURES_DIR neben sub/mid/
    # grand - muessen bei jedem Kopiervorgang mit umziehen, sonst bricht der relative XLink.
    for name in os.listdir(FIXTURES_DIR):
        if name.endswith(".FCStd") and name not in (
            os.path.basename(fixture_sub), os.path.basename(fixture_mid), os.path.basename(fixture_grand)
        ):
            shutil.copy2(os.path.join(FIXTURES_DIR, name), os.path.join(os.path.dirname(out_grand), name))

    # Siehe joint_test_utils.py::open_other_documents_in_dir() - noetig, damit FreeCAD alle
    # transitiven XLink-Ziele (Koerper- UND Sub-/Mid-Dokumente) bereits aufgeloest hat, BEVOR der
    # allererste automatische Solve waehrend restore() laeuft.
    jtu.open_other_documents_in_dir(os.path.dirname(out_grand), skip_path=out_grand)
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


def _as_obj(plc):
    """jtu.jcs_world() erwartet ein Objekt mit .Placement, nicht ein rohes Placement - kleiner
    Adapter (siehe test_revolute_nested_flex.py)."""
    class _Wrapper:
        pass
    w = _Wrapper()
    w.Placement = plc
    return w


def full_check(label, boxC_plc, boxD_mirror_plc, mirror_boxA_plc, mirror_boxB_plc):
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

    jcs1 = jtu.jcs_world(_as_obj(mirror_boxA_plc), INNER_PLC1)
    jcs2 = jtu.jcs_world(_as_obj(mirror_boxB_plc), INNER_PLC2)
    rel = jtu.report(f"{label} (innerster Revolute)", jcs1, jcs2)
    ok_inner = jtu.check_revolute(rel)

    print(f"--- {label} ---")
    print(f"AEUSSERER Joint (BoxC-BoxB, doppelt gespiegelt):  pos={posO:.9f} mm  rot={rotO:.9f} deg")
    print(f"MITTLERER Joint (BoxD-BoxB, einfach gespiegelt):  pos={posM:.9f} mm  rot={rotM:.9f} deg")

    ok_outer = posO <= 1e-6 and rotO <= 1e-4
    ok_middle = posM <= 1e-6 and rotM <= 1e-4
    return ok_outer, ok_middle, ok_inner


def main():
    (
        grand_doc, grand_asm, boxC, midlink, boxD_mirror, sublink_mirror,
        mirror_boxA, mirror_boxB, out_sub, out_mid, out_grand,
    ) = load_fixture_and_solve()

    moved = (mirror_boxB.Placement.Base - App.Vector(999, -999, 999)).Length > 1.0
    if not moved:
        print("RESULT: FAIL (BoxB wurde vom Solver nicht bewegt - Joint(s) wirkungslos?)")
        return 1

    ok_outer, ok_middle, ok_inner = full_check(
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

    ok_outer2, ok_middle2, ok_inner2 = full_check(
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
