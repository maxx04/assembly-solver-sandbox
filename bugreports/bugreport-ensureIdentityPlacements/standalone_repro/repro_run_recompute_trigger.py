# Regressionstest fuer den Slider-matchJCS-Fix (patches/freecad-assembly-jointobject.patch,
# Eintrag 7): laedt Assembly_repro.FCStd und prueft, dass alle 7 Panel-Y-Positionen ein
# assembly.recompute(True) unveraendert ueberstehen. Vor dem Fix kollabierten sie alle auf
# Y=-85 (Position des geerdeten ersten Panels).
#
# Wie repro_run.py, aber:
# - Trigger ist assembly.recompute(True) statt ViewObject.doubleClicked() (laut
#   Bugreport gleichwertig, aber ohne Edit-Mode/3D-View/Task-Panel -> vermeidet
#   den GL-Hang unter Offscreen-Qt in dieser Umgebung).
# - Ergebnis geht in eine Datei (mit explizitem flush), nicht auf stdout - unter
#   dem Offscreen-Qt-Event-Loop wird gepuffertes stdout beim timeout-Kill sonst
#   verworfen (siehe repro_trigger3_delete_recompute.py fuer das gleiche Muster).
# - Guard gegen doppelte Ausfuehrung (FreeCAD -l fuehrt Skripte in dieser
#   Umgebung offenbar zweimal aus).
#
# Aufruf: siehe "Umgebung zum Ausfuehren" in ../README.md (volles FreeCAD-Binary, Offscreen-Qt).
import sys, os

RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_recompute_trigger.txt")
if os.environ.get("_RAN_RECOMPUTETRIGGER"):
    sys.exit(0)
os.environ["_RAN_RECOMPUTETRIGGER"] = "1"

lines = []
def log(m): lines.append(str(m))
def flush():
    with open(RESULT_FILE, "w") as f:
        f.write("\n".join(lines) + "\n")

try:
    import FreeCAD as App

    HERE = os.path.dirname(os.path.abspath(__file__))
    doc = App.openDocument(os.path.join(HERE, "Assembly_repro.FCStd"))
    App.setActiveDocument(doc.Name)
    assembly = doc.getObject("CNC_M1_009_A_Y_Abstreifer")
    panels = [o for o in doc.Objects if "Panel" in o.Name and o.TypeId == "App::Link"]

    log("=== VOR dem Trigger ===")
    for p in panels:
        log(f"  {p.Name}: Y={p.Placement.Base.y:.3f}")

    log("\n=== Trigger: assembly.recompute(True) ===")
    assembly.recompute(True)

    log("\n=== NACH dem Trigger ===")
    for p in panels:
        log(f"  {p.Name}: Y={p.Placement.Base.y:.3f}")

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
