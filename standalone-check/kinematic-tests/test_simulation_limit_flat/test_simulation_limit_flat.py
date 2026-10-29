"""
Regressionstest: die Simulation haelt an einer Gelenkgrenze an, statt das Gelenk per Motion
durch seinen Anschlag zu fahren.

Anlass (TODO 2026-10-11, umgesetzt 2026-10-29): OndselSolver prueft Grenzen nur beim Ziehen
(und seit 45bd7d1045 beim Solve), runKINEMATIC gar nicht (Upstream-PR OndselSolver#11). Jetzt
prueft SystemSolver::output() vor jedem Simulationsbild die Grenzen und beendet die Simulation
beim ersten Bild ausserhalb; die Bilder davor bleiben, FreeCAD meldet Gelenk und Zeit.

Faelle (Fixturen der flachen Tests wiederverwendet, nur Kopien):
1. Slider mit Grenze -2.1..2.1 mm (test_slider_limit_flat), Motion 1 mm/s ueber 5 s:
   Simulation endet vor 2.1 mm + Schritt, letztes Bild innerhalb.
2. Drehgelenk (test_revolute_joint_flat, steht bei ~105 Grad), Grenze Start+50 Grad, Motion
   1 rad/s ueber 3 s (172 Grad): haelt vor der Grenze an.
3. Gegenprobe: Grenze Start+70 Grad, Motion nur 1 s (57 Grad): alle Bilder.

Aufruf:
    ./common/run-test.sh <freecad> test_simulation_limit_flat/test_simulation_limit_flat.py
"""
import math
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

import CommandCreateSimulation as CCS  # noqa: E402
import UtilsAssembly  # noqa: E402

TESTS = os.path.join(THIS_DIR, "..")
STEP = 0.1
TOL = 1e-6


def joint_value(joint):
    """Gelenkwert aus den JCS: Slider = z-Abstand (mm), Drehgelenk = Winkel um z (Grad)."""
    rel = joint.Reference1[0].Placement.multiply(joint.Placement1).inverse().multiply(
        joint.Reference2[0].Placement.multiply(joint.Placement2))
    if joint.JointType == "Slider":
        return rel.Base.z
    return rel.Rotation.toEuler()[0]


def simulate(name, fixture_test, motion_type, limit_setup, t_end, expect_stop):
    fixture = jtu.fixture_path(os.path.join(TESTS, fixture_test))
    out = os.path.join(THIS_DIR, "fcstd-output", f"{name}.FCStd")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    jtu.copy_fixture_to_output(fixture, out)
    doc = App.openDocument(out)
    asm = doc.getObject("Assembly")
    joint = [o for o in doc.Objects if hasattr(o, "JointType") and o.JointType in ("Slider", "Revolute")][0]
    lo, hi = limit_setup(joint, joint_value(joint))
    doc.recompute()
    asm.solve()
    start = joint_value(joint)

    sim = UtilsAssembly.getSimulationGroup(asm).newObject("App::FeaturePython", "Simulation")
    CCS.Simulation(sim)
    sim.bTimeEnd = t_end
    sim.cTimeStepOutput = STEP
    motion = asm.newObject("App::FeaturePython", "Motion")
    CCS.Motion(motion, motion_type, joint, "initialValue + 1.0*time")
    sim.Group = sim.Group + [motion]
    doc.recompute()

    rc = asm.generateSimulation(sim)
    frames = asm.numberOfFrames() if rc == 0 else 0
    full = int(round(t_end / STEP)) + 2  # Bild 0 = Ausgangslage, dann t = 0, STEP, ..., t_end
    values = []
    for k in range(frames):
        asm.updateForFrame(k)
        values.append(joint_value(joint))
    asm.updateForFrame(0)
    App.closeDocument(doc.Name)

    ok = rc == 0 and frames >= 2
    inside = all(lo - TOL <= v <= hi + TOL for v in values)
    stopped = frames < full
    last = values[-1] if values else float("nan")
    print(f"{name}: rc={rc}, {frames}/{full} Bilder, Start {start:.3f}, letztes Bild {last:.3f} "
          f"(Grenze {lo}..{hi})")
    if not inside:
        print(f"  FEHLER: Bild ausserhalb der Grenze: {[round(v, 3) for v in values if not lo - TOL <= v <= hi + TOL][:3]}")
    if stopped != expect_stop:
        print(f"  FEHLER: Simulation {'angehalten' if stopped else 'durchgelaufen'}, erwartet "
              f"{'Anhalten' if expect_stop else 'Durchlauf'}")
    if expect_stop and values and hi - last > 1.5 * (STEP * (1.0 if motion_type == 'Linear' else 57.3)):
        print(f"  FEHLER: zu frueh angehalten ({last:.3f} weit vor {hi})")
        ok = False
    return ok and inside and stopped == expect_stop


def slider_limits(joint, start):
    joint.EnableLengthMin = True
    joint.LengthMin = -2.1
    joint.EnableLengthMax = True
    joint.LengthMax = 2.1
    return -2.1, 2.1


def revolute_limit(span_deg):
    """Grenzen relativ zum Startwinkel (Gelenkwert der Fixtur), innerhalb -180..180 fuer
    joint_value()."""
    def setup(joint, start):
        joint.EnableAngleMin = True
        joint.AngleMin = start - 100.0
        joint.EnableAngleMax = True
        joint.AngleMax = start + span_deg
        return start - 100.0, start + span_deg
    return setup


def main():
    ok = True
    ok &= simulate("slider", "test_slider_limit_flat", "Linear", slider_limits, 5.0, True)
    ok &= simulate("revolute", "test_revolute_joint_flat", "Angular", revolute_limit(50.0), 3.0, True)
    ok &= simulate("revolute_free", "test_revolute_joint_flat", "Angular", revolute_limit(70.0), 1.0, False)
    print("RESULT: PASS" if ok else "RESULT: FAIL (siehe Ausgabe oben)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
