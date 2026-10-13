"""
Regressionstest: Erdung einer Unterbaugruppe, die ueber ZWEI verschiedene Eltern im Baum steckt;
eine Eltern-BG ist angebunden, die andere lose (Regel C5).

Anlass (2026-10-13, PROJ_CNC3018): BG73 und BG75 enthalten beide den Motor BG67 und erden jeweils
dessen Stator. In BG22 ist BG75 angebunden, BG73 noch lose. Die Erdung von BG73 landete beim
Stator der BG75-Instanz: BG73 schwamm (ihre Gelenke fielen aus dem Solve, Teile "nicht am
Platz"), BG75 war doppelt gehalten. Zwei Stellen in getGroundedParts(): Aufloesung der
verschachtelten Erdung ohne Instanz-Kontext (jetzt findInstanceUnder()) und Eintrag des Objekts
aus dem Unterdokument statt der Instanz.

Struktur:
    Sub  (eigenes Dok.): SubA, SubB (Fixed)
    MidA (eigenes Dok.): BoxD + SubLink(Sub, flexibel), Fixed BoxD-SubLink.SubB, GEERDET: SubLink.SubA
    MidB (eigenes Dok.): BoxD + SubLink(Sub, flexibel), Fixed BoxD-SubLink.SubB, GEERDET: SubLink.SubA
    Grand(eigenes Dok.): BoxC (geerdet)
                         + MidALink(MidA) -[Fixed]- BoxC         (angebunden)
                         + MidBLink(MidB), lose, versetzt abgelegt
Geprueft in Grand, nachdem in MidBLink die BoxD-Instanz von ihrem Sub weggeschoben wurde:
  1. MidBs SubA-Instanz bleibt an ihrer Ablage-Position (Anker), BoxD kommt zurueck - MidB wird
     in sich geloest (vorher: lose, Gelenk blieb verletzt).
  2. Alle Gelenke erfuellt, Solve ok.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_nested_cross_parent_grounding/test_nested_cross_parent_grounding.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_nested_cross_parent_grounding/build_fixture.py
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

TEST_NAME = "test_nested_cross_parent_grounding"
INNER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # SubA
INNER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # SubB
MID_PLC1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=30)  # SubB
MID_PLC2 = jtu.face_jcs_placement("Face3", flip=True, extra_z_rotation_deg=15)  # BoxD
OUTER_PLC1 = jtu.face_jcs_placement("Face4", flip=False, extra_z_rotation_deg=90)  # BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face5", flip=True, extra_z_rotation_deg=40)  # BoxD in MidA
MIDB_DROP = App.Placement(App.Vector(150, -60, 40), App.Rotation(App.Vector(0, 0, 1), 90))
PUSH = App.Vector(7, -4, 3)
DOC_SUFFIXES = ("sub", "mida", "midb", "grand")


def fixture_paths():
    return {s: os.path.join(FIXTURES_DIR, f"{TEST_NAME}_{s}.FCStd") for s in DOC_SUFFIXES}


def assembly_of(doc):
    return [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]


def world(obj, chain):
    plc = App.Placement()
    for c in chain:
        plc = plc.multiply(c.Placement)
    return plc.multiply(obj.Placement)


def main():
    import test_originals_untouched as tou

    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    out = os.path.splitext(base)[0] if base else os.path.join(THIS_DIR, "fcstd-output", "default")
    if os.path.isdir(out):
        shutil.rmtree(out)
    shutil.copytree(FIXTURES_DIR, out)

    grand_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
    grand = assembly_of(grand_doc)
    grand_doc.recompute()
    grand.solve()

    midb = jtu.get_by_label(grand_doc, "MidBLink")
    sublink = [o for o in midb.Group if o.TypeId == "Assembly::AssemblyLink"][0]
    subA = [o for o in sublink.Group if o.Label == "SubA"][0]
    boxD = [o for o in midb.Group if o.Label == "BoxD"][0]
    anchor_before = world(subA, [midb, sublink])
    plc = App.Placement(boxD.Placement)
    plc.Base = plc.Base + PUSH
    boxD.Placement = plc
    print(f"BoxD in MidB um {PUSH.Length:.2f} mm vom Sub weggeschoben")

    rc = grand.solve()
    print(f"Solve rc={rc}")
    ok = rc == 0
    anchor_after = world(subA, [midb, sublink])
    moved = (anchor_after.Base - anchor_before.Base).Length
    print(f"Anker (SubA in MidB) bewegt: {moved:.6f} mm")
    if moved > 1e-6:
        print("FEHLER: lose Unterbaugruppe ist nicht an ihrer Ablage-Position geblieben")
        ok = False
    with tempfile.TemporaryDirectory() as tmp:
        errors, _ = tou.check_joints_in_solver_view(grand, tmp)
    print(f"Nach dem Solve: {len(errors)} verletzte Gelenke")
    for e in errors:
        print(f"  FEHLER: {e}")
    ok &= not errors

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
