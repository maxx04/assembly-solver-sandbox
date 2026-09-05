# Kontrollversuch 2: gleiche Datei wie repro_real_file.py, aber NUR doc.recompute()/
# assembly.recompute(True) - ensureIdentityPlacements() wird nicht aufgerufen, kein Dialog,
# kein Edit-Modus. Ergebnis: Placements bleiben korrekt - der normale Solver/Recompute ist
# nicht die Ursache, es ist konkret ensureIdentityPlacements() (aufgerufen z.B. beim Oeffnen
# eines Joint-Dialogs) in Kombination mit vorhandenen Joints.
# Laeuft unter reinem FreeCADCmd (kein PySide/Gui noetig).
import sys, os

RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_plainrecompute.txt")
if os.environ.get("_RAN_PLAIN"):
    sys.exit(0)
os.environ["_RAN_PLAIN"] = "1"

lines = []
def log(m): lines.append(str(m))
def flush():
    with open(RESULT_FILE, "w") as f:
        f.write("\n".join(lines) + "\n")

try:
    import FreeCAD as App

    path = "/home/maxx/Dokumente/CAD_Workspace/PROJ_CNC_M1/3_Panels_aus_6_auswalbar.FCStd"
    doc = App.openDocument(path)

    panels = [o for o in doc.Objects if "Panel" in o.Name and o.TypeId == "App::Link"]
    assembly = doc.getObject("CNC_M1_009_A_Y_Abstreifer")

    log("VOR jeglichem Recompute (frisch geoeffnet):")
    for l in panels:
        log(f"  {l.Name}: {l.Placement.Base}")

    doc.recompute()
    log("\nNACH einfachem doc.recompute() (kein ensureIdentityPlacements, kein Dialog):")
    for l in panels:
        log(f"  {l.Name}: {l.Placement.Base}")

    assembly.recompute(True)
    log("\nNACH assembly.recompute(True):")
    for l in panels:
        log(f"  {l.Name}: {l.Placement.Base}")

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
