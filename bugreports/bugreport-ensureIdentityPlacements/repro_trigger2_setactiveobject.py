# Trigger 2 (Bisektion von ViewProviderAssembly::setEdit(), welches doubleClicked() aufruft):
# Gui.getDocument(doc).ActiveView.setActiveObject('Assembly', assemblyObj) ALLEIN aendert
# nichts. Aber setActiveObject(...) GEFOLGT von assembly.recompute(True) reproduziert die
# Kollabierung exakt - byte-identisch zum vollen doubleClicked(). Isoliert damit den echten
# Ausloeser auf "irgendetwas markiert das Dokument touched, danach erzwingt der naechste
# recompute() einen kompletten Kalt-Solve des Graphen".
import sys, os
RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_" + os.path.basename(__file__).replace(".py", ".txt"))
if os.environ.get("_RAN_BISECT"):
    sys.exit(0)
os.environ["_RAN_BISECT"] = "1"

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

    dump("VOR allem:")

    # Schritt 1: exakt das doCommand aus setEdit() nachbauen (ActiveObject setzen)
    appDoc = App.getDocument(doc.Name)
    Gui.getDocument(appDoc).ActiveView.setActiveObject('Assembly', appDoc.getObject(assembly.Name))
    dump("\nNACH setActiveObject('Assembly', ...) ALLEIN (kein recompute):")

    # Schritt 2: jetzt recompute(True) -- wie am Ende von setEdit()
    assembly.recompute(True)
    dump("\nNACH assembly.recompute(True) MIT vorher gesetztem ActiveObject:")

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
