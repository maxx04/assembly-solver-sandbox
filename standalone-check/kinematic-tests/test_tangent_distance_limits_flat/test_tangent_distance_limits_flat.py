"""
Tangent-Gelenk: Abstand-Feld und Grenzen entlang der Kurve (TODO 3+4, Nutzer 2026-11-22).

Abstand (Eigenschaft Distance), je ein frischer Solve, per OCC bzw. Projektion gemessen:
  1. Punkt ueber der gewoelbten Flaeche, Abstand 3   -> 3 mm entlang der Normalen, Startseite
  2. Rad ueber der gewoelbten Flaeche, Abstand 2     -> kleinster Abstand 2, Startseite
  3. Rad ueber der Quader-Deckflaeche, Abstand 1.5   -> kleinster Abstand 1.5
  4. Rad neben der ebenen Wellenkurve, Abstand 2     -> kleinster Abstand 2, Startseite
  5. Punkt neben der ebenen Wellenkurve, Abstand 4   -> 4 mm in der Kurvenebene, Startseite
  6. Punkt neben einer Raumkurve, Abstand 2.5        -> Rohr: 2.5 mm von der Kurve
Grenzen (Punkt auf der Wellenkurve, LengthMin 30 / LengthMax 70, Weg = Bogenlaenge ab Anfang):
  7. Start bei 100 -> 70, Start bei 10 -> 30, Start bei 50 -> bleibt (Solve)
  8. Ziehen weit ueber beide Grenzen hinaus -> 70 bzw. 30
  9. Rad ueber Bump mit Abstand 2 speichern, neu laden, Recompute -> Abstand bleibt 2

Vanilla kennt den Gelenktyp nicht, dort ist FAIL erwartet.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_tangent_distance_limits_flat/test_tangent_distance_limits_flat.py
Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import os
import sys

import FreeCAD as App
import Part

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "test_tangent_surface_curve_flat"))
import joint_test_utils as jtu  # noqa: E402
import test_tangent_surface_curve_flat as base  # noqa: E402

TEST_NAME = "test_tangent_distance_limits_flat"
TOL = 1e-5
V = App.Vector
GP = base.GROUND_PLACEMENT


def space_curve():
    poles = [V(-40, 0, 0), V(-20, 15, 10), V(0, -10, 20), V(20, 15, 5), V(40, 0, -10)]
    curve = Part.BSplineCurve()
    curve.buildFromPoles(poles, False, 3)
    return curve.toShape()


def build(out_path, ground_kind, mover_kind, distance=0.0, limits=None):
    doc = App.newDocument(TEST_NAME)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    doc.saveAs(out_path)
    ground = doc.addObject("Part::Feature", ground_kind)
    ground.Label = ground_kind
    ground.Shape = {
        "Bump": base.make_bump_shape,
        "Plate": lambda: Part.makeBox(100, 100, 5, V(-50, -50, -5)),
        "Track": base.make_track_shape,
        "Space": space_curve,
    }[ground_kind]()
    ground.Placement = GP
    mover = doc.addObject("Part::Feature", "Mover")
    mover.Label = "Wheel" if mover_kind == "wheel" else "Mover"
    if mover_kind == "wheel":
        mover.Shape = Part.makeCircle(base.WHEEL_RADIUS)
        mover_side = ((mover, ["Edge1", "Edge1"]), base.WHEEL_JCS)
    else:
        mover.Shape = Part.Vertex(V(0, 0, 0))
        mover_side = ((mover, ["Vertex1", "Vertex1"]), App.Placement())
    doc.recompute()
    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    assembly.addObject(ground)
    assembly.addObject(mover)
    doc.recompute()
    jtu.ground_object(ground)

    import JointObject
    import UtilsAssembly

    joint = UtilsAssembly.getJointGroup(assembly).newObject("App::FeaturePython", "Joint")
    JointObject.Joint(joint, JointObject.JointTypes.index("Tangent"))
    if App.GuiUp:
        JointObject.ViewProviderJoint(joint.ViewObject)
    element = {"Bump": "Face1", "Plate": "Face6", "Track": "Edge1", "Space": "Edge1"}[ground_kind]
    joint.Reference1 = (ground, [element, element])
    joint.Reference2 = mover_side[0]
    joint.Detach1 = True
    joint.Detach2 = True
    joint.Placement1 = base.GROUND_JCS
    joint.Placement2 = mover_side[1]
    joint.Distance = distance
    if limits:
        joint.EnableLengthMin = True
        joint.EnableLengthMax = True
        joint.LengthMin = limits[0]
        joint.LengthMax = limits[1]
    doc.recompute()
    return doc, assembly, ground, mover, joint


def element_shape(ground):
    return ground.Shape.getElement("Face6" if ground.Label == "Plate" else
                                   "Face1" if ground.Label == "Bump" else "Edge1")


def point_surface_signed(ground, point):
    face = element_shape(ground)
    u, v = face.Surface.parameter(point)
    foot = face.Surface.value(u, v)
    return (point - foot).dot(face.Surface.normal(u, v))


def plane_normal():
    return GP.Rotation.multVec(V(0, 0, 1))


def track_in_plane(ground, point):
    """Abstand zur ebenen Kurve in der Ebene (mit Vorzeichen) und Abstand zur Ebene."""
    edge = element_shape(ground)
    u = edge.Curve.parameter(point)
    foot = edge.Curve.value(u)
    tangent = edge.Curve.tangent(u)[0]
    n = plane_normal()
    return (point - foot).dot(n.cross(tangent)), (point - foot).dot(n)


def arc_position(ground, point):
    edge = element_shape(ground)
    u0, u1 = edge.FirstParameter, edge.LastParameter
    u = min(max(edge.Curve.parameter(point), u0), u1)
    return edge.Curve.length(u0, u) if u > u0 else 0.0


def point_at_arc(ground, s, offset):
    edge = element_shape(ground)
    u = edge.getParameterByLength(s)
    return edge.valueAt(u) + offset


def report(name, ok, text):
    print(f"[{name}] {text} -> {'OK' if ok else 'FEHLER'}")
    return ok


def distance_cases(path):
    results = []
    # 1. Punkt ueber Bump
    doc, asm, ground, mover, _ = build(path("p_bump"), "Bump", "vertex", 3.0)
    mover.Placement = App.Placement(GP.multVec(V(-12, 9, 25)), App.Rotation())
    rc = asm.solve()
    d = point_surface_signed(ground, mover.Placement.Base)
    results.append(report("Punkt/Bump d=3", rc == 0 and abs(d - 3.0) < TOL, f"rc={rc}, Abstand {d:.6f}"))
    App.closeDocument(doc.Name)
    # 2./3. Rad ueber Bump bzw. Plate
    for name, kind, dist, start in (
        ("Rad/Bump d=2", "Bump", 2.0, base.world(V(-15, 10, 30), angle=70)),
        ("Rad/Plate d=1.5", "Plate", 1.5, base.world(V(5, 8, 20), angle=80)),
    ):
        doc, asm, ground, mover, _ = build(path(name[:8].replace("/", "_")), kind, "wheel", dist)
        mover.Placement = start
        side0 = base.side_of(ground, mover.Placement.Base)
        rc = asm.solve()
        d = base.wheel_distance(ground, mover)
        side = base.side_of(ground, mover.Placement.Base)
        results.append(report(name, rc == 0 and abs(d - dist) < TOL and side == side0,
                              f"rc={rc}, kleinster Abstand {d:.6f}, Seite {side0:+d}->{side:+d}"))
        App.closeDocument(doc.Name)
    # 4. Rad neben Track
    doc, asm, ground, mover, _ = build(path("w_track"), "Track", "wheel", 2.0)
    mover.Placement = base.world(V(-5, 20, 0), angle=0)
    side0 = base.side_of(ground, mover.Placement.Base)
    rc = asm.solve()
    d = element_shape(ground).distToShape(mover.Shape)[0]
    side = base.side_of(ground, mover.Placement.Base)
    results.append(report("Rad/Track d=2", rc == 0 and abs(d - 2.0) < TOL and side == side0,
                          f"rc={rc}, kleinster Abstand {d:.6f}, Seite {side0:+d}->{side:+d}"))
    App.closeDocument(doc.Name)
    # 5. Punkt neben Track (eben): in der Ebene, Startseite
    doc, asm, ground, mover, _ = build(path("p_track"), "Track", "vertex", 4.0)
    mover.Placement = App.Placement(GP.multVec(V(-10, -9, 3)), App.Rotation())
    s0, _ = track_in_plane(ground, mover.Placement.Base)
    rc = asm.solve()
    s, off_plane = track_in_plane(ground, mover.Placement.Base)
    results.append(report("Punkt/Track d=4", rc == 0 and abs(abs(s) - 4.0) < TOL and s * s0 > 0
                          and abs(off_plane) < TOL,
                          f"rc={rc}, in der Ebene {s:.6f} (Start {s0:+.2f}), aus der Ebene {off_plane:.2e}"))
    App.closeDocument(doc.Name)
    # 6. Punkt neben Raumkurve: Rohr
    doc, asm, ground, mover, _ = build(path("p_space"), "Space", "vertex", 2.5)
    mover.Placement = App.Placement(GP.multVec(V(5, 4, 15)), App.Rotation())
    rc = asm.solve()
    d = element_shape(ground).distToShape(Part.Vertex(mover.Placement.Base))[0]
    results.append(report("Punkt/Raumkurve d=2.5", rc == 0 and abs(d - 2.5) < TOL,
                          f"rc={rc}, Abstand {d:.6f}"))
    App.closeDocument(doc.Name)
    return results


def limit_cases(path):
    results = []
    lo, hi = 30.0, 70.0
    offset = GP.Rotation.multVec(V(0, 0, 2))  # Start etwas neben der Kurve
    for start_s, expect in ((100.0, hi), (10.0, lo), (50.0, None)):
        doc, asm, ground, mover, _ = build(path(f"lim_{int(start_s)}"), "Track", "vertex", limits=(lo, hi))
        mover.Placement = App.Placement(point_at_arc(ground, start_s, offset), App.Rotation())
        rc = asm.solve()
        p = mover.Placement.Base
        s = arc_position(ground, p)
        on = element_shape(ground).distToShape(Part.Vertex(p))[0]
        want = expect if expect is not None else start_s
        tol = 1e-5 if expect is not None else 0.5  # ohne Grenze: Fusspunkt, nicht exakt start_s
        results.append(report(f"Grenzen, Start bei {start_s:g}", rc == 0 and abs(s - want) < tol and on < TOL,
                              f"rc={rc}, Weg {s:.6f} (erwartet {want:g}), auf der Kurve {on:.2e}"))
        App.closeDocument(doc.Name)
    # Ziehen ueber beide Grenzen
    doc, asm, ground, mover, _ = build(path("lim_drag"), "Track", "vertex", limits=(lo, hi))
    mover.Placement = App.Placement(point_at_arc(ground, 50.0, V()), App.Rotation())
    asm.solve()
    for target_s, expect in ((140.0, hi), (-20.0, lo)):
        start = App.Placement(mover.Placement)
        length = element_shape(ground).Length
        forward = target_s > 100
        # weit hinter das jeweilige Kurvenende, entlang der Kurvenachse (Track-x)
        target = point_at_arc(ground, length if forward else 0.0, V()) \
            + GP.Rotation.multVec(V(80, 0, 0) if forward else V(-80, 0, 0))
        asm.preDrag([mover])
        for k in range(1, 41):
            plc = App.Placement(start)
            plc.Base = start.Base + (target - start.Base) * (k / 40)
            mover.Placement = plc
            asm.doDragStep()
        asm.postDrag()
        p = mover.Placement.Base
        s = arc_position(ground, p)
        on = element_shape(ground).distToShape(Part.Vertex(p))[0]
        results.append(report(f"Ziehen Richtung {'Ende' if forward else 'Anfang'}",
                              abs(s - expect) < 1e-5 and on < TOL,
                              f"Weg {s:.6f} (erwartet {expect:g}), auf der Kurve {on:.2e}"))
    App.closeDocument(doc.Name)
    return results


def reload_case(out_path):
    doc, asm, ground, mover, _ = build(out_path, "Bump", "wheel", 2.0)
    mover.Placement = base.world(V(-15, 10, 30), angle=70)
    asm.solve()
    doc.recompute()
    d0 = base.wheel_distance(ground, mover)
    doc2, wheel2, exc = jtu.save_close_reopen_recompute(doc, out_path, box_b_name="Wheel")
    if exc is not None:
        return [report("Neuladen", False, f"Exception {exc!r}")]
    bump2 = jtu.get_by_label(doc2, "Bump")
    d = base.wheel_distance(bump2, wheel2)
    App.closeDocument(doc2.Name)
    return [report("Neuladen Rad/Bump d=2", abs(d0 - 2.0) < TOL and abs(d - 2.0) < TOL,
                   f"vorher {d0:.6f}, nach Neuladen {d:.6f}")]


def main():
    out_path = os.environ.get(
        "KINEMATIC_TEST_OUTPUT_FCSTD", f"/tmp/kinematic-tests/{TEST_NAME}/{TEST_NAME}.FCStd"
    )
    base_dir = os.path.dirname(out_path)

    def path(suffix):
        return os.path.join(base_dir, f"{TEST_NAME}_{suffix}.FCStd")

    results = distance_cases(path) + limit_cases(path) + reload_case(out_path)
    if all(results):
        print("RESULT: PASS")
        return 0
    print(f"RESULT: FAIL ({results.count(False)} von {len(results)} Faellen, siehe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
