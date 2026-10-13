"""
Regressionstest: verschachtelte Unterbaugruppe von starr auf flexibel umschalten - die aeussere
Baugruppe muss danach ohne Sprung weiterlaufen.

Anlass (2026-10-12, PROJ_CNC3018): Motor 067 in BG75 von starr auf flexibel umgeschaltet, BG75
steckt in BG22. In BG22 flog der Motor 374mm/90 Grad weg, Solve 58-166s, gescheitert. Drei
Ursachen in AssemblyLink::synchronizeComponents():
  1. Nachfuehren eines flexiblen Container-Spiegels verschob seine Teile in der Welt - jetzt
     weltlage-erhaltend (Teile bekommen die Gegenverschiebung).
  2. Beim Wechsel ZU flexibel kopierte der Spiegel die Teil-Lagen vom Gegenstueck (BG75) - die
     kennen nur die Lage in BG75, nicht die geloeste Lage des starren Koerpers in BG22.
  3. Ein verschachtelter Spiegel wird VOR seinem Eltern-Objekt neu berechnet und arbeitete mit
     dem veralteten Starr-Wert aus der Datei - jetzt wendet er Regel C1 selbst zuerst an.

Struktur:
    Sub  (eigenes Dok.): SubA (GEERDET) -[Fixed]- SubB
    Mid  (eigenes Dok.): BoxD (geerdet) + SubLink(Sub, STARR, Container-Versatz) -[Fixed MID]- BoxD
    Grand(eigenes Dok.): BoxC (geerdet) + MidLink(Mid, flexibel) -[Fixed OUTER, mit Drehung]- MidLink.BoxD
Geprueft:
  A. Live: Grand geoeffnet und geloest, dann in Mid SubLink.Rigid = False -> Grand vor dem ersten
     Solve ohne verletzte Gelenke, danach Solve ok.
  B. Wiedereroeffnen: Mid mit flexiblem SubLink gespeichert, Grand (noch mit starrem Spiegel)
     frisch geoeffnet -> vor dem ersten Solve ohne verletzte Gelenke, danach Solve ok.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_nested_rigid_to_flexible_switch/test_nested_rigid_to_flexible_switch.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_nested_rigid_to_flexible_switch/build_fixture.py
"""
import os
import shutil
import sys
import tempfile

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
FIXTURES_DIR = os.path.join(THIS_DIR, "fixtures")
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "test_originals_untouched"))
import joint_test_utils as jtu  # noqa: E402

TEST_NAME = "test_nested_rigid_to_flexible_switch"
INNER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # SubA
INNER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # SubB
MID_PLC1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=30)  # BoxD
MID_PLC2 = jtu.face_jcs_placement("Face3", flip=True, extra_z_rotation_deg=15)  # SubB im starren Link
OUTER_PLC1 = jtu.face_jcs_placement("Face4", flip=False, extra_z_rotation_deg=90)  # BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face5", flip=True, extra_z_rotation_deg=40)  # BoxD-Spiegel
SUBLINK_OFFSET = App.Placement(App.Vector(-140, 450, 55), App.Rotation(App.Vector(0, 0, 1), 0))
DOC_SUFFIXES = ("sub", "mid", "grand")


def fixture_paths():
    return {s: os.path.join(FIXTURES_DIR, f"{TEST_NAME}_{s}.FCStd") for s in DOC_SUFFIXES}


def output_dir():
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    out = os.path.splitext(base)[0] if base else os.path.join(THIS_DIR, "fcstd-output", "default")
    if os.path.isdir(out):
        shutil.rmtree(out)
    shutil.copytree(FIXTURES_DIR, out)
    return out


def assembly_of(doc):
    return [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]


def close_all():
    for name in list(App.listDocuments()):
        App.closeDocument(name)


def refresh_mirrors(grand):
    """Spiegel aktualisieren wie ein Recompute, aber ohne den Solve der Baugruppe."""
    def rec(container):
        for obj in container.Group:
            if obj.TypeId == "Assembly::AssemblyLink":
                obj.recompute()
                rec(obj)
    rec(grand)


def check(label, grand, tmp):
    import test_originals_untouched as tou
    errors, _ = tou.check_joints_in_solver_view(grand, tmp)
    print(f"{label}: {len(errors)} verletzte Gelenke")
    for e in errors:
        print(f"  FEHLER: {e}")
    return not errors


def main():
    out = output_dir()
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        # A. Live umschalten
        grand_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
        grand = assembly_of(grand_doc)
        grand_doc.recompute()
        grand.solve()
        ok &= check("A vorher", grand, tmp)
        mid_doc = App.getDocument(f"{TEST_NAME}_mid")
        sublink = jtu.get_by_label(mid_doc, "SubLink")
        sublink.Rigid = False
        mid_doc.recompute()
        refresh_mirrors(grand)
        ok &= check("A nach dem Umschalten, vor dem Solve", grand, tmp)
        grand_doc.recompute()
        rc = grand.solve()
        print(f"A Solve rc={rc}")
        ok &= rc == 0
        ok &= check("A nach dem Solve", grand, tmp)
        mid_doc.save()
        close_all()

        # B. Grand mit starrem Spiegel aus der Fixtur, Mid flexibel (eben gespeichert)
        shutil.copy2(os.path.join(FIXTURES_DIR, f"{TEST_NAME}_grand.FCStd"),
                     os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
        grand_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
        grand = assembly_of(grand_doc)
        refresh_mirrors(grand)
        ok &= check("B nach dem Oeffnen, vor dem Solve", grand, tmp)
        grand_doc.recompute()
        rc = grand.solve()
        print(f"B Solve rc={rc}")
        ok &= rc == 0
        ok &= check("B nach dem Solve", grand, tmp)
    close_all()

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
