"""
Regressionstest: Gelenkgrenze im Original geaendert - eingebaute Instanz wird an die neue Grenze
gezogen, sonst Meldung.

Anlass (2026-10-13, Nutzerentscheidung "Aenderung im Original"): Aenderungen im Original werden in
eingebauten Instanzen NICHT nachgezogen (Regel C2) - mit einer Ausnahme: Gelenkgrenzen. Die Instanz
wird beim naechsten Solve in die neue Grenze gezogen; klappt das nicht, gibt es eine Meldung.
OndselSolver verwirft einen unerfuellbaren Grenz-Durchgang still (runDragStep() ->
restorePosRot()), AssemblyObject::reportUnsatisfiedLimits() misst deshalb jede Grenze nach.

Struktur:
    Mid  (eigenes Dok.): BoxA (geerdet) -[Slider, Grenze -50..50]- BoxB bei Gelenkwert 30
    Grand(eigenes Dok.): BoxC (geerdet) -[Fixed]- BoxA-Instanz, Mid flexibel eingebaut
Fall 1: Grenze im Original auf max 10 -> Instanz steht danach bei 10, Original unveraendert, keine
        Meldung.
Fall 2: Abstand Punkt-Punkt BoxC-BoxB-Instanz in Grand haelt den Gelenkwert fest, Grenze im
        Original auf max 0 -> nicht einhaltbar: Meldung (getLimitViolations()), Instanz bleibt.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_nested_limit_change_in_original/test_nested_limit_change_in_original.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_nested_limit_change_in_original/build_fixture.py
"""
import os
import shutil
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
FIXTURES_DIR = os.path.join(THIS_DIR, "fixtures")
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402

TEST_NAME = "test_nested_limit_change_in_original"
SLIDER_PLC1 = jtu.face_jcs_placement("Face1", flip=False, extra_z_rotation_deg=25)  # BoxA
SLIDER_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=10)  # BoxB
START_VALUE = 30.0
LIMIT = (-50.0, 50.0)
OUTER_PLC1 = jtu.face_jcs_placement("Face4", flip=False, extra_z_rotation_deg=90)  # BoxC
OUTER_PLC2 = jtu.face_jcs_placement("Face3", flip=True, extra_z_rotation_deg=40)  # BoxA-Instanz
DIST_PLC1 = jtu.vertex_jcs_placement("Vertex1")  # BoxC
DIST_PLC2 = jtu.vertex_jcs_placement("Vertex7")  # BoxB-Instanz
TOL = 1e-6
DOC_SUFFIXES = ("mid", "grand")


def fixture_paths():
    return {s: os.path.join(FIXTURES_DIR, f"{TEST_NAME}_{s}.FCStd") for s in DOC_SUFFIXES}


def assembly_of(doc):
    return [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]


def slider_value(a, b):
    """Gelenkwert des Sliders; beide Teile im selben Container, dessen Lage kuerzt sich."""
    return jtu.jcs_world(a, SLIDER_PLC1).inverse().multiply(jtu.jcs_world(b, SLIDER_PLC2)).Base.z


def slider_joint(doc):
    return [o for o in doc.Objects if getattr(o, "JointType", None) == "Slider"][0]


def change_limit_in_original(mid_doc, new_max):
    joint = slider_joint(mid_doc)
    joint.LengthMax = new_max
    mid_doc.recompute()
    mid_doc.save()


def main():
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    out = os.path.splitext(base)[0] if base else os.path.join(THIS_DIR, "fcstd-output", "default")
    if os.path.isdir(out):
        shutil.rmtree(out)
    shutil.copytree(FIXTURES_DIR, out)

    ok = True
    grand_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
    grand = assembly_of(grand_doc)
    mid_doc = [d for d in App.listDocuments().values() if d.FileName.endswith(f"{TEST_NAME}_mid.FCStd")][0]
    ntu.remember_originals(grand_doc)
    grand_doc.recompute()
    rc = grand.solve()
    midlink = jtu.get_by_label(grand_doc, "MidLink")
    ia = [o for o in midlink.Group if o.Label == "BoxA"][0]
    ib = [o for o in midlink.Group if o.Label == "BoxB"][0]
    print(f"Start: rc={rc}, Gelenkwert der Instanz {slider_value(ia, ib):.4f} mm")
    if not hasattr(grand, "getLimitViolations"):
        print("FEHLER: AssemblyObject.getLimitViolations() fehlt - Grenzverletzungen werden nicht gemeldet")
        ok = False

    # Fall 1: Grenze enger, einhaltbar.
    change_limit_in_original(mid_doc, 10.0)
    ntu.remember_originals(grand_doc)
    grand_doc.recompute()
    rc = grand.solve()
    value = slider_value(ia, ib)
    violations = grand.getLimitViolations() if hasattr(grand, "getLimitViolations") else []
    print(f"Fall 1 (Grenze max 10): rc={rc}, Gelenkwert der Instanz {value:.4f} mm, Meldungen {violations}")
    if rc != 0 or abs(value - 10.0) > 1e-4:
        print("FEHLER: Instanz wurde nicht an die neue Grenze gezogen")
        ok = False
    if violations:
        print("FEHLER: Meldung, obwohl die Grenze eingehalten wird")
        ok = False
    ok &= ntu.originals_unchanged("Fall 1")

    # Fall 2: Abstand haelt den Gelenkwert fest, Grenze im Original nicht mehr einhaltbar.
    boxC = jtu.get_by_label(grand_doc, "BoxC")
    p_c = boxC.Placement.multVec(DIST_PLC1.Base)
    p_b = midlink.Placement.multiply(ib.Placement).multVec(DIST_PLC2.Base)
    dist = jtu.make_joint(grand, 5, boxC, ib, DIST_PLC1, DIST_PLC2, elem1="Vertex1", elem2="Vertex7")
    dist.Distance = (p_c - p_b).Length
    grand_doc.recompute()
    rc = grand.solve()
    held = slider_value(ia, ib)
    print(f"Fall 2 mit Abstand {float(dist.Distance):.4f} mm: rc={rc}, Gelenkwert {held:.4f} mm")
    change_limit_in_original(mid_doc, 0.0)
    ntu.remember_originals(grand_doc)
    grand_doc.recompute()
    rc = grand.solve()
    value = slider_value(ia, ib)
    violations = grand.getLimitViolations() if hasattr(grand, "getLimitViolations") else []
    print(f"Fall 2 (Grenze max 0): rc={rc}, Gelenkwert der Instanz {value:.4f} mm, Meldungen {violations}")
    if len(violations) != 1 or slider_joint(mid_doc).Name not in violations[0]:
        print("FEHLER: nicht einhaltbare Grenze wurde nicht (genau einmal) gemeldet")
        ok = False
    if abs(value - held) > 1e-4:
        print("FEHLER: Instanz hat sich trotz nicht einhaltbarer Grenze bewegt")
        ok = False
    ok &= ntu.originals_unchanged("Fall 2")

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
