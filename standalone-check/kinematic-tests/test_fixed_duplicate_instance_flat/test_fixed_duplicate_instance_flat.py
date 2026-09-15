"""
Mehrfachinstanz-Testfall 1 ("flach"): dieselbe verlinkte Unterbaugruppe wird ZWEIMAL in dieselbe
GrandTop-Baugruppe eingefuegt - genau das Szenario, das der Nutzer beim realen Aufbau von BG22/
BG25 gefunden hat ("nach dem import kann Freecad nicht trennen 1 und 2 BG 25"). Deckt zwei
unabhaengige, in dieser Sandbox gefundene und gefixte Bugs ab (siehe docs/ARCHITECTURE.md
Abschnitt 4/5):

  Bug A: AssemblyObject::canonicalizeForMbD() loeste eine lokale Spiegel-Kopie bisher PER NAME auf
  ein Objekt im verlinkten Dokument auf - funktioniert nur, solange Spiegel- und Realname
  uebereinstimmen (immer der Fall bei genau EINER Instanz). Bei der ZWEITEN Instanz vergibt
  FreeCAD dem zweiten Spiegelsatz automatisch einen ANDEREN internen Namen (Kollisionsaufloesung,
  seit Nutzerauftrag 2026-09-14 bewusst NICHT mehr per Hand auf "BoxB"/"BoxB001" o.ae. gesetzt,
  siehe check_unique_names() unten) - dieser Name existiert im inneren, verlinkten Dokument nie,
  die Namenssuche schlug fehl, die Funktion fiel faelschlich auf "Spiegel bleibt Spiegel" zurueck
  statt das echte Objekt zu liefern. Gefixt ueber AssemblyLink::objLinkMap/getSourceForMirror()
  (identitaetsbasiert statt namensbasiert).

  Bug B: getJoints() liefert fuer beide Instanzen korrekt je einen eigenen JointRef-Eintrag fuer
  Subs INNEREN Joint (gleicher realer Joint-Zeiger, aber unterschiedliches nestingPrefix, je nach
  dem internen Namen der jeweiligen AssemblyLink-Instanz, z.B. "Assembly001."/"Assembly002.") -
  die vormalige jointNestingPrefixMap war aber nur nach Joint-ZEIGER geschluesselt und konnte
  deshalb nur EINEN der beiden nestingPrefix-Werte gleichzeitig halten. Der Solver bekam denselben
  realen Joint zweimal mit IDENTISCHEM ASMT-Namen - eine echte Namenskollision, nicht nur ein
  Aufloesungsfehler. Gefixt durch ersatzlose Entfernung der Map: das nestingPrefix reist seitdem
  explizit durch die gesamte Aufrufkette (resolvePartForMbD()/isMbDJointValid()/
  handleOneSideOfJoint()/getRackPinionMarkers()/makeMbdJoint(), sowie jointParts()/
  removeUnconnectedJoints()/traverseAndMarkConnectedParts()/getConnectedParts() ueber
  vector<JointRef> statt vector<DocumentObject*>).

Struktur (siehe nested_test_utils.py fuer die Basis-Nomenklatur; LABEL-Namen unten - der
INTERNE Name ist seit Nutzerauftrag 2026-09-14 IMMER FreeCAD selbst ueberlassen, siehe
check_unique_names()):
    Sub (eigenes Dokument):     BoxA (GEERDET) -[innerer Fixed-Joint]- BoxB
    GrandTop (eigenes Dokument): BoxC (geerdet)
        -[aeusserer Fixed-Joint 1]- SubLink.BoxB    (Spiegel Instanz 1, Label - interner Name z.B. "Assembly001"/"Box002")
        -[aeusserer Fixed-Joint 2]- SubLink.BoxB    (Spiegel Instanz 2, GLEICHES Label - interner Name z.B. "Assembly002"/"Box004")

Beide aeussere Joints bekommen ABSICHTLICH IDENTISCHE Placement1/2-Werte - ein physikalisch
konsistentes Duplikat (beide Instanzen sollen an derselben Stelle relativ zu BoxC sitzen), damit
es weiterhin GENAU EINE analytisch vorhersagbare Loesung gibt, statt eines widerspruechlichen
Systems. Das macht den Testfall zwar (bewusst) redundant fuer den Solver - genau das ist die
Absicht: der OndselSolver muss dabei einen der beiden Aussenjoints UND eine der beiden Kopien von
Subs Innenjoint als redundant erkennen und eliminieren, OHNE dass die (jetzt eindeutigen)
ASMT-Namen kollidieren (das war Bug Bs konkretes Symptom - siehe Log-Vergleich unten).

Aufruf:
    ./run-test.sh <freecad-install-dir> test_fixed_duplicate_instance_flat.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu

TEST_NAME = "test_fixed_duplicate_instance_flat"

INNER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # BoxA
INNER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # BoxB
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


def _find_mirror_by_source(sublink, real_obj):
    """Findet das Spiegel-Kind von 'sublink', dessen LinkedObject auf 'real_obj' zeigt - robuster
    als ntu.get_mirror() (namensbasiert), weil bei der zweiten Instanz der Spiegelname
    auto-suffigiert ist (siehe Moduldocstring, Bug A) und wir hier bewusst NICHT voraussetzen
    wollen, WIE genau FreeCAD ihn benannt hat."""
    for child in sublink.Group:
        if getattr(child, "LinkedObject", None) == real_obj:
            return child
    return None


def load_fixture_and_solve():
    fixture_sub, fixture_grand = ntu.fixture_paths(THIS_DIR)
    sub_path, grand_path = _output_paths()
    ntu.copy_fixture_to_output(fixture_sub, fixture_grand, sub_path, grand_path)

    jtu.open_other_documents_in_dir(os.path.dirname(grand_path), skip_path=grand_path)
    grand_doc = App.openDocument(grand_path)
    grand_asm = grand_doc.getObject("Assembly")
    boxC = jtu.get_by_label(grand_doc, "BoxC")
    sublink1 = jtu.get_by_label(grand_doc, "SubLink")
    # FCPROJECT-PATCH (Mehrfachinstanz-Fix, Nutzerauftrag 2026-09-14 "FreeCAD soll selber Namen
    # vergeben, wie es ueblich ist"): BEIDE Instanzen heissen intern NICHT "SubLink"/"SubLink001"
    # - new_grand_assembly_with_sublink() vergibt seit diesem Auftrag (wie
    # CommandInsertLink.py::onItemClicked() das fuer den echten Nutzer tut) als internen Namen das
    # LABEL der verlinkten Unterbaugruppe selbst (typischerweise "Assembly"), was hier mit
    # GrandTops EIGENEM Top-Level-AssemblyObject (ebenfalls "Assembly" genannt) UND miteinander
    # kollidiert - FreeCAD loest das selbst auf, typischerweise zu "Assembly001"/"Assembly002".
    # "SubLink" bleibt nur noch das LABEL (Lesbarkeit) - deshalb hier identitaetsbasiert suchen
    # (LinkedObject-Vergleich) statt einen festen internen Namen anzunehmen.
    sublink2 = None
    for obj in grand_doc.Objects:
        if obj.TypeId == "Assembly::AssemblyLink" and obj != sublink1 \
                and obj.LinkedObject == sublink1.LinkedObject:
            sublink2 = obj
            break
    assert sublink2 is not None, "Zweite AssemblyLink-Instanz nicht gefunden"

    grand_doc.recompute()
    grand_asm.solve(False)
    grand_doc.recompute()

    sub_doc = App.getDocument(sublink1.LinkedObject.Document.Name)
    subB = jtu.get_by_label(sub_doc, "BoxB")

    mirror_boxB1 = _find_mirror_by_source(sublink1, subB)
    mirror_boxB2 = _find_mirror_by_source(sublink2, subB)
    assert mirror_boxB1 is not None, "Spiegel von BoxB (Instanz 1) nicht gefunden"
    assert mirror_boxB2 is not None, "Spiegel von BoxB (Instanz 2) nicht gefunden"

    return grand_doc, grand_asm, boxC, sublink1, sublink2, mirror_boxB1, mirror_boxB2, sub_path, grand_path


def check_unique_names(label, obj1, obj2):
    """Prueft explizit, dass die INTERNEN Namen (obj.Name, nicht das Label!) von Instanz 1 und
    Instanz 2 tatsaechlich eindeutig sind - wie in FreeCAD ueblich (Name ist je Dokument
    IMMER eindeutig, FreeCAD haengt bei einer Kollision automatisch einen Zaehler an, z.B.
    "BoxB" -> "BoxB001"). Nutzerauftrag 2026-09-14: explizit sichtbar machen/pruefen statt nur
    stillschweigend vorauszusetzen. Labels DUERFEN dagegen gleich sein (Nutzerkorrektur) - das
    wird hier bewusst NICHT geprueft."""
    same_doc = obj1.Document == obj2.Document
    unique = (not same_doc) or (obj1.Name != obj2.Name)
    print(f"--- {label}: interne Namen ---")
    print(f"Instanz 1: Name='{obj1.Name}' (Dokument={obj1.Document.Name})")
    print(f"Instanz 2: Name='{obj2.Name}' (Dokument={obj2.Document.Name})")
    print(f"Eindeutig: {unique}")
    return unique


def check_boxB(label, boxC_plc, boxB_plc, tol_pos=1e-6, tol_rot=1e-4):
    expected = boxC_plc.multiply(OUTER_PLC1).multiply(OUTER_PLC2.inverse())
    pos_diff = (expected.Base - boxB_plc.Base).Length
    rot_diff_deg = abs(
        expected.Rotation.multiply(boxB_plc.Rotation.inverted()).Angle
    ) * 180.0 / 3.141592653589793
    print(f"--- {label} ---")
    print("BoxB (analytisch erwartet):", expected)
    print("BoxB (tatsaechlich):       ", boxB_plc)
    print(f"Differenz: {pos_diff:.9f} mm / {rot_diff_deg:.9f} deg")
    return pos_diff <= tol_pos and rot_diff_deg <= tol_rot


def main():
    (
        grand_doc,
        grand_asm,
        boxC,
        sublink1,
        sublink2,
        mirror_boxB1,
        mirror_boxB2,
        sub_path,
        grand_path,
    ) = load_fixture_and_solve()

    # Sanity-Check: der Solver muss BoxB tatsaechlich von der in der Fixture hinterlegten falschen
    # Startposition wegbewegt haben (analog zu den anderen Testfaellen).
    moved = (mirror_boxB1.Placement.Base - App.Vector(999, -999, 999)).Length > 1.0
    if not moved:
        print("RESULT: FAIL (BoxB wurde vom Solver nicht bewegt - Joint wirkungslos?)")
        return 1

    ok1 = check_boxB("Instanz 1", boxC.Placement, mirror_boxB1.Placement)
    ok2 = check_boxB("Instanz 2", boxC.Placement, mirror_boxB2.Placement)

    # Nutzerauftrag 2026-09-14: interne Namen (Name, NICHT Label) muessen zwischen Instanz 1 und
    # Instanz 2 eindeutig sein - explizit pruefen statt nur stillschweigend vorauszusetzen.
    ok_names_sublink = check_unique_names("AssemblyLink-Instanzen", sublink1, sublink2)
    ok_names_mirror = check_unique_names("BoxB-Spiegel", mirror_boxB1, mirror_boxB2)

    # Sync-Pruefung: BEIDE Spiegel muessen mit dem EINEN echten Sub#BoxB uebereinstimmen (Bug A -
    # vor dem Fix wich Instanz 2 hier ab bzw. wurde ueberhaupt nicht synchronisiert).
    sub_doc = App.getDocument(sublink1.LinkedObject.Document.Name)
    real_boxB = jtu.get_by_label(sub_doc, "BoxB")
    sync1 = (real_boxB.Placement.Base - mirror_boxB1.Placement.Base).Length
    sync2 = (real_boxB.Placement.Base - mirror_boxB2.Placement.Base).Length
    print(f"\nSync-Differenz (Instanz 1) BoxB: {sync1:.9f} mm")
    print(f"Sync-Differenz (Instanz 2) BoxB: {sync2:.9f} mm")
    ok_sync = sync1 <= 1e-6 and sync2 <= 1e-6

    grand_doc.save()
    print("\nGrandTop gespeichert:", grand_path)
    print("Sub gespeichert:", sub_path)

    # Speichern+Schliessen+Neuladen+Recompute - dieselbe schaerfere Kontrolle wie bei den
    # einfach verschachtelten Tests (siehe test_fixed_nested_flex.py).
    grand_doc.save()
    docname = grand_doc.Name
    App.closeDocument(docname)

    reload_exc = None
    try:
        grand_doc2 = App.openDocument(grand_path)
        grand_doc2.recompute()
    except Exception as e:  # noqa: BLE001 - bewusst breit, das IST der Test
        reload_exc = e

    if reload_exc is not None:
        print(f"RESULT: FAIL (Exception beim Neuladen+Recompute: {reload_exc!r})")
        return 1

    boxC2 = jtu.get_by_label(grand_doc2, "BoxC")
    sublink1_2 = jtu.get_by_label(grand_doc2, "SubLink")
    sublink2_2 = None
    for obj in grand_doc2.Objects:
        if obj.TypeId == "Assembly::AssemblyLink" and obj != sublink1_2 \
                and obj.LinkedObject == sublink1_2.LinkedObject:
            sublink2_2 = obj
            break
    assert sublink2_2 is not None, "Zweite AssemblyLink-Instanz nach Neuladen nicht gefunden"
    sub_doc2 = App.getDocument(sublink1_2.LinkedObject.Document.Name)
    real_boxB2 = jtu.get_by_label(sub_doc2, "BoxB")
    mirror_boxB1_2 = _find_mirror_by_source(sublink1_2, real_boxB2)
    mirror_boxB2_2 = _find_mirror_by_source(sublink2_2, real_boxB2)
    if mirror_boxB1_2 is None or mirror_boxB2_2 is None:
        print("RESULT: FAIL (Spiegel-Objekte nach Neuladen nicht mehr auffindbar)")
        return 1

    ok1_reload = check_boxB("Instanz 1, nach Neuladen", boxC2.Placement, mirror_boxB1_2.Placement)
    ok2_reload = check_boxB("Instanz 2, nach Neuladen", boxC2.Placement, mirror_boxB2_2.Placement)
    ok_names_sublink_reload = check_unique_names(
        "AssemblyLink-Instanzen, nach Neuladen", sublink1_2, sublink2_2
    )
    ok_names_mirror_reload = check_unique_names(
        "BoxB-Spiegel, nach Neuladen", mirror_boxB1_2, mirror_boxB2_2
    )

    if (
        ok1 and ok2 and ok_sync and ok1_reload and ok2_reload
        and ok_names_sublink and ok_names_mirror
        and ok_names_sublink_reload and ok_names_mirror_reload
    ):
        print("RESULT: PASS")
        return 0
    else:
        reasons = []
        if not ok1:
            reasons.append("Instanz 1 weicht von der analytischen Erwartung ab")
        if not ok2:
            reasons.append("Instanz 2 weicht von der analytischen Erwartung ab")
        if not ok_sync:
            reasons.append("Spiegel und echtes Sub#BoxB nicht synchron")
        if not ok1_reload:
            reasons.append("Instanz 1 weicht nach Neuladen ab")
        if not ok2_reload:
            reasons.append("Instanz 2 weicht nach Neuladen ab")
        if not ok_names_sublink:
            reasons.append("AssemblyLink-Instanzen haben denselben internen Namen")
        if not ok_names_mirror:
            reasons.append("BoxB-Spiegel haben denselben internen Namen")
        if not ok_names_sublink_reload:
            reasons.append("AssemblyLink-Instanzen nach Neuladen nicht mehr eindeutig benannt")
        if not ok_names_mirror_reload:
            reasons.append("BoxB-Spiegel nach Neuladen nicht mehr eindeutig benannt")
        print(f"RESULT: FAIL ({'; '.join(reasons)})")
        return 1


if __name__ == "__main__":
    sys.exit(main())
