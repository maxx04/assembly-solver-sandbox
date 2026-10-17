"""
Regressionstest: die Redundanz-Warnung nennt den eigentlichen Verursacher und die
ueberbestimmte Gruppe, nicht ein beliebiges Gelenk.

Anlass (2026-10-11, PROJ_CNC3018): in BG22 ueberbestimmte eine Parallel-Verbindung die Drehung um
eine Gewindestange (Parallel sperrt zwei Drehungen, nur eine war frei). OndselSolver entnimmt die
abhaengige Gleichung je nach Pivotreihenfolge irgendeinem Gelenk der Schleife; gemeldet wurde ein
Gelenk einer anderen Fuehrungsbaugruppe. Jetzt berechnet PosICNewtonRaphson die minimale
abhaengige Gruppe (System::redundancyGroups), AssemblyObject nennt das zuletzt angelegte Gelenk
dieser Baugruppe darin als Verursacher (getRedundancyGroups()).

Aufbau: BoxA (geerdet) -[Revolute]- BoxB, dazu als LETZTES Gelenk Parallel BoxB-BoxA mit
Achsen senkrecht zur Drehachse (sperrt die Drehung, eine Gleichung zu viel: das Kippen sperrt die
Revolute schon). Unbeteiligt: BoxA -[Slider]- BoxC -[Fixed]- BoxD.
Erwartet (Regel C7, 2026-10-14): die stimmige Redundanz wird geschluckt (keine Gruppe, Bedingung
"geschluckt"). Wird die Parallel-Achse um 3 Grad verkippt, ist es ein Widerspruch: genau eine Gruppe
mit "Parallel" und "Dreh", ohne "Gleit"/"Starr".

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_redundancy_group_flat/test_redundancy_group_flat.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_redundancy_group_flat/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

TEST_NAME = "test_redundancy_group_flat"
BOXES = ("BoxA", "BoxB", "BoxC", "BoxD")
REV_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=20)  # BoxA
REV_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=5)  # BoxB
TILT = App.Placement(App.Vector(), App.Rotation(App.Vector(0, 1, 0), 90))  # z -> x
PAR_PLC1 = REV_PLC2.multiply(TILT)  # BoxB, Achse senkrecht zur Drehachse
PAR_PLC2 = REV_PLC1.multiply(TILT)  # BoxA, dieselbe Weltrichtung
SLD_PLC1 = jtu.face_jcs_placement("Face4", flip=False, extra_z_rotation_deg=10)  # BoxA
SLD_PLC2 = jtu.face_jcs_placement("Face3", flip=True, extra_z_rotation_deg=0)  # BoxC
FIX_PLC1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=30)  # BoxC
FIX_PLC2 = jtu.face_jcs_placement("Face5", flip=True, extra_z_rotation_deg=0)  # BoxD


def check_groups(groups, expect_conflict):
    ok = True
    if not expect_conflict:
        if groups:
            print("FEHLER: stimmige Redundanz wurde als Widerspruch gemeldet")
            ok = False
        return ok
    if len(groups) != 1:
        print(f"FEHLER: erwartet genau eine widerspruechliche Gruppe, gefunden {len(groups)}")
        return False
    if "'Parallel'" not in groups[0] or "'Dreh'" not in groups[0]:
        print("FEHLER: Parallel-Verbindung oder Revolute fehlt in der Gruppe")
        ok = False
    if "'Gleit'" in groups[0] or "'Starr'" in groups[0]:
        print("FEHLER: unbeteiligte Gelenke in der Gruppe")
        ok = False
    return ok


def main():
    fixture_file = jtu.fixture_path(THIS_DIR)
    out_path = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD", "/tmp/kinematic_test_redundancy_group.FCStd")
    jtu.copy_fixture_to_output(fixture_file, out_path, box_labels=BOXES)
    doc = App.openDocument(out_path)
    assembly = doc.getObject("Assembly")
    doc.recompute()
    ok = True

    # Fall 1 (Regel C7): die Redundanz ist stimmig -> geschluckt, keine Gruppe.
    rc = assembly.solve()
    groups = assembly.getRedundancyGroups()
    states = assembly.getRedundancyStates()
    print(f"Stimmig: rc={rc}, Gruppen {groups}, Bedingungen {states}")
    ok &= rc == 0
    if not states or not all("geschluckt" in s for s in states):
        print("FEHLER: stimmige redundante Bedingung nicht als 'geschluckt' gefuehrt")
        ok = False
    ok &= check_groups(groups, expect_conflict=False)

    # Fall 2: Parallel-Achse um 3 Grad zur Drehachse hin verkippt (um die JCS-y-Achse; die x-Achse ist
    # die Drehachse, eine Kippung darum gleicht die Drehung aus) -> Widerspruch.
    par = [j for j in doc.Objects if getattr(j, "JointType", None) == "Parallel"][0]
    par.Placement1 = par.Placement1.multiply(App.Placement(App.Vector(), App.Rotation(App.Vector(0, 1, 0), 3)))
    doc.recompute()
    rc = assembly.solve()
    groups = assembly.getRedundancyGroups()
    states = assembly.getRedundancyStates()
    print(f"Verkippt: rc={rc}, Gruppen:")
    for g in groups:
        print(f"  {g}")
    print(f"  Bedingungen {states}")
    if not any("Konflikt" in s for s in states):
        print("FEHLER: Widerspruch nicht als 'Konflikt' erkannt")
        ok = False
    ok &= check_groups(groups, expect_conflict=True)

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
