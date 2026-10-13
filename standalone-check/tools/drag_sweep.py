"""
drag_sweep.py - Drag-Rundumtest an einem echten Projekt (Standard: PROJ_CNC3018).

Zieht in jeder angegebenen Baugruppe jedes Slider-, Dreh- und Zylindergelenk (je Gelenk-INSTANZ)
so, wie es die GUI beim Ziehen mit der Maus tut: Klickteil -> getJointOfPartConnectingToGround(),
bei Fixed weiter zum beweglichen Teil (getUpstreamMovingPart()), Mitlaeufer per
getDownstreamParts(), dann preDrag()/doDragStep()/postDrag(). Danach werden ALLE Gelenke in der
Solver-Sicht geprueft (ASMT-Export, wie test_originals_untouched); gezaehlt werden nur
Verletzungen, die vorher nicht bestanden.

Entstanden 2026-10-11 (BG75: Halteplatte wurde beim Ziehen der Scheibe mitgezogen, weil sie nicht
mit dem geerdeten Motor gebuendelt wurde). Faehrt bewusst weit (Standard 20 mm bzw. 100 Grad),
damit Gelenk-Anschlaege erreicht werden - genau dort zeigen sich solche Fehler.

Arbeitet immer auf einer KOPIE des Projekts (temporaerer Ordner), die Projektdateien bleiben
unveraendert.

Aufruf (aus standalone-check/kinematic-tests):
    ./common/run-test.sh /home/maxx/freecad-sandbox/install ../tools/drag_sweep.py
Umgebungsvariablen (optional):
    DRAG_SWEEP_PROJECT  Projektordner (Standard: standalone-check/PROJ_CNC3018)
    DRAG_SWEEP_DOCS     kommagetrennte Dokumentnamen ohne .FCStd
    DRAG_SWEEP_STEPS    Anzahl Schritte (Standard 20; je 1 mm bzw. 5 Grad)
Gibt am Ende "RESULT: PASS" (keine neuen Verletzungen) oder "RESULT: FAIL ..." aus.
"""
import glob
import os
import shutil
import sys
import tempfile

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
STANDALONE = os.path.dirname(THIS_DIR)
KT = os.path.join(STANDALONE, "kinematic-tests")
sys.path.insert(0, os.path.join(KT, "test_originals_untouched"))
sys.path.insert(0, os.path.join(KT, "common"))
import test_originals_untouched as tou  # noqa: E402

PROJECT = os.environ.get("DRAG_SWEEP_PROJECT", os.path.join(STANDALONE, "PROJ_CNC3018"))
DOCS = os.environ.get(
    "DRAG_SWEEP_DOCS",
    "CNC3018_075_A_TreiberBaugruppe_X,CNC3018_025_A_FuerungsBaugruppe330,"
    "CNC3018_032_A_FuehrungsBaugruppe400,CNC3018_037_A_TraegerBaugruppe_Z,"
    "CNC3018_043_A_TreiberBaugruppe100,CNC3018_022_A_CNC3018_GesamtBaugruppe",
).split(",")
STEPS = int(os.environ.get("DRAG_SWEEP_STEPS", "20"))
STEP_MM = 1.0
STEP_DEG = 5.0


def close_all():
    for name in list(App.listDocuments()):
        App.closeDocument(name)


def open_top(work_dir, name):
    close_all()
    doc = App.openDocument(os.path.join(work_dir, name + ".FCStd"))
    asm = [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]
    doc.recompute()
    asm.solve()
    return doc, asm


def instance_of(asm, ref_obj, prefix):
    """Objekt, auf das der Nutzer in der aktiven BG klicken wuerde: das Teil selbst bzw. sein
    Spiegel entlang des Verschachtelungspfads."""
    container = asm
    for name in [p for p in prefix.split(".") if p]:
        nxt = [o for o in container.Group if o.Name == name]
        if not nxt:
            return None
        container = nxt[0]
    if ref_obj.Document is asm.Document and ref_obj in container.Group:
        return ref_obj
    for obj in container.Group:
        if getattr(obj, "LinkedObject", None) == ref_obj:
            return obj
    return None


def world_placements(asm):
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


def drag_case(work_dir, name, index, tmpdir):
    doc, asm = open_top(work_dir, name)
    instances = [
        x for x in asm.getJointInstances() if x[0].JointType in ("Slider", "Revolute", "Cylindrical")
    ]
    if index >= len(instances):
        return None
    joint, prefix = instances[index]
    label = f"{joint.Document.Name}#{joint.Name}" + (f" [{prefix}]" if prefix else "")
    clicked = None
    for ref in ("Reference2", "Reference1"):
        clicked = instance_of(asm, getattr(joint, ref)[0], prefix)
        if clicked is not None:
            break
    if clicked is None:
        return (label, "uebersprungen (kein Klickteil gefunden)", 0)

    before, _ = tou.check_joints_in_solver_view(asm, tmpdir)
    # wie findDragMode(): Bewegungsgelenk mit den wenigsten Freiheitsgraden unter allen
    # Verbindungen zur Masse (Fixed ueber die starre Kette), die uebrigen zaehlen nicht als Erdung
    part, moving_joint, ref_name, bypassed = asm.getDragJointOfPart(clicked)
    if moving_joint is None:
        return (label, "uebersprungen (kein Bewegungsgelenk / frei)", 0)

    downstream = asm.getDownstreamParts(part, moving_joint, bypassed)
    drag_parts = [part] + [p for p in downstream if p != part]
    worlds = world_placements(asm)
    if part not in worlds:
        return (label, "uebersprungen (Teil nicht im Baum)", 0)
    jcs = worlds[part].multiply(
        moving_joint.Placement1 if ref_name == "Reference1" else moving_joint.Placement2
    )
    initial = {p: App.Placement(p.Placement) for p in drag_parts}

    asm.preDrag(drag_parts)
    for step in range(1, STEPS + 1):
        for p in drag_parts:
            plc = App.Placement(initial[p])
            if moving_joint.JointType == "Revolute":
                turned = jcs.multiply(
                    App.Placement(App.Vector(), App.Rotation(App.Vector(0, 0, 1), STEP_DEG * step))
                )
                plc = turned.multiply(plc.inverse().multiply(jcs).inverse())
            else:
                axis = jcs.Rotation.multVec(App.Vector(0, 0, 1))
                plc.Base = plc.Base + axis * (STEP_MM * step)
            p.Placement = plc
        asm.doDragStep()
    asm.postDrag()

    after, _ = tou.check_joints_in_solver_view(asm, tmpdir)
    new = [e for e in after if e not in before]
    desc = (f"{label}: zieht {part.Name} ueber {moving_joint.Name} ({moving_joint.JointType}), "
            f"{len(drag_parts)} Teil(e)")
    return (desc, "OK" if not new else "; ".join(new[:3]), len(new))


def main():
    if not os.path.isdir(PROJECT):
        print(f"RESULT: FAIL (Projektordner fehlt: {PROJECT})")
        return 1
    total_new = 0
    with tempfile.TemporaryDirectory() as tmpdir:
        work_dir = os.path.join(tmpdir, "projekt")
        os.makedirs(work_dir)
        for path in glob.glob(os.path.join(PROJECT, "*.FCStd")):
            shutil.copy2(path, work_dir)
        for name in DOCS:
            index = 0
            while True:
                try:
                    result = drag_case(work_dir, name, index, tmpdir)
                except Exception as exc:  # noqa: BLE001 - jeder Fall soll berichtet werden
                    result = (f"{name} Gelenk {index}", f"FEHLER {exc!r}", 1)
                if result is None:
                    break
                total_new += result[2]
                print(f"  {result[0]} -> {result[1]}")
                index += 1
    close_all()
    if total_new:
        print(f"RESULT: FAIL ({total_new} neue Gelenkverletzung(en) nach dem Ziehen)")
        return 1
    print("RESULT: PASS")
    return 0
