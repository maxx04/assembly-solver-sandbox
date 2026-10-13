"""
test_simulation_nested.py - Regressionstest fuer die Assembly-Simulation in flachen und
verschachtelten Baugruppen (Plan plans/2026-10-10-simulation-an-struktur-anpassen.md).

Anlass (2026-10-10, CNC3018: Rotor-Gelenk aus BG67 in BG43/BG37/BG22 angetrieben): "initialValue"
in der Motion-Formel wurde an den ORIGINALEN im Dokument des Gelenks gemessen statt an den
Spiegeln der aktiven BG - das erste Simulationsbild sprang um den Unterschied (BG43: -30 Grad).

Laeuft ueber die Revolute-Fixturen flach, verschachtelt und doppelt verschachtelt (Fixturen nur
gelesen, Kopie mit Original-Dateinamen). Bei verschachtelten Fixturen wird vorher das ORIGINAL im
Unterdokument um ORIGINAL_OFFSET_DEG um die Gelenkachse gedreht (Gelenk bleibt dort erfuellt) -
der echte Fall "Unter-BG separat in anderer Stellung gespeichert". Nur dann unterscheiden sich
Original und Spiegel, und ein aus den Originalen gemessener Startwinkel faellt auf. Je Fixtur:
oberste BG loesen, Motion "Angular" mit
Formel "initialValue + 1.0*time" (rad) auf das Drehgelenk, Simulation erzeugen, dann pruefen:
  1. Bild 0 = Ausgangslage (alle Instanz-Placements unveraendert).
  2. Bild 1 (time = 0) = Startwinkel: kein Sprung.
  3. Letztes Bild (time = 1): Gelenkwinkel = Startwinkel + 1 rad.
  4. Drehgelenk in jedem geprueften Bild erfuellt (Gelenkpunkte deckungsgleich, Achsen parallel).
  5. Originale in allen anderen Dokumenten unveraendert (Regel C2).

Die Gelenkstellung wird unabhaengig vom Solver aus den Instanzen der aktiven BG berechnet: Instanz
eines Referenz-Teils = das Teil selbst (flach) bzw. der Spiegel im obersten Dokument, dessen
LinkedObject das referenzierte Teil ist; Weltlage = Placement-Kette der Container.

Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import glob
import math
import os
import shutil
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_ROOT = os.path.dirname(THIS_DIR)
sys.path.insert(0, os.path.join(TESTS_ROOT, "common"))
import nested_test_utils as ntu  # noqa: E402

ntu.allow_duplicate_labels()

import UtilsAssembly  # noqa: E402
import CommandCreateSimulation as CCS  # noqa: E402

FIXTURES = ("test_revolute_joint_flat", "test_revolute_nested_flex", "test_revolute_double_nested_flex")
FORMULA_RATE = 1.0  # rad pro Sekunde
ORIGINAL_OFFSET_DEG = 30.0
TOL_POS = 1e-5
TOL_ANG = 1e-5  # rad


def output_root():
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    if base:
        return os.path.splitext(base)[0]
    return os.path.join(THIS_DIR, "fcstd-output", "default")


def close_all_documents():
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)


def assembly_of(doc):
    for obj in doc.Objects:
        if obj.TypeId == "Assembly::AssemblyObject":
            return obj
    return None


def top_document_path(out_dir):
    grand = glob.glob(os.path.join(out_dir, "*_grand.FCStd"))
    if grand:
        return grand[0]
    name = os.path.basename(out_dir)
    return os.path.join(out_dir, name + ".FCStd")


def world_placements(asm):
    """Weltlage jedes Objekts im Baum der aktiven BG (Container-Kette)."""
    result = {}

    def rec(container, plc):
        for obj in container.Group:
            if "Placement" not in obj.PropertiesList:
                continue
            world = plc.multiply(obj.Placement)
            result[obj] = world
            if obj.TypeId == "Assembly::AssemblyLink":
                rec(obj, world)

    rec(asm, App.Placement())
    return result


def instance_of(ref_obj, top_doc, worlds):
    if ref_obj.Document is top_doc:
        return ref_obj
    for obj in worlds:
        if obj.TypeId == "App::Link" and obj.LinkedObject == ref_obj:
            return obj
    return None


def joint_jcs(joint, top_doc, asm):
    worlds = world_placements(asm)
    jcs = []
    for ref_name, plc_name in (("Reference1", "Placement1"), ("Reference2", "Placement2")):
        inst = instance_of(getattr(joint, ref_name)[0], top_doc, worlds)
        if inst is None:
            return None
        jcs.append(worlds[inst].multiply(getattr(joint, plc_name)))
    return jcs


def joint_state(joint, top_doc, asm):
    """(Winkel in rad, Gelenkpunkt-Abstand, Achsen-Winkel) der Gelenk-Instanz in der BG asm
    (top_doc = deren Dokument; dort die Teile selbst, sonst ihre Spiegel)."""
    jcs = joint_jcs(joint, top_doc, asm)
    if jcs is None:
        return None
    rel = jcs[0].inverse().multiply(jcs[1])
    x_axis = rel.Rotation.multVec(App.Vector(1, 0, 0))
    angle = math.atan2(x_axis.y, x_axis.x)
    z0 = jcs[0].Rotation.multVec(App.Vector(0, 0, 1))
    z1 = jcs[1].Rotation.multVec(App.Vector(0, 0, 1))
    axis_dev = z0.getAngle(z1)
    axis_dev = min(axis_dev, math.pi - axis_dev)
    return angle, (jcs[0].Base - jcs[1].Base).Length, axis_dev


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def snapshot_originals(top_doc):
    snap = {}
    for doc in App.listDocuments().values():
        if doc is top_doc:
            continue
        for obj in doc.Objects:
            if "Placement" in obj.PropertiesList:
                snap[f"{doc.Name}#{obj.Name}"] = App.Placement(obj.Placement)
    return snap


def check_fixture(name):
    fixture_dir = os.path.join(TESTS_ROOT, name)
    out_dir = os.path.join(output_root(), name)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    shutil.copytree(os.path.join(fixture_dir, "fixtures"), out_dir)

    top_doc = App.openDocument(top_document_path(out_dir))
    asm = assembly_of(top_doc)
    top_doc.recompute()
    for _ in range(3):
        asm.solve(False)

    errors = []
    instances = UtilsAssembly.getJointInstancesOfType(asm, ["Revolute"])
    if not instances:
        close_all_documents()
        return [f"{name}: kein Drehgelenk gefunden"]
    joint, prefix = instances[0]

    if joint.Document is not top_doc:
        # Original im Unterdokument um die Gelenkachse drehen (siehe Moduldokstring).
        own_asm = assembly_of(joint.Document)
        jcs = joint_jcs(joint, joint.Document, own_asm)
        moving = getattr(joint, "Reference2")[0]
        turn = App.Placement(App.Vector(), App.Rotation(App.Vector(0, 0, 1), ORIGINAL_OFFSET_DEG))
        delta = jcs[1].multiply(turn).multiply(jcs[1].inverse())
        moving.Placement = delta.multiply(moving.Placement)
        joint.Document.recompute()
        top_doc.recompute()
        orig = joint_state(joint, joint.Document, own_asm)
        inst = joint_state(joint, top_doc, asm)
        if abs(math.degrees(wrap(orig[0] - inst[0]))) < ORIGINAL_OFFSET_DEG / 2:
            errors.append("Original und Spiegel unterscheiden sich nicht - Test waere unempfindlich")

    start = joint_state(joint, top_doc, asm)
    if start is None:
        close_all_documents()
        return [f"{name}: Gelenk-Instanzen nicht auffindbar"]
    before_instances = {o: App.Placement(p) for o, p in world_placements(asm).items()}
    before_originals = snapshot_originals(top_doc)

    sim_group = UtilsAssembly.getSimulationGroup(asm)
    sim = sim_group.newObject("App::FeaturePython", "Simulation")
    CCS.Simulation(sim)
    motion = asm.newObject("App::FeaturePython", "Motion")
    value = (joint, [prefix]) if prefix else joint
    CCS.Motion(motion, "Angular", value, f"initialValue + {FORMULA_RATE}*time")
    sim.Group = [motion]

    rc = asm.generateSimulation(sim)
    frames = asm.numberOfFrames() if rc == 0 else 0
    if rc != 0 or frames < 3:
        close_all_documents()
        return [f"{name}: generateSimulation rc={rc}, Bilder={frames}"]

    asm.updateForFrame(0)
    for obj, plc in world_placements(asm).items():
        old = before_instances.get(obj)
        if old is not None and not old.isSame(plc, TOL_POS):
            errors.append(f"Bild 0 weicht von der Ausgangslage ab: {obj.Name}")

    for frame, expected in ((1, start[0]), (frames - 1, start[0] + FORMULA_RATE)):
        asm.updateForFrame(frame)
        state = joint_state(joint, top_doc, asm)
        diff = abs(wrap(state[0] - expected))
        if diff > TOL_ANG:
            errors.append(
                f"Bild {frame}: Winkel {math.degrees(state[0]):.4f} statt {math.degrees(expected):.4f} Grad"
            )
        if state[1] > TOL_POS or state[2] > TOL_ANG:
            errors.append(f"Bild {frame}: Drehgelenk verletzt ({state[1]:.6f}mm, {math.degrees(state[2]):.6f} Grad)")
    asm.updateForFrame(0)

    after_originals = snapshot_originals(top_doc)
    for key, plc in before_originals.items():
        now = after_originals.get(key)
        if now is not None and not now.isSame(plc, 1e-12):
            errors.append(f"Original {key} veraendert")

    errors = [f"{name}: {e}" for e in errors]
    print(f"  {name}: Startwinkel {math.degrees(start[0]):.3f} Grad, {frames} Bilder, "
          f"{'OK' if not errors else f'{len(errors)} FEHLER'}")
    close_all_documents()
    return errors


def main():
    close_all_documents()
    all_errors = []
    for name in FIXTURES:
        all_errors.extend(check_fixture(name))
    for e in all_errors[:40]:
        print(f"  FEHLER: {e}")
    if all_errors:
        print(f"RESULT: FAIL ({len(all_errors)} Fehler in {len(FIXTURES)} Fixturen)")
        return 1
    print(f"RESULT: PASS ({len(FIXTURES)} Fixturen)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
