import sys, os
RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_" + os.path.basename(__file__).replace(".py", ".txt"))
if os.environ.get("_RAN_DELNOGUI"):
    sys.exit(0)
os.environ["_RAN_DELNOGUI"] = "1"

lines = []
def log(m): lines.append(str(m))
def flush():
    with open(RESULT_FILE, "w") as f:
        f.write("\n".join(lines) + "\n")

def dump(label, links):
    log(label)
    for l in links:
        p = l.Placement
        log(f"  {l.Name}: pos=({p.Base.x:.4f},{p.Base.y:.4f},{p.Base.z:.4f})")

try:
    import FreeCAD as App
    # BEWUSST KEIN FreeCADGui-Import, KEIN setActiveDocument, KEIN ActiveObject,
    # KEIN doubleClicked - nur reines App-Level, um zu pruefen ob "Objekte
    # loeschen + recompute" ALLEIN (ohne jeglichen Gui-Kontext) reicht.
    # (JointObject.py selbst importiert allerdings PySide/Gui-Module intern,
    # das lief bisher nur unter dem vollen FreeCAD-Binary mit Offscreen-Qt -
    # wir nutzen also weiterhin den vollen Prozess, vermeiden aber jeglichen
    # Gui.*-Python-Aufruf im Skript selbst.)

    path = "/home/maxx/Dokumente/CAD_Workspace/PROJ_CNC_M1/3_Panels_aus_6_auswalbar.FCStd"
    doc = App.openDocument(path)
    assembly = doc.getObject("CNC_M1_009_A_Y_Abstreifer")

    panels = [doc.getObject(n) for n in [
        "CNC_M1_011_P_Panel", "CNC_M1_011_P_Panel001", "CNC_M1_011_P_Panel002",
        "CNC_M1_011_P_Panel003", "CNC_M1_011_P_Panel004", "CNC_M1_011_P_Panel005",
        "CNC_M1_011_P_Panel006",
    ]]
    ground = doc.getObject("CNC_M1_010_G")

    dump("VOR jeglicher Aenderung (frisch geoeffnet, reines App-Level, kein Gui-Aufruf):",
         [ground] + panels)

    for jn in ["Joint014", "Joint015", "Joint016", "Joint017"]:
        j = doc.getObject(jn)
        if j:
            doc.removeObject(jn)
    for pn in ["CNC_M1_011_P_Panel003", "CNC_M1_011_P_Panel004", "CNC_M1_011_P_Panel005", "CNC_M1_011_P_Panel006"]:
        p = doc.getObject(pn)
        if p:
            doc.removeObject(pn)

    remaining = [ground] + panels[:3]
    dump("\nNACH removeObject (VOR recompute):", remaining)

    doc.recompute()
    dump("\nNACH doc.recompute() (KEIN Gui, KEIN ActiveObject, KEIN doubleClicked):", remaining)

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
