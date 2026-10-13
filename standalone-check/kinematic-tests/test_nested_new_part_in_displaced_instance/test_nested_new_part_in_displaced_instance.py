"""
Regressionstest: neues Teil in einer schon eingebauten, versetzten flexiblen Unterbaugruppe.

Anlass (2026-10-13, PROJ_CNC3018): in BG75 zwei Schrauben per Teiletausch ersetzt. BG75 steckt in
BG22 ~430mm und 90 Grad anders als in BG75 selbst. Die neuen Schrauben-Spiegel in BG22 starteten
in ihrer BG75-Lage - 430mm/90 Grad neben Scheibe und Nutstein, BG22 hing im Solve. Jetzt
uebernimmt ein neu angelegter Spiegel den Versatz seines naechsten schon vorhandenen Nachbarn
(AssemblyLink::synchronizeComponents(), Nachlauf "neues Teil in bestehender Instanz").

Struktur:
    Mid  (eigenes Dok.): BoxD (geerdet)
    Grand(eigenes Dok.): BoxC (geerdet) + MidLink(Mid, flexibel) -[Fixed OUTER, 90 Grad]- MidLink.BoxD
Test (wie im Projekt): Mid allein oeffnen, neues Teil (BoxA, im Code boxE) per Fixed an BoxD, Mid
speichern und schliessen; dann Grand oeffnen - der MidLink legt den Spiegel des neuen Teils an.
Geprueft: vor dem ersten Solve in Grand alle Gelenke erfuellt; danach Solve ok.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_nested_new_part_in_displaced_instance/test_nested_new_part_in_displaced_instance.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_nested_new_part_in_displaced_instance/build_fixture.py
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

TEST_NAME = "test_nested_new_part_in_displaced_instance"
OUTER_PLC1 = jtu.face_jcs_placement("Face4", flip=False, extra_z_rotation_deg=90)  # BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face5", flip=True, extra_z_rotation_deg=40)  # BoxD-Spiegel
NEW_PLC1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=25)  # BoxD
NEW_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # BoxE
DOC_SUFFIXES = ("mid", "grand")


def fixture_paths():
    return {s: os.path.join(FIXTURES_DIR, f"{TEST_NAME}_{s}.FCStd") for s in DOC_SUFFIXES}


def assembly_of(doc):
    return [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]


def refresh_mirrors(container):
    for obj in container.Group:
        if obj.TypeId == "Assembly::AssemblyLink":
            obj.recompute()
            refresh_mirrors(obj)


def check(label, asm, tmp):
    import test_originals_untouched as tou
    errors, _ = tou.check_joints_in_solver_view(asm, tmp)
    print(f"{label}: {len(errors)} verletzte Gelenke")
    for e in errors:
        print(f"  FEHLER: {e}")
    return not errors


def main():
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    out = os.path.splitext(base)[0] if base else os.path.join(THIS_DIR, "fcstd-output", "default")
    if os.path.isdir(out):
        shutil.rmtree(out)
    shutil.copytree(FIXTURES_DIR, out)

    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        # Wie im Projekt: die Unterbaugruppe allein bearbeiten und speichern ...
        mid_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_mid.FCStd"))
        mid = assembly_of(mid_doc)
        boxD = jtu.get_by_label(mid_doc, "BoxD")
        box_paths = jtu.ensure_box_bodies_in(out, ["BoxA"])
        boxE = jtu.link_external_box(mid_doc, "BoxA", box_paths["BoxA"])
        mid.addObject(boxE)
        mid_doc.recompute()
        jtu.make_joint(mid, 0, boxD, boxE, NEW_PLC1, NEW_PLC2, elem1="Face2", elem2="Face1")
        boxE.Placement = boxD.Placement.multiply(NEW_PLC1).multiply(NEW_PLC2.inverse())
        mid_doc.recompute()
        mid.solve()
        groups = [o for o in mid.OutList if o.TypeId == "Assembly::JointGroup"]
        if len(groups) != 1:
            print(f"FEHLER: Fixtur ungeeignet - {len(groups)} JointGroups in Mid")
            ok = False
        ok &= check("Mid allein", mid, tmp)
        mid_doc.save()
        for name in list(App.listDocuments()):
            App.closeDocument(name)

        # ... dann die aeussere Baugruppe oeffnen: der Spiegel des neuen Teils entsteht jetzt.
        grand_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
        grand = assembly_of(grand_doc)
        refresh_mirrors(grand)
        if len(grand.getJointInstances()) != 2:
            print("FEHLER: das neue Gelenk aus Mid fehlt im Solve von Grand")
            ok = False
        ok &= check("Grand geoeffnet, vor dem Solve", grand, tmp)
        grand_doc.recompute()
        rc = grand.solve()
        print(f"Solve rc={rc}")
        ok &= rc == 0
        ok &= check("nach dem Solve", grand, tmp)

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
