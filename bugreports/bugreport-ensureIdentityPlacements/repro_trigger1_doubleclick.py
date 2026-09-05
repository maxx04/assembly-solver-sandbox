# Trigger 1: assembly.ViewObject.doubleClicked() GANZ ALLEIN (kein ensureIdentityPlacements(),
# kein sonstiger Aufruf danach) reproduziert bereits die Kollabierung. Widerlegt den fruehen
# Verdacht gegen ensureIdentityPlacements() - der war ein Artefakt der Aufrufreihenfolge im
# ersten Testskript (doubleClicked() lief dort VOR ensureIdentityPlacements(), nicht danach).
import sys, os
RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_" + os.path.basename(__file__).replace(".py", ".txt"))
if os.environ.get("_RAN_DCLICK"):
    sys.exit(0)
os.environ["_RAN_DCLICK"] = "1"

lines = []
def log(m): lines.append(str(m))
def flush():
    with open(RESULT_FILE, "w") as f:
        f.write("\n".join(lines) + "\n")

def dump(label):
    log(label)
    for o in assembly.Group:
        if hasattr(o, "Placement"):
            log(f"  {o.Name}: {o.Placement}")

try:
    import FreeCAD as App
    import FreeCADGui as Gui

    path = "/home/maxx/Dokumente/CAD_Workspace/PROJ_CNC_M1/3_Panels_aus_6_auswalbar.FCStd"
    doc = App.openDocument(path)
    App.setActiveDocument(doc.Name)
    assembly = doc.getObject("CNC_M1_009_A_Y_Abstreifer")

    dump("VOR doubleClicked() (frisch geoeffnet, kein ensureIdentityPlacements aufgerufen):")

    assembly.ViewObject.doubleClicked()

    dump("\nNACH doubleClicked() alleine (OHNE jeglichen weiteren Aufruf):")

    log(f"\nUtilsAssembly.activeAssembly() jetzt aktiv: pruefe importierbar")
    import UtilsAssembly
    active = UtilsAssembly.activeAssembly()
    log(f"activeAssembly() = {active}")

    dump("\nNoch einmal nach activeAssembly()-Aufruf (reiner Read, kein write):")

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
