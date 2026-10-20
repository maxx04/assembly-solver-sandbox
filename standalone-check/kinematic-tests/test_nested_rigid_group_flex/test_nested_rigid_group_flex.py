"""
Regressionstest: starre Gruppe (RigidGroupJoint) in einer flexibel eingebauten Unterbaugruppe.

Anlass (2026-10-20, live an BG22 mit BG112 "775 DC Motor"): die starren Gruppen einer flexiblen
Unterbaugruppe kamen im Solver der einbauenden Baugruppe nie an (getRigidGroups() las nur die
eigene Gelenkgruppe). Vom Motor lief beim Ziehen nur das per Fixed angebundene Teil mit, der Rest
blieb stehen. Folgefehler beim Beheben: die innere Erdung der Unterbaugruppe wurde ueber ihre
Gruppe auf alle Mitglieder verteilt (Motor festgefroren), und das Buendeln beim Ziehen riss die
Gruppe auseinander.

Struktur (wie BG112 in BG22):
    Mid   (eigenes Dok.): BoxA (geerdet), BoxB, BoxD - alle drei in EINER starren Gruppe
    Grand (eigenes Dok.): BoxC (geerdet), BoxS -[Slider]- BoxC, MidLink (Mid, flexibel,
                          Container versetzt), BoxS -[Fixed]- BoxB (nicht geerdetes Mitglied)
Geprueft:
    1. nach dem Solve: Gelenke erfuellt, Gruppe in der Anordnung aus Mid, Originale unveraendert
    2. BoxS entlang des Sliders ziehen: BoxA/B/D laufen exakt mit (Lage relativ zu BoxS konstant),
       Gelenke auch mitten im Ziehen erfuellt, Originale unveraendert
    3. Diagramm (exportIdentityGraphDot): genau ein blauer Rahmen um die drei Mitglieder

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_nested_rigid_group_flex/test_nested_rigid_group_flex.py
Fixtur neu bauen:
    ./common/run-test.sh <freecad-install-dir> test_nested_rigid_group_flex/build_fixture.py
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
sys.path.insert(0, os.path.join(THIS_DIR, "..", "test_originals_untouched"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402

TEST_NAME = "test_nested_rigid_group_flex"
B_IN_MID = App.Placement(App.Vector(25, -10, 8), App.Rotation(App.Vector(1, 1, 0), 30))
D_IN_MID = App.Placement(App.Vector(-15, 30, 4), App.Rotation(App.Vector(0, 0, 1), 55))
MID_DROP = App.Placement(App.Vector(60, 20, 15), App.Rotation(App.Vector(0, 1, 0), 20))
SLIDER_PLC1 = jtu.face_jcs_placement("Face6", flip=False)  # BoxC oben
SLIDER_PLC2 = jtu.face_jcs_placement("Face5", flip=True)  # BoxS unten
FIXED_PLC1 = jtu.face_jcs_placement("Face2", flip=False, extra_z_rotation_deg=15)  # BoxS
FIXED_PLC2 = jtu.face_jcs_placement("Face1", flip=True, extra_z_rotation_deg=35)  # BoxB-Instanz
STEPS = 6
STEP_MM = 2.0
DOC_SUFFIXES = ("mid", "grand")
BLUE = "#1971c2"


def fixture_paths():
    return {s: os.path.join(FIXTURES_DIR, f"{TEST_NAME}_{s}.FCStd") for s in DOC_SUFFIXES}


def assembly_of(doc):
    return [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]


def check_joints(label, asm, tmp):
    import test_originals_untouched as tou
    errors, _ = tou.check_joints_in_solver_view(asm, tmp)
    print(f"{label}: {len(errors)} verletzte Gelenke")
    for e in errors:
        print(f"  FEHLER: {e}")
    return not errors


def members(midlink):
    return {lbl: ntu.get_mirror(midlink, lbl) for lbl in ("BoxA", "BoxB", "BoxD")}


def world(midlink, obj):
    return midlink.Placement.multiply(obj.Placement)


def layout_ok(label, midlink, mid_doc, tol=1e-6):
    """Anordnung der Gruppe in Grand (Welt) gleich der in Mid (relativ zu BoxA)."""
    inst = members(midlink)
    orig = {lbl: jtu.get_by_label(mid_doc, lbl) for lbl in inst}
    ok = True
    for lbl in ("BoxB", "BoxD"):
        rel_inst = world(midlink, inst["BoxA"]).inverse().multiply(world(midlink, inst[lbl]))
        rel_orig = orig["BoxA"].Placement.inverse().multiply(orig[lbl].Placement)
        dev = (rel_inst.Base - rel_orig.Base).Length
        same = rel_inst.isSame(rel_orig, tol)
        print(f"{label}: {lbl} relativ zu BoxA, Abweichung {dev:.2e} mm -> {'OK' if same else 'FEHLER'}")
        ok &= same
    return ok


def drag_slider(grand_doc, grand, midlink, tmp):
    boxS = jtu.get_by_label(grand_doc, "BoxS")
    part, mj, ref_name, bypassed = grand.getDragJointOfPart(boxS)
    if mj is None:
        print("FEHLER: kein Bewegungsgelenk fuer BoxS gefunden")
        return False
    drag_parts = [part] + [p for p in grand.getDownstreamParts(part, mj, bypassed) if p != part]
    print(f"Ziehe {part.Label} ueber {mj.Label} ({mj.JointType}), {len(drag_parts)} Teil(e)")
    inst = members(midlink)
    rel_start = {lbl: boxS.Placement.inverse().multiply(world(midlink, o)) for lbl, o in inst.items()}
    axis = boxS.Placement.multiply(mj.Placement2 if ref_name == "Reference2" else mj.Placement1)
    axis = axis.Rotation.multVec(App.Vector(0, 0, 1))
    initial = {p: App.Placement(p.Placement) for p in drag_parts}
    start_s = App.Placement(boxS.Placement)

    ok = True
    grand.preDrag(drag_parts)
    for step in range(1, STEPS + 1):
        for p in drag_parts:
            plc = App.Placement(initial[p])
            plc.Base = plc.Base + axis * (STEP_MM * step)
            p.Placement = plc
        grand.doDragStep()
        moved = (boxS.Placement.Base - start_s.Base).Length
        worst = 0.0
        for lbl, o in inst.items():
            rel = boxS.Placement.inverse().multiply(world(midlink, o))
            worst = max(worst, (rel.Base - rel_start[lbl].Base).Length)
            if not rel.isSame(rel_start[lbl], 1e-6):
                ok = False
        print(f"Schritt {step}: BoxS {moved:.3f} mm gefahren, Gruppe weicht max. {worst:.2e} mm ab")
    if moved < 1e-3:
        print("FEHLER: BoxS hat sich nicht bewegt")
        ok = False
    ok &= check_joints("mitten im Ziehen", grand, tmp)
    grand.postDrag()
    ok &= check_joints("nach dem Ziehen", grand, tmp)
    if not ok:
        print("FEHLER: die starre Gruppe ist beim Ziehen nicht mitgelaufen")
    return ok


def diagram_ok(grand):
    dot = grand.exportIdentityGraphDot()
    blocks = re.findall(r'subgraph cluster_\d+ \{\n(?:(?!subgraph)[^}])*?color="' + BLUE + r'";(.*?)\n  \}', dot, re.S)
    print(f"Diagramm: {len(blocks)} blaue(r) Rahmen")
    if len(blocks) != 1:
        print("FEHLER: erwartet genau einen blauen Rahmen (starre Gruppe)")
        return False
    labels = [lbl for lbl in ("BoxA", "BoxB", "BoxD") if lbl in blocks[0]]
    others = [lbl for lbl in ("BoxC", "BoxS") if lbl in blocks[0]]
    print(f"Diagramm: im blauen Rahmen {labels}, faelschlich {others}")
    return len(labels) == 3 and not others


def main():
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    out = os.path.splitext(base)[0] if base else os.path.join(THIS_DIR, "fcstd-output", "default")
    if os.path.isdir(out):
        shutil.rmtree(out)
    shutil.copytree(FIXTURES_DIR, out)

    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        grand_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_grand.FCStd"))
        mid_doc = App.getDocument(f"{TEST_NAME}_mid") if f"{TEST_NAME}_mid" in App.listDocuments() else None
        if mid_doc is None:
            mid_doc = App.openDocument(os.path.join(out, f"{TEST_NAME}_mid.FCStd"))
        ntu.remember_originals(grand_doc)
        grand = assembly_of(grand_doc)
        grand_doc.recompute()
        rc = grand.solve()
        print(f"Solve rc={rc}")
        ok &= rc == 0
        midlink = jtu.get_by_label(grand_doc, "MidLink")
        ok &= check_joints("nach dem Solve", grand, tmp)
        ok &= layout_ok("nach dem Solve", midlink, mid_doc)
        ok &= ntu.originals_unchanged("nach dem Solve")

        ok &= drag_slider(grand_doc, grand, midlink, tmp)
        ok &= layout_ok("nach dem Ziehen", midlink, mid_doc)
        ok &= ntu.originals_unchanged("nach dem Ziehen")

        ok &= diagram_ok(grand)

    if ok:
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (siehe Ausgabe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
