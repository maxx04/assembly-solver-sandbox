"""
Tangent-Gelenk, erster Schritt "Vertex auf Kurve" (plans/2026-10-16-tangent-joint.md), flach.

Aufbau: "Rail" (Part::Feature mit einer offenen, raeumlichen B-Spline-Kante, eigene Placement
gedreht/verschoben, geerdet) und BoxB (App::Link auf die externe Koerperdatei). Tangent-Gelenk
zwischen Rail.Edge1 und BoxB.Vertex2. Zwangsbedingung (solver-unabhaengig pruefbar): der
Weltpunkt von BoxB.Vertex2 liegt auf der Kante (OCC distToShape), die Drehung von BoxB ist frei.

Faelle (je ein frischer Solve aus einer anderen Startlage von BoxB):
  1. Punkt neben der Kurve           -> danach auf der Kurve
  2. Referenzen vertauscht (Vertex als Reference1) -> wird beim Solve getauscht, wieder auf der Kurve
  3. Punkt weit hinter dem Kurvenende -> bleibt am Endpunkt stehen (Nutzerentscheidung 2026-10-16)
  4. Speichern + Neuladen + Recompute -> weiterhin auf der Kurve

Abweichung von der Fixture-Methodik (joint_test_utils.py): das Modell wird hier bei jedem Lauf
gebaut, weil die Fixture in der Cloud-Session ohne FreeCAD nicht erzeugt werden konnte. Vanilla
kennt den Gelenktyp nicht, dort ist FAIL erwartet.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_tangent_vertex_spline_flat/test_tangent_vertex_spline_flat.py
Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import os
import sys

import FreeCAD as App
import Part

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu

TEST_NAME = "test_tangent_vertex_spline_flat"
JOINT_TYPE_INDEX = 13  # JointObject.JointTypes[13] == "Tangent"
TOL_ON_CURVE = 1e-6
TOL_AT_END = 1e-5

RAIL_PLACEMENT = App.Placement(App.Vector(20, -5, 3), App.Rotation(App.Vector(1, 2, 3), 35))
# Beliebiges JCS auf der Kante: die Kurve wird in dessen Koordinaten an den Solver gegeben.
RAIL_JCS = App.Placement(App.Vector(4, 1, -2), App.Rotation(App.Vector(0, 1, 1), 20))
VERTEX_JCS = jtu.vertex_jcs_placement("Vertex2")


def make_rail_shape():
    poles = [
        App.Vector(0, 0, 0),
        App.Vector(15, 20, 5),
        App.Vector(30, -20, 10),
        App.Vector(45, 15, 0),
        App.Vector(60, 0, 8),
    ]
    curve = Part.BSplineCurve()
    curve.buildFromPoles(poles, False, 3)
    return curve.toShape()


def build_model(out_path, swap_refs=False):
    doc = App.newDocument(TEST_NAME)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    doc.saveAs(out_path)

    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(out_path), ["BoxB"])
    boxB = jtu.link_external_box(doc, "BoxB", box_paths["BoxB"])

    rail = doc.addObject("Part::Feature", "Rail")
    rail.Label = "Rail"
    rail.Shape = make_rail_shape()
    rail.Placement = RAIL_PLACEMENT
    doc.recompute()

    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    assembly.addObject(rail)
    assembly.addObject(boxB)
    doc.recompute()
    jtu.ground_object(rail)

    import JointObject
    import UtilsAssembly

    joint_group = UtilsAssembly.getJointGroup(assembly)
    joint = joint_group.newObject("App::FeaturePython", "Joint")
    JointObject.Joint(joint, JOINT_TYPE_INDEX)
    if App.GuiUp:
        JointObject.ViewProviderJoint(joint.ViewObject)
    edge_side = ((rail, ["Edge1", "Edge1"]), RAIL_JCS)
    vertex_side = ((boxB, ["Vertex2", "Vertex2"]), VERTEX_JCS)
    first, second = (vertex_side, edge_side) if swap_refs else (edge_side, vertex_side)
    joint.Reference1 = first[0]
    joint.Reference2 = second[0]
    joint.Detach1 = True
    joint.Detach2 = True
    joint.Placement1 = first[1]
    joint.Placement2 = second[1]
    doc.recompute()
    return doc, assembly, rail, boxB, joint


def vertex_world(boxB):
    return boxB.Placement.multVec(VERTEX_JCS.Base)


def distance_to_rail(rail, point):
    return rail.Shape.Edges[0].distToShape(Part.Vertex(point))[0]


def rail_end_world(rail):
    return rail.Shape.Edges[0].Vertexes[-1].Point


def run_case(name, out_path, start_base, swap_refs=False, expect_end=False):
    doc, assembly, rail, boxB, joint = build_model(out_path, swap_refs)
    boxB.Placement = App.Placement(start_base, App.Rotation(App.Vector(1, 0, 1), 25))
    doc.recompute()
    assembly.solve(False)
    doc.recompute()

    p = vertex_world(boxB)
    d = distance_to_rail(rail, p)
    ok = d < TOL_ON_CURVE
    print(f"[{name}] Abstand Vertex-Kurve: {d:.3e} -> {'OK' if ok else 'FEHLER'}")
    if swap_refs:
        swapped = joint.Reference1[0] == rail
        print(f"[{name}] Referenzen getauscht (Kante ist Reference1): {swapped}")
        ok = ok and swapped
    if expect_end:
        d_end = (p - rail_end_world(rail)).Length
        ok_end = d_end < TOL_AT_END
        print(f"[{name}] Abstand zum Kurvenende: {d_end:.3e} -> {'OK' if ok_end else 'FEHLER'}")
        ok = ok and ok_end
    return ok, doc, rail


def main():
    out_path = os.environ.get(
        "KINEMATIC_TEST_OUTPUT_FCSTD", f"/tmp/kinematic-tests/{TEST_NAME}/{TEST_NAME}.FCStd"
    )
    base_dir = os.path.dirname(out_path)

    results = []
    near = RAIL_PLACEMENT.multVec(App.Vector(25, 4, 12))
    ok, doc, rail = run_case("neben der Kurve", out_path, near)
    results.append(ok)

    doc2, boxB2, exc = jtu.save_close_reopen_recompute(doc, out_path)
    if exc is not None:
        print(f"RESULT: FAIL (Exception beim Neuladen+Recompute: {exc!r})")
        return 1
    rail2 = jtu.get_by_label(doc2, "Rail")
    d_reload = distance_to_rail(rail2, vertex_world(boxB2))
    ok_reload = d_reload < TOL_ON_CURVE
    print(f"[nach Neuladen] Abstand Vertex-Kurve: {d_reload:.3e} -> {'OK' if ok_reload else 'FEHLER'}")
    results.append(ok_reload)
    App.closeDocument(doc2.Name)

    ok, doc, _ = run_case(
        "Referenzen vertauscht", os.path.join(base_dir, f"{TEST_NAME}_swap.FCStd"), near, swap_refs=True
    )
    results.append(ok)
    App.closeDocument(doc.Name)

    beyond = RAIL_PLACEMENT.multVec(App.Vector(90, 0, 8))
    ok, doc, _ = run_case(
        "hinter dem Kurvenende", os.path.join(base_dir, f"{TEST_NAME}_end.FCStd"), beyond, expect_end=True
    )
    results.append(ok)
    App.closeDocument(doc.Name)

    if all(results):
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (Tangent-Zwangsbedingung verletzt, siehe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
