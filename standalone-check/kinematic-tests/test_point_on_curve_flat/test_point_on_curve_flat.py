"""
Regressionstest Punkt-auf-Kurve (plans/2026-11-01-punkt-auf-kurve.md): Abstand-Gelenk Punkt-Kurve
mit Abstand 0 haelt den Punkt genau auf der Kurve bzw. Kurvenkette.

Vorher (Vanilla): Punkt-Kurve wurde als "Punkt in der Ebene der Kurve" gerechnet - bei 3D-Kurven
falsch, beim Kreisbogen darf der Punkt irgendwo in der Kreisebene liegen.

Aufbau je Fall: geerdete Bahn (Part::Feature, verschoben und gedreht) -[Abstand 0, Kante der Bahn
<-> Vertex1 eines Quaders]- Quader frei. Placement1 absichtlich schief (die Kurve wird in das System
von Placement1 umgerechnet). Geprueft: Abstand der Quader-Ecke zur ganzen Bahn nach dem Solve.

Aufruf:
    ./common/run-test.sh <freecad> test_point_on_curve_flat/test_point_on_curve_flat.py
"""
import os
import sys

import FreeCAD as App
import Part

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

DISTANCE = 5
TOL = 1e-6
RAIL_PLC = App.Placement(App.Vector(30, -10, 5), App.Rotation(App.Vector(1, 2, 3), 35))
PLC1 = App.Placement(App.Vector(4, 1, -2), App.Rotation(App.Vector(0, 1, 1), 50))


def bspline():
    pts = [App.Vector(0, 0, 0), App.Vector(20, 10, 5), App.Vector(40, -5, 15), App.Vector(60, 10, 0)]
    c = Part.BSplineCurve()
    c.interpolate(pts)
    return c.toShape()


def arc3d():
    return Part.ArcOfCircle(Part.Circle(App.Vector(0, 0, 0), App.Vector(1, 1, 1), 25), 0.2, 2.5).toShape()


def chain():
    line1 = Part.LineSegment(App.Vector(0, 0, 0), App.Vector(40, 0, 0)).toShape()
    arc = Part.ArcOfCircle(Part.Circle(App.Vector(40, 20, 0), App.Vector(0, 0, 1), 20), -1.5707963267948966, 0.0).toShape()
    line2 = Part.LineSegment(App.Vector(60, 20, 0), App.Vector(60, 70, 0)).toShape()
    return Part.Wire([line1, arc, line2])


def near(edge_index, fraction, offset):
    """Startlage neben dem Punkt bei 'fraction' der Kante (innerhalb der Bahn)."""
    def start(shape):
        e = shape.Edges[edge_index]
        return e.valueAt(e.FirstParameter + fraction * (e.LastParameter - e.FirstParameter)) + offset
    return start


def beyond(edge_index, at_end, extension, offset):
    """Startlage hinter einem Bahnende: entlang der Endtangente um 'extension' hinaus."""
    def start(shape):
        e = shape.Edges[edge_index]
        u = e.LastParameter if at_end else e.FirstParameter
        t = e.tangentAt(u)
        sign = 1 if at_end else -1
        return e.valueAt(u) + t * (sign * extension) + offset
    return start


# (Name, Bahn, gewaehlte Kante, Startlage der Quader-Ecke in Bahn-Koordinaten[, erwartetes Ende])
CASES = [
    ("B-Spline 3D", bspline, "Edge1", near(0, 0.4, App.Vector(2, 4, 3))),
    ("B-Spline 3D weit", bspline, "Edge1", near(0, 0.7, App.Vector(-6, -9, 8))),
    ("Kreisbogen schraeg", arc3d, "Edge1", near(0, 0.5, App.Vector(3, -4, 2))),
    # Kette: gewaehlt ist der Bogen (Edge2).
    ("Kette: Start an Gerade 1", chain, "Edge2", near(0, 0.4, App.Vector(0, 4, 2))),
    ("Kette: Start am Bogen", chain, "Edge2", near(1, 0.5, App.Vector(2, -1, -1))),
    ("Kette: Start an Gerade 2", chain, "Edge2", near(2, 0.6, App.Vector(4, 0, 3))),
    # 2026-11-21: Gerade einer Kette gewaehlt -> ganze Kette ist die Bahn (vorher unendliche Gerade).
    ("Kette, Gerade gewaehlt: Start am Bogen", chain, "Edge1", near(1, 0.5, App.Vector(2, -1, -1))),
    ("Kette, Gerade gewaehlt: Start an Gerade 2", chain, "Edge1", near(2, 0.6, App.Vector(4, 0, 3))),
    # 2026-11-21: Anschlag an offenen Enden (vorher Verlaengerung der Endtangente).
    ("B-Spline: hinter dem Ende", bspline, "Edge1", beyond(0, True, 12, App.Vector(1, 2, 0)), "end"),
    ("B-Spline: vor dem Anfang", bspline, "Edge1", beyond(0, False, 8, App.Vector(0, 1, 2)), "start"),
    ("Kette: hinter dem Ende", chain, "Edge2", beyond(2, True, 15, App.Vector(2, 0, 1)), "end"),
]


def path_ends(shape):
    """Anfangs- und Endpunkt der Bahn (Welt)."""
    if shape.ShapeType == "Wire":
        v = shape.OrderedVertexes
    else:
        v = shape.Vertexes
    return v[0].Point, v[-1].Point


def run_case(i, name, make, edge, start_local, expect_end=None):
    path = os.path.join(THIS_DIR, "fcstd-output", f"case{i}", "case.FCStd")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    doc = App.newDocument(f"POC{i}")
    doc.saveAs(path)
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    rail = doc.addObject("Part::Feature", "Rail")
    rail.Shape = make()
    rail.Placement = RAIL_PLC
    box = doc.addObject("Part::Box", "Mover")
    asm.addObject(rail)
    asm.addObject(box)
    doc.recompute()
    jtu.ground_object(rail)
    start = RAIL_PLC.multVec(start_local(rail.Shape.copy().transformGeometry(RAIL_PLC.inverse().toMatrix())))
    box.Placement = App.Placement(start, App.Rotation(App.Vector(1, 0, 0), 20))
    joint = jtu.make_joint(asm, DISTANCE, rail, box, PLC1, App.Placement(), edge, "Vertex1")
    joint.Distance = 0.0
    doc.recompute()
    rc = asm.solve()
    corner = box.Placement.Base  # Vertex1 des Quaders liegt im Quader-Ursprung
    dist = rail.Shape.distToShape(Part.Vertex(corner))[0]
    moved = (corner - start).Length
    ok = rc == 0 and dist < TOL
    extra = ""
    if expect_end:
        first, last = path_ends(rail.Shape)
        d_end = (corner - (last if expect_end == "end" else first)).Length
        ok = ok and d_end < 1e-5
        extra = f", Abstand zum Bahn{'ende' if expect_end == 'end' else 'anfang'} {d_end:.2e} mm"
    print(f"{name}: rc={rc}, Abstand zur Bahn {dist:.2e} mm, bewegt {moved:.3f} mm{extra} -> {'OK' if ok else 'FEHLER'}")
    App.closeDocument(doc.Name)
    return ok


def drag_case(name, target_local, expect_end):
    """Ziehen ueber ein Bahnende hinaus (2026-11-21): die Ecke muss am Ende stehen bleiben und auf
    der Bahn liegen - ohne Anschlag lief sie entlang der Endtangente weiter."""
    path = os.path.join(THIS_DIR, "fcstd-output", "drag", f"{expect_end}.FCStd")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    doc = App.newDocument(f"POCdrag{expect_end}")
    doc.saveAs(path)
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    rail = doc.addObject("Part::Feature", "Rail")
    rail.Shape = chain()
    rail.Placement = RAIL_PLC
    box = doc.addObject("Part::Box", "Mover")
    asm.addObject(rail)
    asm.addObject(box)
    doc.recompute()
    jtu.ground_object(rail)
    shape = rail.Shape.copy().transformGeometry(RAIL_PLC.inverse().toMatrix())
    box.Placement = App.Placement(RAIL_PLC.multVec(near(1, 0.5, App.Vector(1, 1, 1))(shape)), App.Rotation())
    joint = jtu.make_joint(asm, DISTANCE, rail, box, PLC1, App.Placement(), "Edge2", "Vertex1")
    joint.Distance = 0.0
    doc.recompute()
    rc = asm.solve()
    start = App.Placement(box.Placement)
    target = RAIL_PLC.multVec(target_local)
    steps = 40
    asm.preDrag([box])
    for k in range(1, steps + 1):
        plc = App.Placement(start)
        plc.Base = start.Base + (target - start.Base) * (k / steps)
        box.Placement = plc
        asm.doDragStep()
    asm.postDrag()
    corner = box.Placement.Base
    first, last = path_ends(rail.Shape)
    dist = rail.Shape.distToShape(Part.Vertex(corner))[0]
    d_end = (corner - (last if expect_end == "end" else first)).Length
    ok = rc == 0 and dist < TOL and d_end < 1e-5
    print(f"{name}: rc={rc}, Abstand zur Bahn {dist:.2e} mm, Abstand zum Bahn"
          f"{'ende' if expect_end == 'end' else 'anfang'} {d_end:.2e} mm -> {'OK' if ok else 'FEHLER'}")
    App.closeDocument(doc.Name)
    return ok


def rectangle():
    pts = [App.Vector(0, 0, 0), App.Vector(40, 0, 0), App.Vector(40, 25, 0), App.Vector(0, 25, 0)]
    return Part.Wire([Part.LineSegment(pts[i], pts[(i + 1) % 4]).toShape() for i in range(4)])


def rectangle_case():
    """Gerade einer reinen Vieleck-Kontur (Rechteck-Skizze) bleibt "Punkt auf unendlicher Gerade"
    wie in Vanilla (2026-11-21): Start hinter der Ecke auf der Verlaengerung von Edge1 (y = 0)
    mit Versatz - der Punkt muss auf y = 0 hinter der Ecke landen, nicht auf der Kontur."""
    doc = App.newDocument("POCrect")
    path = os.path.join(THIS_DIR, "fcstd-output", "rectangle", "case.FCStd")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    doc.saveAs(path)
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    rail = doc.addObject("Part::Feature", "Rail")
    rail.Shape = rectangle()
    rail.Placement = RAIL_PLC
    box = doc.addObject("Part::Box", "Mover")
    asm.addObject(rail)
    asm.addObject(box)
    doc.recompute()
    jtu.ground_object(rail)
    box.Placement = App.Placement(RAIL_PLC.multVec(App.Vector(55, 2, 1)), App.Rotation())
    import UtilsAssembly

    # Gerade: JCS entlang der Geraden wie beim Anlegen in der Oberflaeche (nicht das schiefe PLC1)
    plc1 = UtilsAssembly.findPlacement([rail, ["Edge1", "Edge1"]])
    joint = jtu.make_joint(asm, DISTANCE, rail, box, plc1, App.Placement(), "Edge1", "Vertex1")
    joint.Distance = 0.0
    doc.recompute()
    rc = asm.solve()
    local = RAIL_PLC.inverse().multVec(box.Placement.Base)
    off_line = (local.y ** 2 + local.z ** 2) ** 0.5
    ok = rc == 0 and off_line < TOL and local.x > 40 + 1
    print(f"Rechteck-Kontur, Gerade gewaehlt: rc={rc}, Abstand zur Geraden {off_line:.2e} mm, "
          f"x = {local.x:.3f} (hinter der Ecke bei 40) -> {'OK' if ok else 'FEHLER'}")
    App.closeDocument(doc.Name)
    return ok


def main():
    ok = True
    ok &= rectangle_case()
    for i, case in enumerate(CASES):
        ok &= run_case(i, *case)
    ok &= drag_case("Ziehen weit hinter das Ende", App.Vector(150, 200, 30), "end")
    ok &= drag_case("Ziehen weit vor den Anfang", App.Vector(-150, -40, -20), "start")
    print("RESULT: PASS" if ok else "RESULT: FAIL (siehe Ausgabe oben)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
