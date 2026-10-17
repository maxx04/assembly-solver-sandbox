"""
Regressionstest Regel C7: Redundanz innerhalb einer Toleranz wird geschluckt.

Anlass (2026-10-14, PROJ_CNC3018, BG22 mit Tisch BG89): ein Joint anzulegen dauerte ueber 30 s,
dazu viele "ueberbestimmt"-Warnungen. OndselSolver suchte bei jedem Solve die redundanten
Bedingungen von vorn (~80% der Solve-Zeit). Nutzer: "Solver soll z.B. zwei Fuehrungen schlucken,
in bestimmtem Toleranzbereich." Jetzt merkt sich AssemblyObject die Redundanzen (nimmt sie beim
naechsten Solve vorab heraus) und misst jede herausgenommene Bedingung nach: innerhalb der
Toleranz "geschluckt", darueber "Konflikt".

Aufbau (wie zwei Fuehrungen): BoxA (geerdet) -[Fixed]- BoxC (zweite Stange), BoxB gleitet auf
BoxA (Gleiter 1) UND auf BoxC (Gleiter 2, parallel, stimmig).
Geprueft:
  1. stimmig: rc 0, keine widerspruechliche Gruppe, alle herausgenommenen Bedingungen geschluckt;
     ein zweiter Solve (mit Gedaechtnis) liefert dieselbe Lage.
  2. Gleiter 2 um 1 mm quer versetzt: Konflikt gemeldet (Gruppe mit beiden Gleitern).
  3. Toleranz auf 2 mm erhoeht: derselbe Versatz wird geschluckt.
  4. Gleiter 1 geloescht: Gedaechtnis verworfen, Gleiter 2 allein haelt BoxB, keine Redundanz.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_redundancy_tolerance_flat/test_redundancy_tolerance_flat.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_redundancy_tolerance_flat/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

TEST_NAME = "test_redundancy_tolerance_flat"
BOXES = ("BoxA", "BoxB", "BoxC")
SLD1_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=25)  # BoxA
SLD1_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # BoxB
FIX_PLC1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=0)  # BoxA
FIX_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=0)  # BoxC
# Gleiter 2: Weltachse parallel zu Gleiter 1, seitlich versetzt (Bezug in BoxC bzw. BoxB lokal,
# in build_fixture.py aus der Weltlage berechnet).
SIDE_OFFSET = App.Vector(30, 0, 0)
PARAM_PATH = "User parameter:BaseApp/Preferences/Mod/Assembly"


def slider_joints(doc):
    return sorted(
        [j for j in doc.Objects if getattr(j, "JointType", None) == "Slider"], key=lambda j: j.Name
    )


def check_slider(label, joint):
    import UtilsAssembly
    g1 = UtilsAssembly.getJcsGlobalPlc(joint.Placement1, joint.Reference1)
    g2 = UtilsAssembly.getJcsGlobalPlc(joint.Placement2, joint.Reference2)
    return jtu.check_slider(jtu.report(label, g1, g2))


def solve_and_report(label, assembly):
    rc = assembly.solve()
    groups = assembly.getRedundancyGroups()
    states = assembly.getRedundancyStates()
    print(f"{label}: rc={rc}")
    for g in groups:
        print(f"  Gruppe: {g}")
    for s in states:
        print(f"  Bedingung: {s}")
    return rc, groups, states


def main():
    params = App.ParamGet(PARAM_PATH)
    params.RemFloat("RedundancyToleranceLength")
    fixture_file = jtu.fixture_path(THIS_DIR)
    out_path = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD", "/tmp/kinematic_test_redundancy_tolerance.FCStd")
    jtu.copy_fixture_to_output(fixture_file, out_path, box_labels=BOXES)
    doc = App.openDocument(out_path)
    assembly = doc.getObject("Assembly")
    boxB = jtu.get_by_label(doc, "BoxB")
    doc.recompute()
    ok = True

    # 1. stimmig
    rc, groups, states = solve_and_report("Stimmig", assembly)
    first = App.Placement(boxB.Placement)
    rc2, groups2, states2 = solve_and_report("Stimmig, zweiter Solve", assembly)
    ok &= rc == 0 and rc2 == 0
    if groups or groups2:
        print("FEHLER: stimmige zweite Fuehrung als Widerspruch gemeldet")
        ok = False
    if not states or not all("geschluckt" in s for s in states + states2):
        print("FEHLER: redundante Bedingungen nicht als 'geschluckt' gefuehrt")
        ok = False
    if not boxB.Placement.isSame(first, 1e-9):
        print("FEHLER: zweiter Solve (mit Gedaechtnis) liefert eine andere Lage")
        ok = False
    for j in slider_joints(doc):
        ok &= check_slider(f"Stimmig {j.Label}", j)

    # 2. Gleiter 2 um 1 mm quer versetzt
    sld2 = slider_joints(doc)[1]
    sld2.Placement2 = sld2.Placement2.multiply(App.Placement(App.Vector(1, 0, 0), App.Rotation()))
    doc.recompute()
    rc, groups, states = solve_and_report("Versetzt 1 mm", assembly)
    if not any("Konflikt" in s for s in states) or len(groups) != 1:
        print("FEHLER: 1 mm Versatz der zweiten Fuehrung nicht als Widerspruch gemeldet")
        ok = False
    elif groups[0].count("'Gleiter") < 2:
        print("FEHLER: Gruppe nennt nicht beide Gleiter")
        ok = False

    # 3. Toleranz 2 mm
    params.SetFloat("RedundancyToleranceLength", 2.0)
    rc, groups, states = solve_and_report("Versetzt 1 mm, Toleranz 2 mm", assembly)
    params.RemFloat("RedundancyToleranceLength")
    if groups or not states or not all("geschluckt" in s for s in states):
        print("FEHLER: Versatz innerhalb der Toleranz nicht geschluckt")
        ok = False

    # 4. Gleiter 1 loeschen: das Gedaechtnis darf keine Bedingung von Gleiter 2 mehr herausnehmen.
    doc.removeObject(slider_joints(doc)[0].Name)
    doc.recompute()
    rc, groups, states = solve_and_report("Gleiter 1 geloescht", assembly)
    ok &= rc == 0
    if groups or states:
        print("FEHLER: nach dem Loeschen noch Redundanz - Gedaechtnis nicht verworfen")
        ok = False
    ok &= check_slider("Gleiter 2 allein", slider_joints(doc)[0])

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
