"""Tangent-Gelenk in der Simulation (Nutzerauftrag 2026-11-21): Kurvenscheibe mit Rolle.

Vier Arten: "Rolle" (Kreis an Kurve), "Spitze" (Punkt auf Kurve), "Rolle auf Flaeche" (Kreis an
der Flaeche, die aus der Kurve entlang Y extrudiert ist), "Spitze mit Abstand" (Punkt mit
Abstand R zur Kurve, 2026-11-22: muss genau wie die Rollenmitte laufen). Dazu (2026-11-22) zwei
Laeufe, die anhalten muessen: Grenzen entlang der Kurve (LengthMin/Max) und das Kurvenende.

Aufbau (alles in der XZ-Ebene):
  - Base (Quader), geerdet.
  - Cam: Spline-Kurve z(x), per Slider an Base, nur entlang X verschiebbar. Eine Motion "Linear"
    treibt sie an (SHIFT mm in 1 s).
  - Roller: Kreis (Radius R), per Slider an Base, nur entlang Z verschiebbar (x = 0).
  - Tangent: Kreis an Kurve (Kante an Kante, einseitig) - die Rolle muss der Kontur folgen.

Geprueft je Bild: Rolle beruehrt die Kurve (Abstand < TOL), liegt oberhalb, bleibt auf x = 0,
und hebt bzw. senkt sich insgesamt (folgt wirklich der Kontur, nicht nur Stillstand).
Vanilla kennt den Gelenktyp nicht, dort ist FAIL erwartet.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_tangent_simulation_flat/test_tangent_simulation_flat.py
Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import os
import sys

import FreeCAD as App
import Part

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

TEST_NAME = "test_tangent_simulation_flat"
R = 5.0
SHIFT = 40.0
TOL = 1e-5
V = App.Vector
ALONG_X = App.Rotation(V(0, 1, 0), 90)  # JCS-z entlang X


def cam_curve():
    poles = [V(-80, 0, 0), V(-45, 0, 10), V(-15, 0, -6), V(15, 0, 12), V(45, 0, -4), V(80, 0, 0)]
    curve = Part.BSplineCurve()
    curve.buildFromPoles(poles, False, 3)
    return curve.toShape()


def cam_shape(kind):
    edge = cam_curve()
    if kind == "Rolle auf Flaeche":
        edge.translate(V(0, -20, 0))
        return edge.extrude(V(0, 40, 0))
    return edge


def cam_element(kind):
    return "Face1" if kind == "Rolle auf Flaeche" else "Edge1"


def follower_shape(kind):
    if kind.startswith("Spitze"):
        return Part.Vertex(V(0, 0, 0))
    return Part.makeCircle(R, V(0, 0, 0), V(0, 1, 0))


def follower_element(kind):
    return "Vertex1" if kind.startswith("Spitze") else "Edge1"


def add_joint(group, name, type_index, ref1, plc1, ref2, plc2):
    import JointObject

    joint = group.newObject("App::FeaturePython", name)
    JointObject.Joint(joint, type_index)
    if App.GuiUp:
        JointObject.ViewProviderJoint(joint.ViewObject)
    joint.Reference1 = ref1
    joint.Reference2 = ref2
    joint.Detach1 = True
    joint.Detach2 = True
    joint.Placement1 = plc1
    joint.Placement2 = plc2
    return joint


def build(out_path, kind):
    import JointObject
    import UtilsAssembly

    doc = App.newDocument(TEST_NAME)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    doc.saveAs(out_path)

    base = doc.addObject("Part::Box", "Base")
    base.Placement = App.Placement(V(-5, -5, -60), App.Rotation())
    cam = doc.addObject("Part::Feature", "Cam")
    cam.Shape = cam_shape(kind)
    roller = doc.addObject("Part::Feature", "Roller")
    roller.Shape = follower_shape(kind)
    roller.Placement = App.Placement(V(0, 0, 25), App.Rotation())
    doc.recompute()

    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    for obj in (base, cam, roller):
        assembly.addObject(obj)
    doc.recompute()
    jtu.ground_object(base)

    group = UtilsAssembly.getJointGroup(assembly)
    slider = JointObject.JointTypes.index("Slider")
    tangent = JointObject.JointTypes.index("Tangent")
    # JCS in Base-Koordinaten (Base liegt bei (-5,-5,-60)), Cam/Roller-JCS im Ursprung.
    base_origin = App.Placement(V(5, 5, 60), App.Rotation())
    cam_slide = add_joint(
        group, "CamSlider", slider,
        (base, ["Face1", "Face1"]), App.Placement(base_origin.Base, ALONG_X),
        (cam, [cam_element(kind)] * 2), App.Placement(V(), ALONG_X),
    )
    add_joint(
        group, "RollerSlider", slider,
        (base, ["Face1", "Face1"]), base_origin,
        (roller, [follower_element(kind)] * 2), App.Placement(),
    )
    contact = add_joint(
        group, "Contact", tangent,
        (cam, [cam_element(kind)] * 2), App.Placement(),
        (roller, [follower_element(kind)] * 2), App.Placement(),
    )
    if kind == "Spitze mit Abstand":
        # 2026-11-22: Punkt im Abstand R in der Kurvenebene = Bahn der Rollenmitte.
        contact.Distance = R
    doc.recompute()
    return doc, assembly, cam, roller, cam_slide


def edge_face_distance(edge, face):
    """Kleinster Abstand Kante-Flaeche ueber Projektion auf die Flaeche (Abtastung + Verfeinerung).
    OCCs distToShape rechnet Kante gegen Flaeche nur auf ~1e-7..1e-3 genau (hier gemessen)."""

    def g(t):
        p = edge.valueAt(t)
        u, v = face.Surface.parameter(p)
        return (p - face.Surface.value(u, v)).Length

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


def check_frame(cam, roller, kind):
    """Abstand Folger-Nocken, "oberhalb" und fuer die Rolle kein Eindringen: kleinster Abstand
    der Nockenkontur zur Rollenmitte >= R (distToShape waere auch bei einem Schnitt 0)."""
    d, pairs, _ = roller.Shape.distToShape(cam.Shape)
    if kind == "Rolle auf Flaeche":
        d = edge_face_distance(roller.Shape.Edges[0], cam.Shape.Faces[0])
    center = roller.Placement.Base
    contact = pairs[0][1]
    above = center.z > contact.z - TOL
    if kind == "Spitze mit Abstand":
        d = abs(d - R)  # Abweichung vom Sollabstand
    if not kind.startswith("Spitze"):
        to_center = cam.Shape.distToShape(Part.Vertex(center))[0]
        above = above and to_center > R - TOL
    return d, above, center


def run(kind, out_path):
    import CommandCreateSimulation as CCS
    import UtilsAssembly

    doc, assembly, cam, roller, cam_slide = build(out_path, kind)
    rc = assembly.solve()
    doc.recompute()
    d0, above0, c0 = check_frame(cam, roller, kind)
    print(f"[{kind}] Startlage: solve={rc} Abstand {d0:.2e}, oberhalb={above0}, Folger z={c0.z:.3f}")
    ok = rc == 0 and d0 < TOL and above0

    sim = UtilsAssembly.getSimulationGroup(assembly).newObject("App::FeaturePython", "Simulation")
    CCS.Simulation(sim)
    motion = assembly.newObject("App::FeaturePython", "Motion")
    CCS.Motion(motion, "Linear", cam_slide, f"initialValue + {SHIFT}*time")
    sim.Group = [motion]
    rc = assembly.generateSimulation(sim)
    frames = assembly.numberOfFrames() if rc == 0 else 0
    print(f"[{kind}] generateSimulation rc={rc}, Bilder={frames}")
    if rc != 0 or frames < 10:
        App.closeDocument(doc.Name)
        return False

    worst = 0.0
    zs = []
    xs_off = 0.0
    cam_x = []
    all_above = True
    for k in range(frames):
        assembly.updateForFrame(k)
        d, above, c = check_frame(cam, roller, kind)
        worst = max(worst, d)
        all_above &= above
        zs.append(c.z)
        xs_off = max(xs_off, abs(c.x), abs(c.y))
        cam_x.append(cam.Placement.Base.x)
    shift = cam_x[-1] - cam_x[0]
    travel = max(zs) - min(zs)
    print(f"[{kind}] Nocken verschoben {shift:.2f} mm, Folger Hub {travel:.3f} mm")
    print(f"[{kind}] groesster Abstand {worst:.2e}, immer oberhalb/ohne Eindringen={all_above}, "
          f"Folger seitlich max {xs_off:.2e}")
    ok &= abs(shift - SHIFT) < 1e-3 and travel > 1.0 and worst < TOL and all_above and xs_off < 1e-6

    assembly.updateForFrame(0)
    doc.save()
    App.closeDocument(doc.Name)
    return ok


def arc_on_cam(cam, point):
    """Weg des Kontaktpunkts auf der Nockenkurve ab deren Anfang (wie LengthMin/LengthMax)."""
    edge = cam.Shape.Edges[0]
    u0 = edge.FirstParameter
    u = edge.Curve.parameter(point)
    return edge.Curve.length(u0, u) if u > u0 else 0.0


def run_stop(label, out_path, limits, shift):
    """2026-11-22: die Simulation haelt an den Grenzen entlang der Kurve bzw. am Kurvenende an.
    Spitze auf dem Nocken; ihr Weg auf der Kurve nimmt ab, waehrend der Nocken in +x faehrt."""
    import CommandCreateSimulation as CCS
    import UtilsAssembly

    doc, assembly, cam, roller, cam_slide = build(out_path, "Spitze")
    contact = next(o for o in doc.Objects if getattr(o, "JointType", "") == "Tangent")
    assembly.solve()
    doc.recompute()
    s0 = arc_on_cam(cam, roller.Placement.Base)
    lo = 0.0
    if limits:
        lo = s0 - limits
        contact.EnableLengthMin = True
        contact.EnableLengthMax = True
        contact.LengthMin = lo
        contact.LengthMax = s0 + limits
        doc.recompute()
    sim = UtilsAssembly.getSimulationGroup(assembly).newObject("App::FeaturePython", "Simulation")
    CCS.Simulation(sim)
    motion = assembly.newObject("App::FeaturePython", "Motion")
    CCS.Motion(motion, "Linear", cam_slide, f"initialValue + {shift}*time")
    sim.Group = [motion]
    rc = assembly.generateSimulation(sim)
    frames = assembly.numberOfFrames() if rc == 0 else 0
    ss = []
    for k in range(frames):
        assembly.updateForFrame(k)
        ss.append(arc_on_cam(cam, roller.Placement.Base))
    App.closeDocument(doc.Name)
    full = 102
    step = shift / (full - 2)  # Weg je Bild etwa = Nockenweg je Bild (Kurve hier flach)
    stopped = 2 <= frames < full
    inside = bool(ss) and min(ss) >= lo - TOL
    near = bool(ss) and min(ss) - lo < 3 * step
    ok = rc == 0 and stopped and inside and near
    print(f"[{label}] rc={rc}, Bilder={frames} (voll {full}), Weg {s0:.3f} -> {min(ss) if ss else 0:.3f}, "
          f"Grenze {lo:.3f} -> {'OK' if ok else 'FEHLER'}")
    return ok


def main():
    out_path = os.environ.get(
        "KINEMATIC_TEST_OUTPUT_FCSTD", f"/tmp/kinematic-tests/{TEST_NAME}/{TEST_NAME}.FCStd"
    )
    base = os.path.splitext(out_path)[0]
    results = []
    for i, kind in enumerate(("Rolle", "Spitze", "Rolle auf Flaeche", "Spitze mit Abstand")):
        path = out_path if i == 0 else f"{base}_{i}.FCStd"
        results.append(run(kind, path))
    results.append(run_stop("Spitze, Grenzen entlang der Kurve", f"{base}_stop1.FCStd", 15.0, SHIFT))
    results.append(run_stop("Spitze, ueber das Kurvenende", f"{base}_stop2.FCStd", None, 120.0))
    ok = all(results)
    print("RESULT: PASS" if ok else "RESULT: FAIL (Folger folgt der Kontur nicht, siehe oben)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
