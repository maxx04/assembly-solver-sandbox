"""
Regressionstest fuer den IdentityGraph-Umbau (Phase 1, siehe
/home/maxx/.claude/plans/enumerated-roaming-river.md): DREI Ebenen flexibler
Assembly::AssemblyLink-Verschachtelung (Sub -> Mid -> GrandTop, wie
test_fixed_double_nested_flex.py), aber die MITTLERE Ebene ("Mid") wird ZWEIMAL als
Geschwister-Instanz in GrandTop eingefuegt (wie test_fixed_duplicate_instance_flat.py, nur
eine Verschachtelungsebene tiefer) - bildet damit direkt den am 2026-09-19 live beobachteten
BG37->BG43->BG67-Bug nach: ein Joint MEHRERE Ebenen unterhalb einer duplizierten Instanz wurde
von resolveJointReference() (siehe deren selbst-dokumentierte Ein-Sprung-Grenze) nicht mehr
korrekt aufgeloest, und der aeussere/mittlere Joint referenziert dabei direkt einen bereits
verschachtelten lokalen Spiegel (die "sechste Baustelle" - siehe refineNestedMirrorTarget() in
AssemblyIdentityGraph.cpp), nicht nur ein Blatt ueber sauberes nestingPrefix.

Struktur (je EINE Instanz von Mid dargestellt, MidLink1/MidLink2 sind vollstaendig
gleichartige, physikalisch konsistente Duplikate - siehe test_fixed_duplicate_instance_flat.py
fuer die Begruendung "gleiche Placement-Werte, kein Konflikt"):

    Sub (eigenes Dokument):  SubA (GEERDET)             -[innerster Fixed-Joint]-  SubB
    Mid (eigenes Dokument):  BoxD (geerdet) + SubLink    -[mittlerer Fixed-Joint]-  SubLink.SubB (Spiegel)
    GrandTop (eigenes Dok.): BoxC (geerdet)
                             + MidLink1(Mid) -[aeusserer Joint 1]- MidLink1.SubLink.SubB (Spiegel des Spiegels, Instanz 1)
                             + MidLink2(Mid) -[aeusserer Joint 2]- MidLink2.SubLink.SubB (Spiegel des Spiegels, Instanz 2)

Nur EIN echter Freiheitsgrad im gesamten System (subB.Placement, das reale kanonische Objekt in
Subs eigenem Dokument) - BoxD und BoxC sind beide geerdet/ReadOnly. Seeding-Methodik identisch
zu test_fixed_double_nested_flex.py: subB nahe am AEUSSERSTEN Ziel vorbelegen (nicht am inneren
oder mittleren), der bekannte 180-Grad-Rest am innersten Joint bleibt bewusst akzeptiert (siehe
[[reference-fixed-joint-180-degree-bug-rootcause]]).

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_fixed_duplicate_instance_nested/test_fixed_duplicate_instance_nested.py
"""
import os
import sys
import shutil

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
FIXTURES_DIR = os.path.join(THIS_DIR, "fixtures")
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu

TEST_NAME = "test_fixed_duplicate_instance_nested"

# Siehe test_fixed_double_nested_flex.py fuer dieselbe Nomenklatur/Rotationswinkel-Konvention.
INNER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # SubA
INNER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # SubB
MIDDLE_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=30)  # BoxD
MIDDLE_PLC2 = jtu.face_jcs_placement("Face2", flip=True, extra_z_rotation_deg=15)  # Spiegel von SubB (in Mid)
OUTER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=40)  # BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face2", flip=True, extra_z_rotation_deg=25)  # Spiegel des Spiegels von SubB


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
    # Externe Koerper-Dateien (BoxA/BoxB/BoxC/BoxD, siehe joint_test_utils.py::
    # ensure_box_bodies_in()) liegen in FIXTURES_DIR neben sub/mid/grand - muessen bei jedem
    # Kopiervorgang mit umziehen, sonst bricht der relative XLink (siehe
    # test_fixed_double_nested_flex/test_fixed_double_nested_flex.py fuer dieselbe Lektion).
    for name in os.listdir(FIXTURES_DIR):
        if name.endswith(".FCStd") and name not in (
            os.path.basename(fixture_sub), os.path.basename(fixture_mid), os.path.basename(fixture_grand)
        ):
            shutil.copy2(os.path.join(FIXTURES_DIR, name), os.path.join(os.path.dirname(out_grand), name))

    jtu.open_other_documents_in_dir(os.path.dirname(out_grand), skip_path=out_grand)
    grand_doc = App.openDocument(out_grand)
    grand_asm = grand_doc.getObject("Assembly")
    boxC = jtu.get_by_label(grand_doc, "BoxC")
    midlink1 = jtu.get_by_label(grand_doc, "MidLink1")
    midlink2 = jtu.get_by_label(grand_doc, "MidLink2")

    grand_doc.recompute()
    grand_asm.solve(False)
    grand_doc.recompute()

    def mirrors_for(midlink):
        boxD_mirror = ntu.get_mirror(midlink, "BoxD")
        sublink_mirror = ntu.get_mirror(midlink, "SubLink")
        assert boxD_mirror is not None, f"Spiegel von BoxD nicht in {midlink.Label}.Group gefunden"
        assert sublink_mirror is not None, f"Spiegel von SubLink nicht in {midlink.Label}.Group gefunden"
        mirror_subB = ntu.get_mirror(sublink_mirror, "SubB")
        mirror_subA = ntu.get_mirror(sublink_mirror, "SubA")
        assert mirror_subB is not None, f"Doppelt gespiegeltes SubB nicht gefunden ({midlink.Label})"
        assert mirror_subA is not None, f"Doppelt gespiegeltes SubA nicht gefunden ({midlink.Label})"
        return boxD_mirror, sublink_mirror, mirror_subA, mirror_subB

    boxD_mirror1, sublink_mirror1, mirror_subA1, mirror_subB1 = mirrors_for(midlink1)
    boxD_mirror2, sublink_mirror2, mirror_subA2, mirror_subB2 = mirrors_for(midlink2)

    return (
        grand_doc, grand_asm, boxC,
        midlink1, boxD_mirror1, sublink_mirror1, mirror_subA1, mirror_subB1,
        midlink2, boxD_mirror2, sublink_mirror2, mirror_subA2, mirror_subB2,
        out_sub, out_mid, out_grand,
    )


def check(label, boxC_plc, boxD_mirror_plc, mirror_subA_plc, mirror_subB_plc, instance_label, tol_pos=1e-6, tol_rot=1e-4):
    """Prueft alle 3 Joints EINER Instanz unabhaengig gegen ihre jeweils eigene analytische
    Erwartung (identisch zu test_fixed_double_nested_flex.py::check(), nur je Instanz separat
    aufgerufen)."""
    outer_expected = boxC_plc.multiply(OUTER_PLC1).multiply(OUTER_PLC2.inverse())
    middle_expected = boxD_mirror_plc.multiply(MIDDLE_PLC1).multiply(MIDDLE_PLC2.inverse())
    inner_expected = middle_expected.multiply(INNER_PLC2).multiply(INNER_PLC1.inverse())

    def diff(expected, actual):
        pos = (expected.Base - actual.Base).Length
        rot = abs(expected.Rotation.multiply(actual.Rotation.inverted()).Angle) * 180.0 / 3.141592653589793
        return pos, rot

    posO, rotO = diff(outer_expected, mirror_subB_plc)
    posM, rotM = diff(middle_expected, mirror_subB_plc)
    posI, rotI = diff(inner_expected, mirror_subA_plc)

    print(f"--- {label} ({instance_label}) ---")
    print(f"AEUSSERER Joint (BoxC-SubB, doppelt gespiegelt):  pos={posO:.9f} mm  rot={rotO:.9f} deg")
    print(f"MITTLERER Joint (BoxD-SubB, einfach gespiegelt):  pos={posM:.9f} mm  rot={rotM:.9f} deg")
    print(f"INNERSTER Joint (SubA-SubB, bekannt 180-Grad-Rest): pos={posI:.9f} mm  rot={rotI:.9f} deg")

    ok_outer = posO <= tol_pos and rotO <= tol_rot
    ok_middle = posM <= tol_pos and rotM <= tol_rot
    return ok_outer, ok_middle


def main():
    (
        grand_doc, grand_asm, boxC,
        midlink1, boxD_mirror1, sublink_mirror1, mirror_subA1, mirror_subB1,
        midlink2, boxD_mirror2, sublink_mirror2, mirror_subA2, mirror_subB2,
        out_sub, out_mid, out_grand,
    ) = load_fixture_and_solve()

    ok_outer1, ok_middle1 = check(
        "Direkt nach dem Solve", boxC.Placement, boxD_mirror1.Placement,
        mirror_subA1.Placement, mirror_subB1.Placement, "Instanz 1"
    )
    ok_outer2, ok_middle2 = check(
        "Direkt nach dem Solve", boxC.Placement, boxD_mirror2.Placement,
        mirror_subA2.Placement, mirror_subB2.Placement, "Instanz 2"
    )

    print("Testdatei gespeichert:", out_grand)

    grand_doc.save()

    # Alle drei Dokumente schliessen+neu oeffnen (wie test_fixed_double_nested_flex.py) -
    # save_close_reopen_recompute_nested() ist fuer NUR 2 Dokumente ausgelegt, hier von Hand mit
    # der dritten Ebene ergaenzt.
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)

    jtu.open_other_documents_in_dir(os.path.dirname(out_grand), skip_path=out_grand)
    grand_doc2 = App.openDocument(out_grand)
    boxC2 = jtu.get_by_label(grand_doc2, "BoxC")
    midlink1_2 = jtu.get_by_label(grand_doc2, "MidLink1")
    midlink2_2 = jtu.get_by_label(grand_doc2, "MidLink2")

    reload_exception = None
    try:
        grand_doc2.recompute()
    except Exception as exc:  # noqa: BLE001 - bewusst breit, das IST der Test
        reload_exception = exc

    if reload_exception is not None:
        print(f"RESULT: FAIL (Exception beim Neuladen+Recompute: {reload_exception!r})")
        return 1

    def mirrors_for(midlink):
        sublink_mirror = ntu.get_mirror(midlink, "SubLink")
        boxD_mirror = ntu.get_mirror(midlink, "BoxD")
        mirror_subA = ntu.get_mirror(sublink_mirror, "SubA")
        mirror_subB = ntu.get_mirror(sublink_mirror, "SubB")
        return boxD_mirror, mirror_subA, mirror_subB

    boxD_mirror1_2, mirror_subA1_2, mirror_subB1_2 = mirrors_for(midlink1_2)
    boxD_mirror2_2, mirror_subA2_2, mirror_subB2_2 = mirrors_for(midlink2_2)

    ok_outer1_2, ok_middle1_2 = check(
        "Nach Speichern+Schliessen+Neuladen+Recompute", boxC2.Placement, boxD_mirror1_2.Placement,
        mirror_subA1_2.Placement, mirror_subB1_2.Placement, "Instanz 1"
    )
    ok_outer2_2, ok_middle2_2 = check(
        "Nach Speichern+Schliessen+Neuladen+Recompute", boxC2.Placement, boxD_mirror2_2.Placement,
        mirror_subA2_2.Placement, mirror_subB2_2.Placement, "Instanz 2"
    )

    failures = []
    if not ok_outer1:
        failures.append("aeusserer Joint Instanz 1 direkt nach dem Solve falsch")
    if not ok_middle1:
        failures.append("mittlerer Joint Instanz 1 direkt nach dem Solve falsch")
    if not ok_outer2:
        failures.append("aeusserer Joint Instanz 2 direkt nach dem Solve falsch")
    if not ok_middle2:
        failures.append("mittlerer Joint Instanz 2 direkt nach dem Solve falsch")
    if not ok_outer1_2:
        failures.append("aeusserer Joint Instanz 1 nach Neuladen falsch")
    if not ok_middle1_2:
        failures.append("mittlerer Joint Instanz 1 nach Neuladen falsch")
    if not ok_outer2_2:
        failures.append("aeusserer Joint Instanz 2 nach Neuladen falsch")
    if not ok_middle2_2:
        failures.append("mittlerer Joint Instanz 2 nach Neuladen falsch")

    if failures:
        print("RESULT: FAIL (" + "; ".join(failures) + ")")
        return 1

    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
