"""
Regressionstest Regel C5, Punkt 4: lose eingebaute Unterbaugruppe OHNE eigene Erdung.

Anlass (2026-10-13, Nutzerentscheidung): ein eingefuegtes Teil darf nie undefiniert herumschwimmen.
Eine flexible Unterbaugruppe ohne eigene Erdung wurde, solange sie unverbunden war, gar nicht
geloest - ihre Gelenke fielen als unverbunden heraus, verschobene oder neue Teile blieben liegen.
Jetzt haelt ihr erstes Teil sie an ihrer Ablage-Position (getGroundedParts()); beim ersten Gelenk
zur restlichen Baugruppe faellt dieser Anker weg (gleiche Gruppierung wie bei echten Erdungen).

Struktur:
    Mid  (eigenes Dok.): BoxD (erstes Teil) -[Fixed]- BoxA, KEINE Erdung
    Grand(eigenes Dok.): BoxC (geerdet) + MidLink(Mid, flexibel), lose, versetzt abgelegt
Geprueft: BoxA-Instanz in Grand weggeschoben, Solve -> BoxA kommt zurueck, BoxD (Anker) bleibt
an der Ablage-Position, Gelenke erfuellt. Container als Ganzes verschoben -> bleibt an der neuen
Ablage-Position (Punkt 3). Danach: Fixed BoxC-BoxD in Grand angelegt -> der Anker
faellt weg, Mid folgt dem neuen Gelenk.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_nested_loose_subassembly_without_ground/test_nested_loose_subassembly_without_ground.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_nested_loose_subassembly_without_ground/build_fixture.py
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

TEST_NAME = "test_nested_loose_subassembly_without_ground"
MID_PLC1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=25)  # BoxD
MID_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # BoxA
OUTER_PLC1 = jtu.face_jcs_placement("Face4", flip=False, extra_z_rotation_deg=90)  # BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face5", flip=True, extra_z_rotation_deg=40)  # BoxD-Instanz
MID_DROP = App.Placement(App.Vector(120, 40, -30), App.Rotation(App.Vector(1, 0, 0), 90))
PUSH = App.Vector(6, 5, -4)
DOC_SUFFIXES = ("mid", "grand")


def fixture_paths():
    return {s: os.path.join(FIXTURES_DIR, f"{TEST_NAME}_{s}.FCStd") for s in DOC_SUFFIXES}


def assembly_of(doc):
    return [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]


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
        grand_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
        grand = assembly_of(grand_doc)
        grand_doc.recompute()
        grand.solve()
        midlink = jtu.get_by_label(grand_doc, "MidLink")
        boxD = [o for o in midlink.Group if o.Label == "BoxD"][0]
        boxA = [o for o in midlink.Group if o.Label == "BoxA"][0]
        anchor = App.Placement(boxD.Placement)
        plc = App.Placement(boxA.Placement)
        plc.Base = plc.Base + PUSH
        boxA.Placement = plc
        rc = grand.solve()
        print(f"Solve rc={rc}")
        ok &= rc == 0
        moved = (boxD.Placement.Base - anchor.Base).Length
        print(f"Anker BoxD bewegt: {moved:.6f} mm")
        if moved > 1e-6:
            print("FEHLER: lose Unterbaugruppe ist nicht an ihrer Ablage-Position geblieben")
            ok = False
        ok &= check("lose, nach dem Solve", grand, tmp)

        # Ziehen als Ganzes (C5 Punkt 3): Container verschieben = neue Ablage-Position.
        shift = App.Vector(-20, 15, 10)
        world_before = midlink.Placement.multiply(boxD.Placement)
        moved_link = App.Placement(midlink.Placement)
        moved_link.Base = moved_link.Base + shift
        midlink.Placement = moved_link
        rc = grand.solve()
        world_after = midlink.Placement.multiply(boxD.Placement)
        dev = (world_after.Base - (world_before.Base + shift)).Length
        print(f"Als Ganzes verschoben: Abweichung der neuen Ablage-Position {dev:.6f} mm")
        if rc != 0 or dev > 1e-6:
            print("FEHLER: lose Unterbaugruppe bleibt nicht an der neuen Ablage-Position")
            ok = False
        anchor = App.Placement(boxD.Placement)

        # Erstes Gelenk zur restlichen Baugruppe: der Anker faellt weg, Mid folgt dem Gelenk.
        boxC = jtu.get_by_label(grand_doc, "BoxC")
        jtu.make_joint(grand, 0, boxC, boxD, OUTER_PLC1, OUTER_PLC2, elem1="Face4", elem2="Face5")
        grand_doc.recompute()
        rc = grand.solve()
        print(f"Angebunden: Solve rc={rc}")
        ok &= rc == 0
        moved = (boxD.Placement.Base - anchor.Base).Length
        print(f"BoxD nach dem Anbinden bewegt: {moved:.3f} mm (soll > 0, Anker weg)")
        if moved < 1e-3:
            print("FEHLER: Anker ist nach dem ersten Gelenk nicht weggefallen")
            ok = False
        ok &= check("angebunden, nach dem Solve", grand, tmp)

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
