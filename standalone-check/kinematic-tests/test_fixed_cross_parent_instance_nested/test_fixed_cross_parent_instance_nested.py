"""
Regressionstest: DIESELBE Unterbaugruppe steckt in ZWEI VERSCHIEDENEN Eltern-Baugruppen, die
beide in derselben obersten Baugruppe eingebaut sind - also nicht als Geschwister nebeneinander
(das deckt test_fixed_duplicate_instance_nested ab), sondern ueber getrennte Pfade.

Anlass (2026-10-11, PROJ_CNC3018): BG47 (Motorbuchse) steckte in BG22 zweimal -
BG22 > BG37 > BG43 > BG47 und BG22 > BG75 > BG47. hasSiblingInstances() erkannte Mehrfach-
instanzen nur unter Geschwistern; beide Buchsen wurden EINE Identitaet, die Joints von BG75
hingen an der Buchse von BG43, der Solve scheiterte (iterMax, 52 "redundante" Joints).

Struktur:

    Sub   (eigenes Dok.): SubA (GEERDET) -[innerer Fixed]- SubB
    MidA  (eigenes Dok.): BoxD (geerdet) + SubLink(Sub) -[Fixed, MIDA_PLC]- SubLink.SubB
    MidB  (eigenes Dok.): BoxD (geerdet, eigene Instanz) + SubLink(Sub) -[Fixed, MIDB_PLC]- SubLink.SubB
    Grand (eigenes Dok.): BoxC (geerdet)
                          + MidALink(MidA) -[Fixed, OUTER_A]- MidALink.BoxD
                          + MidBLink(MidB) -[Fixed, OUTER_B]- MidBLink.BoxD

MidA und MidB richten Sub UNTERSCHIEDLICH aus (andere Joint-Placements). Sind die beiden
Sub-Instanzen in Grand faelschlich dieselbe Identitaet, kann der Solver nicht beide Lagen
gleichzeitig erfuellen. Geprueft in Grand (analytisch):
  1. BoxD-Spiegel (je Instanz) liegen an BoxC * OUTER1 * OUTER2^-1.
  2. SubB-Spiegel je Instanz an Box(D|E) * MID1 * MID2^-1, SubA-Spiegel ueber den inneren Joint.
  3. Im ASMT-Export gibt es ZWEI getrennte Solver-Koerper fuer SubB.
  4. IdentityGraph-Handles konsistent (verifyIdentityGraphHandleConsistency).
  5. Originale in den Unterdokumenten unveraendert (Regel C2).
Die bekannte 180-Grad-Loesung an einem Fixed-Gelenk wird akzeptiert, wenn der Gelenkpunkt exakt
stimmt (ntu.fixed_matches()).

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_fixed_cross_parent_instance_nested/test_fixed_cross_parent_instance_nested.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_fixed_cross_parent_instance_nested/build_fixture.py
"""
import os
import re
import shutil
import sys
import tempfile

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
FIXTURES_DIR = os.path.join(THIS_DIR, "fixtures")
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402

TEST_NAME = "test_fixed_cross_parent_instance_nested"

INNER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # SubA
INNER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # SubB
MIDA_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=30)  # BoxD
MIDA_PLC2 = jtu.face_jcs_placement("Face2", flip=True, extra_z_rotation_deg=15)  # SubB (in MidA)
MIDB_PLC1 = jtu.face_jcs_placement("Face3", flip=False, extra_z_rotation_deg=70)  # BoxD (in MidB)
MIDB_PLC2 = jtu.face_jcs_placement("Face4", flip=True, extra_z_rotation_deg=5)  # SubB (in MidB)
OUTER_A_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=40)  # BoxC
OUTER_A_PLC2 = jtu.face_jcs_placement("Face2", flip=True, extra_z_rotation_deg=25)  # BoxD-Spiegel
OUTER_B_PLC1 = jtu.face_jcs_placement("Face5", flip=False, extra_z_rotation_deg=-35)  # BoxC
OUTER_B_PLC2 = jtu.face_jcs_placement("Face6", flip=True, extra_z_rotation_deg=50)  # BoxD-Spiegel (MidB)

DOC_SUFFIXES = ("sub", "mida", "midb", "grand")


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


def world_placements(asm):
    result = {}

    def rec(container, plc):
        for obj in container.Group:
            if "Placement" not in obj.PropertiesList:
                continue
            world = plc.multiply(obj.Placement)
            result[obj] = world
            if obj.TypeId == "Assembly::AssemblyLink":
                rec(obj, world)

    rec(asm, App.Placement())
    return result


def main():
    out = output_dir()
    grand_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
    ntu.remember_originals(grand_doc)
    grand = assembly_of(grand_doc)
    grand_doc.recompute()
    rc = grand.solve(False)
    grand.solve(False)
    worlds = world_placements(grand)

    boxC = jtu.get_by_label(grand_doc, "BoxC")
    mida_link = jtu.get_by_label(grand_doc, "MidALink")
    midb_link = jtu.get_by_label(grand_doc, "MidBLink")
    if not all((boxC, mida_link, midb_link)):
        print("RESULT: FAIL (Fixtur-Objekte nicht gefunden)")
        return 1

    ok = rc == 0
    if not ok:
        print(f"Solve rc={rc}")

    cases = (
        ("A", mida_link, "BoxD", OUTER_A_PLC1, OUTER_A_PLC2, MIDA_PLC1, MIDA_PLC2),
        ("B", midb_link, "BoxD", OUTER_B_PLC1, OUTER_B_PLC2, MIDB_PLC1, MIDB_PLC2),
    )
    subB_world = []
    for tag, mid_link, box_label, o1, o2, m1, m2 in cases:
        box = ntu.get_mirror(mid_link, box_label)
        sublink = [o for o in mid_link.Group if o.TypeId == "Assembly::AssemblyLink"][0]
        subA = ntu.get_mirror(sublink, "SubA")
        subB = ntu.get_mirror(sublink, "SubB")
        box_expected = worlds[boxC].multiply(o1).multiply(o2.inverse())
        ok &= ntu.fixed_matches(f"Instanz {tag}: {box_label}", box_expected, worlds[box], o2)
        subB_expected = worlds[box].multiply(m1).multiply(m2.inverse())
        ok &= ntu.fixed_matches(f"Instanz {tag}: SubB", subB_expected, worlds[subB], m2)
        subA_expected = worlds[subB].multiply(INNER_PLC2).multiply(INNER_PLC1.inverse())
        ok &= ntu.fixed_matches(f"Instanz {tag}: SubA", subA_expected, worlds[subA], INNER_PLC1)
        subB_world.append(worlds[subB])

    distance = (subB_world[0].Base - subB_world[1].Base).Length
    print(f"Abstand der beiden SubB-Instanzen: {distance:.3f} mm")
    if distance < 1e-3:
        print("FEHLER: beide SubB-Instanzen liegen aufeinander - eine gemeinsame Identitaet?")
        ok = False

    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "grand.asmt")
        grand.exportAsASMT(path)
        text = open(path).read()
    subB_bodies = sorted(set(re.findall(r"\n\t\t\t\t(\S+#\S*Box\S*)\n", text)))
    print(f"Solver-Koerper: {subB_bodies}")
    body_count = len(subB_bodies)
    # BoxC, 2x BoxD, je 2x SubA/SubB = 7 Koerper; SubA/SubB je Instanz eigener Koerper
    if body_count < 7:
        print(f"FEHLER: nur {body_count} Box-Koerper im Solver (erwartet 7: BoxC, 2x BoxD, 2x SubA, 2x SubB)")
        ok = False

    split = grand.verifyIdentityGraphHandleConsistency()
    if split:
        print(f"FEHLER: IdentityGraph-Handles inkonsistent: {split}")
        ok = False

    ok &= ntu.originals_unchanged("Nach dem Solve")

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
