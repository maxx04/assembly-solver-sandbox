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
    """Startlage neben dem Punkt bei 'fraction' der Kante (innerhalb der Bahn, nicht ueber ein Ende
    hinaus - dort haelt das Gelenk den Punkt auf der Verlaengerung der Tangente)."""
    def start(shape):
        e = shape.Edges[edge_index]
        return e.valueAt(e.FirstParameter + fraction * (e.LastParameter - e.FirstParameter)) + offset
    return start


# (Name, Bahn, gewaehlte Kante, Startlage der Quader-Ecke in Bahn-Koordinaten)
CASES = [
    ("B-Spline 3D", bspline, "Edge1", near(0, 0.4, App.Vector(2, 4, 3))),
    ("B-Spline 3D weit", bspline, "Edge1", near(0, 0.7, App.Vector(-6, -9, 8))),
    ("Kreisbogen schraeg", arc3d, "Edge1", near(0, 0.5, App.Vector(3, -4, 2))),
    # Kette: gewaehlt ist der Bogen (Edge2); eine Gerade der Kette waere "Punkt auf Gerade".
    ("Kette: Start an Gerade 1", chain, "Edge2", near(0, 0.4, App.Vector(0, 4, 2))),
    ("Kette: Start am Bogen", chain, "Edge2", near(1, 0.5, App.Vector(2, -1, -1))),
    ("Kette: Start an Gerade 2", chain, "Edge2", near(2, 0.6, App.Vector(4, 0, 3))),
]


def run_case(i, name, make, edge, start_local):
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
    print(f"{name}: rc={rc}, Abstand zur Bahn {dist:.2e} mm, bewegt {moved:.3f} mm -> {'OK' if ok else 'FEHLER'}")
    App.closeDocument(doc.Name)
    return ok


def main():
    ok = True
    for i, case in enumerate(CASES):
        ok &= run_case(i, *case)
    print("RESULT: PASS" if ok else "RESULT: FAIL (siehe Ausgabe oben)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
