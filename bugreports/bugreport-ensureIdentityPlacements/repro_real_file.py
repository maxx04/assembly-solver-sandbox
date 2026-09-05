# KORRIGIERT (siehe README.md, Abschnitt "Korrektur"): dieses Skript war der urspruengliche
# Reproduktionsversuch und hat den Verdacht gegen ensureIdentityPlacements() ausgeloest - der
# sich als FALSCH herausgestellt hat. Der eigentliche Ausloeser ist NICHT der explizite
# ensureIdentityPlacements()-Aufruf, sondern das vorangehende assembly.ViewObject.doubleClicked()
# (noetig, um activeAssembly() ueberhaupt nicht-None zu bekommen) - siehe
# repro_trigger1_doubleclick.py / repro_trigger2_setactiveobject.py fuer die korrigierte,
# isolierte Reproduktion. Datei hier nur noch als historische Dokumentation des Irrwegs belassen.
#
# Diese Datei (3_Panels_aus_6_auswalbar.FCStd) ist NICHT im Repo enthalten (privates
# Projekt des Nutzers) - Pfad unten anpassen, wenn du das selbst nachvollziehen willst.
# Ausfuehren mit dem vollen FreeCAD-Binary + Offscreen-Qt, siehe README.md in diesem Ordner.
import sys, os

RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_realfile.txt")
if os.environ.get("_RAN_REALFILE5"):
    sys.exit(0)
os.environ["_RAN_REALFILE5"] = "1"

lines = []
def log(m): lines.append(str(m))
def flush():
    with open(RESULT_FILE, "w") as f:
        f.write("\n".join(lines) + "\n")

try:
    import FreeCAD as App
    import FreeCADGui as Gui

    path = "/home/maxx/Dokumente/CAD_Workspace/PROJ_CNC_M1/3_Panels_aus_6_auswalbar.FCStd"
    doc = App.openDocument(path)
    App.setActiveDocument(doc.Name)
    gui_doc = Gui.getDocument(doc)
    log(f"activeView() = {gui_doc.activeView()}")

    panels = [o for o in doc.Objects if "Panel" in o.Name and o.TypeId == "App::Link"]

    log("=== VOR dem Oeffnen des Joint-Dialogs ===")
    for l in panels:
        log(f"  {l.Name}: {l.Placement.Base}")

    joint013 = doc.getObject("Joint013")
    log(f"\nJoint013 gefunden: {joint013}")

    import JointObject as JO
    import UtilsAssembly

    asm_obj = doc.getObject("CNC_M1_009_A_Y_Abstreifer")
    asm_obj.ViewObject.doubleClicked()  # entspricht Doppelklick in den Baum, um "hineinzugehen"

    assembly = UtilsAssembly.activeAssembly()
    log(f"\nactiveAssembly() nach doubleClicked() = {assembly}")

    log("\n--- ElementCount DIREKT VOR ensureIdentityPlacements() ---")
    for l in panels:
        log(f"  {l.Name}: ElementCount={l.ElementCount} LinkedObject={l.LinkedObject} ElementList={l.ElementList}")

    log("\n--- Rufe ensureIdentityPlacements() EINZELN auf ---")
    assembly.ensureIdentityPlacements()
    for l in panels:
        log(f"  {l.Name}: {l.Placement.Base}")

    log("\n--- Jetzt kompletter TaskAssemblyCreateJoint(0, Joint013) ---")
    task = JO.TaskAssemblyCreateJoint(0, joint013)
    for l in panels:
        log(f"  {l.Name}: {l.Placement.Base}")

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
