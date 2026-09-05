# Kontrolle: ein direkter assembly.solve(False)-Aufruf (nutzt die schon zwischengespeicherten
# JCS-Werte, umgeht den Dependency-Graph-basierten "Kalt-Solve") bleibt IMMER sauber - egal ob
# unter FreeCADCmd oder dem vollen GUI-Binary. Zeigt: der Solver selbst ist nicht kaputt, nur
# der Pfad ueber einen erzwungenen kompletten Graph-Recompute nach touched-Zustand.
import sys, os
RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_" + os.path.basename(__file__).replace(".py", ".txt"))
if os.environ.get("_RAN_SOLVEDIRECT_GUI"):
    sys.exit(0)
os.environ["_RAN_SOLVEDIRECT_GUI"] = "1"

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

    path = "/home/maxx/Dokumente/CAD_Workspace/PROJ_CNC_M1/3_Panels_aus_6_auswalbar.FCStd"
    doc = App.openDocument(path)
    assembly = doc.getObject("CNC_M1_009_A_Y_Abstreifer")

    dump("VOR jeglichem Aufruf (frisch geoeffnet, KEIN Gui, KEIN doubleClicked):")

    ret = assembly.solve(False)
    log(f"\nassembly.solve(False) return = {ret}")
    dump("NACH assembly.solve(False) direkt (reiner AppDocumentObject-Call, kein ViewProvider/Edit-Mode):")

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
