"""
Regressionstest: Startlage einer frisch eingefuegten, verschachtelten Baugruppe.

Anlass (2026-10-11, PROJ_CNC3018): BG75 (enthaelt BG47 flexibel und den Motor 067 STARR) wurde
flexibel und noch unverbunden in BG22 eingefuegt. Der erste Solve brauchte 60 s, scheiterte in
der GUI (iterNo > iterMax), die Teile flogen umher. Zwei Ursachen, beide schon VOR dem ersten
Solve sichtbar:
  1. Ein neu angelegter verschachtelter Spiegel startete in der Lage des INNERSTEN Originals
     (BG47s Buchse) statt in der seines Gegenstuecks (BG75s Buchsen-Spiegel, von BG75 geloest):
     470 mm daneben (AssemblyLink::synchronizeComponents(), startPlacementSource()).
  2. Der Marker eines Joints auf einer STARREN Unterbaugruppe aus einem anderen Dokument wurde
     relativ zum referenzierten Teil (Rotor) statt zum starren Koerper gesetzt: 6 mm/70 Grad
     daneben (AssemblyObject::handleOneSideOfJoint()).

Struktur:

    SubF (eigenes Dok.): FA (GEERDET) -[Fixed]- FB
    SubR (eigenes Dok.): RA (GEERDET) -[Fixed]- RB
    Mid  (eigenes Dok.): BoxD (geerdet)
                         + SubFLink(SubF, FLEXIBEL) -[Fixed MID_F, Ref. SubFLink + "FB.Face1"]- BoxD
                         + SubRLink(SubR, STARR)    -[Fixed MID_R, Ref. SubRLink + "RB.Face1"]- BoxD
                         gespeichert NACH dem Solve: Spiegel in Mid != Originale in SubF/SubR
    Grand(eigenes Dok.): BoxC (geerdet), Mid wird im Test flexibel und UNVERBUNDEN eingefuegt
                         (wie der Nutzer: einfuegen, noch kein Joint)

Geprueft in Grand:
  1. Direkt nach dem Einfuegen, VOR dem ersten Solve: alle Joints in der Solver-Sicht erfuellt.
  2. Der erste Recompute bewegt nichts (alle Teile schon in gueltiger Lage).
  3. Danach Joints erfuellt, Originale in Mid/SubF/SubR unveraendert (Regel C2).

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_nested_insert_start_state/test_nested_insert_start_state.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_nested_insert_start_state/build_fixture.py
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
import nested_test_utils as ntu  # noqa: E402

TEST_NAME = "test_nested_insert_start_state"

INNER_F_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # FA
INNER_F_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # FB
INNER_R_PLC1 = jtu.face_jcs_placement("Face3", flip=False, extra_z_rotation_deg=35)  # RA
INNER_R_PLC2 = jtu.face_jcs_placement("Face4", flip=True, extra_z_rotation_deg=-15)  # RB
MID_F_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=30)  # BoxD
MID_F_PLC2 = jtu.face_jcs_placement("Face2", flip=True, extra_z_rotation_deg=15)  # FB-Spiegel
MID_R_PLC1 = jtu.face_jcs_placement("Face5", flip=False, extra_z_rotation_deg=-40)  # BoxD
MID_R_PLC2 = jtu.face_jcs_placement("Face6", flip=True, extra_z_rotation_deg=60)  # RB im starren Link
INSERT_PLC = App.Placement(App.Vector(120, -80, 45), App.Rotation(App.Vector(1, 2, 3), 70))

DOC_SUFFIXES = ("subf", "subr", "mid", "grand")


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


def world_placements(container, plc, result):
    for obj in container.Group:
        if "Placement" not in obj.PropertiesList:
            continue
        world = plc.multiply(obj.Placement)
        result[obj.Name] = world
        if obj.TypeId == "Assembly::AssemblyLink":
            world_placements(obj, world, result)
    return result


def main():
    import test_originals_untouched as tou

    out = output_dir()
    grand_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
    mid_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_mid.FCStd"))
    ntu.remember_originals(grand_doc)
    grand = assembly_of(grand_doc)
    mid = assembly_of(mid_doc)
    grand_doc.recompute()

    # Einfuegen wie Assembly_Insert: flexibler AssemblyLink, noch ohne Joint.
    App.setActiveDocument(grand_doc.Name)
    link = grand.newObject("Assembly::AssemblyLink", mid.Label)
    link.LinkedObject = mid
    link.Rigid = False
    link.Placement = INSERT_PLC
    # Spiegel anlegen (in der GUI im selben Recompute vor dem Solve der Baugruppe).
    link.recompute()
    for child in link.Group:
        if child.TypeId == "Assembly::AssemblyLink":
            child.recompute()

    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        errors, _ = tou.check_joints_in_solver_view(grand, tmp)
        print(f"Vor dem ersten Solve: {len(errors)} verletzte Joints")
        for e in errors:
            print(f"  FEHLER: {e}")
        ok &= not errors

        before = world_placements(link, link.Placement, {})
        grand_doc.recompute()
        rc = grand.solve()
        if rc != 0:
            print(f"FEHLER: Solve rc={rc}")
            ok = False
        after = world_placements(link, link.Placement, {})
        moved = max((before[k].Base - after[k].Base).Length for k in before if k in after)
        print(f"Groesste Bewegung im ersten Recompute: {moved:.6f} mm ({len(before)} Objekte)")
        if moved > 1e-6:
            print("FEHLER: eingefuegte Baugruppe hat sich beim ersten Recompute bewegt")
            ok = False

        errors, _ = tou.check_joints_in_solver_view(grand, tmp)
        print(f"Nach dem Solve: {len(errors)} verletzte Joints")
        for e in errors:
            print(f"  FEHLER: {e}")
        ok &= not errors

    ok &= ntu.originals_unchanged("Nach dem Solve")

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
