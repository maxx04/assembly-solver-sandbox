"""
Tangent-Gelenk, zweiter Schritt: Vertex auf Flaeche, Kante an Flaeche, Kante an Kante
(plans/2026-10-16-tangent-joint.md), flach.

Geometrie (alle Part::Feature mit eigener, gedrehter/verschobener Placement):
  - "Bump":  geerdete bikubische B-Spline-Flaeche ueber 120 x 120, in der Mitte erhoeht
  - "Plate": geerdeter Quader 100 x 100 x 5, Deckflaeche Face6 (Ebene, beim Auslesen auf die
             Flaeche beschnitten)
  - "Track": geerdete ebene Wellenkurve (offene B-Spline)
  - "Wheel": frei, ein Kreis mit Radius 5 (Kante Edge1)
  - BoxB:    frei, App::Link auf die externe Koerperdatei (Vertex2)

Zwangsbedingungen, solver-unabhaengig per OCC gemessen:
  - Vertex auf Flaeche:  Abstand Vertex-Flaeche ~ 0
  - Kante an Flaeche:    kleinster Abstand Kreis-Flaeche ~ 0, Kreismitte bleibt auf ihrer
                         Startseite (keine Durchdringung)
  - Kante an Kante:      kleinster Abstand Kreis-Kurve ~ 0, Kreismitte bleibt auf ihrer Seite

Faelle (je ein frischer Solve):
  1. Vertex ueber der gewoelbten Flaeche          -> auf der Flaeche
  2. Rad ueber der gewoelbten Flaeche (schraeg)   -> beruehrt von oben
  3. Rad unter der gewoelbten Flaeche             -> beruehrt von unten
  4. Rad ueber der Deckflaeche des Quaders        -> beruehrt von oben
  5. Rad neben der Wellenkurve (gleiche Ebene)    -> beruehrt von seiner Seite
  6. Referenzen vertauscht (Flaeche als Reference2) -> beruehrt, Gelenk bleibt unveraendert
  7. Fall 2 speichern + neu laden + Recompute     -> beruehrt weiterhin

Wie test_tangent_vertex_spline_flat wird das Modell bei jedem Lauf gebaut. Vanilla kennt den
Gelenktyp nicht, dort ist FAIL erwartet.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_tangent_surface_curve_flat/test_tangent_surface_curve_flat.py
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

TEST_NAME = "test_tangent_surface_curve_flat"
JOINT_TYPE_INDEX = 13  # JointObject.JointTypes[13] == "Tangent"
TOL_CONTACT = 1e-5
WHEEL_RADIUS = 5.0

GROUND_PLACEMENT = App.Placement(App.Vector(20, -5, 3), App.Rotation(App.Vector(1, 2, 3), 35))
# Beliebige JCS: die Geometrie geht in deren Koordinaten an den Solver.
GROUND_JCS = App.Placement(App.Vector(4, 1, -2), App.Rotation(App.Vector(0, 1, 1), 20))
WHEEL_JCS = App.Placement(App.Vector(1, -2, 0.5), App.Rotation(App.Vector(1, 0, 0), 30))
VERTEX_JCS = jtu.vertex_jcs_placement("Vertex2")


def make_bump_shape():
    z = [[0, 0, 0, 0], [0, 12, 8, 0], [0, 8, 12, 0], [0, 0, 0, 0]]
    poles = [[App.Vector(-60 + 40 * i, -60 + 40 * j, z[i][j]) for j in range(4)] for i in range(4)]
    surface = Part.BSplineSurface()
    surface.buildFromPolesMultsKnots(poles, [4, 4], [4, 4], [0, 1], [0, 1], False, False, 3, 3)
    return surface.toShape()


def make_track_shape():
    poles = [
        App.Vector(-60, 0, 0),
        App.Vector(-30, 12, 0),
        App.Vector(0, -12, 0),
        App.Vector(30, 12, 0),
        App.Vector(60, 0, 0),
    ]
    curve = Part.BSplineCurve()
    curve.buildFromPoles(poles, False, 3)
    return curve.toShape()


def make_ground(doc, name):
    obj = doc.addObject("Part::Feature", name)
    obj.Label = name
    if name == "Bump":
        obj.Shape = make_bump_shape()
    elif name == "Plate":
        obj.Shape = Part.makeBox(100, 100, 5, App.Vector(-50, -50, -5))
    else:
        obj.Shape = make_track_shape()
    obj.Placement = GROUND_PLACEMENT
    return obj


def ground_element(name):
    return {"Bump": "Face1", "Plate": "Face6", "Track": "Edge1"}[name]


def build_model(out_path, ground_name, mover_kind, swap_refs=False):
    doc = App.newDocument(TEST_NAME)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    doc.saveAs(out_path)

    ground = make_ground(doc, ground_name)
    if mover_kind == "vertex":
        box_paths = jtu.ensure_box_bodies_in(os.path.dirname(out_path), ["BoxB"])
        mover = jtu.link_external_box(doc, "BoxB", box_paths["BoxB"])
        mover_side = ((mover, ["Vertex2", "Vertex2"]), VERTEX_JCS)
    else:
        mover = doc.addObject("Part::Feature", "Wheel")
        mover.Label = "Wheel"
        mover.Shape = Part.makeCircle(WHEEL_RADIUS)
        mover_side = ((mover, ["Edge1", "Edge1"]), WHEEL_JCS)
    doc.recompute()

    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    assembly.addObject(ground)
    assembly.addObject(mover)
    doc.recompute()
    jtu.ground_object(ground)

    import JointObject
    import UtilsAssembly

    joint_group = UtilsAssembly.getJointGroup(assembly)
    joint = joint_group.newObject("App::FeaturePython", "Joint")
    JointObject.Joint(joint, JOINT_TYPE_INDEX)
    if App.GuiUp:
        JointObject.ViewProviderJoint(joint.ViewObject)
    element = ground_element(ground_name)
    ground_side = ((ground, [element, element]), GROUND_JCS)
    first, second = (mover_side, ground_side) if swap_refs else (ground_side, mover_side)
    joint.Reference1 = first[0]
    joint.Reference2 = second[0]
    joint.Detach1 = True
    joint.Detach2 = True
    joint.Placement1 = first[1]
    joint.Placement2 = second[1]
    doc.recompute()
    return doc, assembly, ground, mover, joint


def ground_shape(ground):
    name = ground.Label
    element = ground_element(name)
    shape = ground.Shape.getElement(element)
    return shape


def side_of(ground, point):
    """+1/-1: auf welcher Seite von Flaeche/Kurve der Punkt liegt (Welt)."""
    shape = ground_shape(ground)
    if isinstance(shape, Part.Face):
        u, v = shape.Surface.parameter(point)
        foot = shape.Surface.value(u, v)
        normal = shape.Surface.normal(u, v)
        return 1 if (point - foot).dot(normal) > 0 else -1
    # Ebene Kurve: Seite ueber die Normale in der Kurvenebene.
    plane_normal = ground.Placement.Rotation.multVec(App.Vector(0, 0, 1))
    curve = shape.Curve
    t = curve.parameter(point)
    tangent = curve.tangent(t)[0]
    in_plane = plane_normal.cross(tangent)
    return 1 if (point - curve.value(t)).dot(in_plane) > 0 else -1


def wheel_distance(ground, wheel):
    """Kleinster Abstand Rad-Geometrie. Gegen eine Flaeche ueber Projektion (Abtastung +
    Verfeinerung): OCCs distToShape rechnet Kante gegen Flaeche nur auf ~1e-7..1e-4 genau
    (2026-11-21 nachgemessen - das war die vermeintliche Ungenauigkeit unter der Beule)."""
    shape = ground_shape(ground)
    if not isinstance(shape, Part.Face):
        return shape.distToShape(wheel.Shape)[0]
    edge = wheel.Shape.Edges[0]
    surface = shape.Surface

    def g(t):
        p = edge.valueAt(t)
        u, v = surface.parameter(p)
        return (p - surface.value(u, v)).Length

    a, b = edge.ParameterRange
    n = 720
    ts = [a + (b - a) * i / n for i in range(n + 1)]
    k = min(range(len(ts)), key=lambda i: g(ts[i]))
    lo, hi = ts[max(k - 1, 0)], ts[min(k + 1, n)]
    for _ in range(80):
        m1, m2 = lo + (hi - lo) / 3, hi - (hi - lo) / 3
        if g(m1) < g(m2):
            hi = m2
        else:
            lo = m1
    return g((lo + hi) / 2)


def wheel_center(wheel):
    return wheel.Placement.Base


def run_case(name, out_path, ground_name, mover_kind, start, swap_refs=False):
    doc, assembly, ground, mover, joint = build_model(out_path, ground_name, mover_kind, swap_refs)
    mover.Placement = start
    doc.recompute()
    start_side = side_of(ground, wheel_center(mover)) if mover_kind == "wheel" else None
    assembly.solve(False)
    doc.recompute()

    shape = ground_shape(ground)
    if mover_kind == "vertex":
        point = mover.Placement.multVec(VERTEX_JCS.Base)
        d = shape.distToShape(Part.Vertex(point))[0]
        ok = d < TOL_CONTACT
        print(f"[{name}] Abstand Vertex-Flaeche: {d:.3e} -> {'OK' if ok else 'FEHLER'}")
    else:
        d = wheel_distance(ground, mover)
        ok_touch = d < TOL_CONTACT
        end_side = side_of(ground, wheel_center(mover))
        ok_side = end_side == start_side
        print(f"[{name}] kleinster Abstand Rad-{ground_name}: {d:.3e} -> {'OK' if ok_touch else 'FEHLER'}")
        print(f"[{name}] Radmitte auf Startseite ({start_side:+d}): {end_side:+d} -> {'OK' if ok_side else 'FEHLER'}")
        ok = ok_touch and ok_side
    if swap_refs:
        unchanged = joint.Reference2[0] == ground
        print(f"[{name}] Gelenk unveraendert (Geometrie bleibt Reference2): {unchanged}")
        ok = ok and unchanged
    return ok, doc


def world(local, rotation_axis=App.Vector(1, 0, 0), angle=0):
    return App.Placement(GROUND_PLACEMENT.multVec(local), GROUND_PLACEMENT.Rotation.multiply(
        App.Rotation(rotation_axis, angle)))


def main():
    out_path = os.environ.get(
        "KINEMATIC_TEST_OUTPUT_FCSTD", f"/tmp/kinematic-tests/{TEST_NAME}/{TEST_NAME}.FCStd"
    )
    base_dir = os.path.dirname(out_path)

    def path(suffix):
        return os.path.join(base_dir, f"{TEST_NAME}_{suffix}.FCStd")

    results = []
    cases = [
        ("Vertex ueber Bump", path("vertex_bump"), "Bump", "vertex",
         App.Placement(GROUND_PLACEMENT.multVec(App.Vector(-12, 9, 25)), App.Rotation(App.Vector(1, 0, 1), 25))),
        ("Rad ueber Bump", path("wheel_bump_above"), "Bump", "wheel", world(App.Vector(-15, 10, 30), angle=70)),
        ("Rad unter Bump", path("wheel_bump_below"), "Bump", "wheel", world(App.Vector(10, -5, -25), angle=110)),
        ("Rad ueber Plate", path("wheel_plate"), "Plate", "wheel", world(App.Vector(5, 8, 20), angle=80)),
        ("Rad neben Track", path("wheel_track"), "Track", "wheel", world(App.Vector(-5, 20, 0), angle=0)),
    ]
    for name, case_path, ground_name, mover_kind, start in cases:
        ok, doc = run_case(name, case_path, ground_name, mover_kind, start)
        results.append(ok)
        App.closeDocument(doc.Name)

    ok, doc = run_case("Referenzen vertauscht", path("swap"), "Bump", "wheel",
                       world(App.Vector(-15, 10, 30), angle=70), swap_refs=True)
    results.append(ok)
    App.closeDocument(doc.Name)

    # "Umkehren" im Gelenk-Dialog (2026-11-21): einseitiger Kontakt wechselt die Seite und
    # beruehrt danach weiterhin - Kante an Flaeche (Bump, Plate) und Kante an Kante (Track).
    for name, ground_name, start in (
        ("Umkehren Rad/Bump", "Bump", world(App.Vector(-15, 10, 30), angle=70)),
        ("Umkehren Rad/Plate", "Plate", world(App.Vector(5, 8, 20), angle=80)),
        ("Umkehren Rad/Track", "Track", world(App.Vector(-5, 20, 0), angle=0)),
    ):
        ok, doc = run_case(name, path("flip_" + ground_name.lower()), ground_name, "wheel", start)
        ground = jtu.get_by_label(doc, ground_name)
        wheel = jtu.get_by_label(doc, "Wheel")
        joint = next(o for o in doc.Objects if getattr(o, "JointType", "") == "Tangent")
        before = side_of(ground, wheel_center(wheel))
        flipped = joint.Proxy.flipTangentSide(joint)
        doc.recompute()
        after = side_of(ground, wheel_center(wheel))
        d = wheel_distance(ground, wheel)
        ok_flip = flipped and after == -before and d < TOL_CONTACT
        print(f"[{name}] Seite {before:+d} -> {after:+d}, Abstand {d:.3e} -> {'OK' if ok_flip else 'FEHLER'}")
        results.append(ok and ok_flip)
        App.closeDocument(doc.Name)

    ok, doc = run_case("vor dem Speichern", out_path, "Bump", "wheel", world(App.Vector(-15, 10, 30), angle=70))
    results.append(ok)
    doc2, wheel2, exc = jtu.save_close_reopen_recompute(doc, out_path, box_b_name="Wheel")
    if exc is not None:
        print(f"RESULT: FAIL (Exception beim Neuladen+Recompute: {exc!r})")
        return 1
    bump2 = jtu.get_by_label(doc2, "Bump")
    d_reload = wheel_distance(bump2, wheel2)
    ok_reload = d_reload < TOL_CONTACT and side_of(bump2, wheel_center(wheel2)) == 1
    print(f"[nach Neuladen] kleinster Abstand Rad-Bump: {d_reload:.3e} -> {'OK' if ok_reload else 'FEHLER'}")
    results.append(ok_reload)
    App.closeDocument(doc2.Name)

    if all(results):
        print("RESULT: PASS")
        return 0
    print("RESULT: FAIL (Tangent-Zwangsbedingung verletzt, siehe oben)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
