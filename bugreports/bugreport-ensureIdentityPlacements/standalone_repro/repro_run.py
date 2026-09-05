# Eigenstaendiges Reproduktionsskript - braucht nur die Dateien in diesem Ordner
# (Assembly_repro.FCStd, CNC_M1_010_G.FCStd, CNC_M1_011_P_Panel.FCStd), keine externen
# Pfade. Ausfuehren mit dem vollen FreeCAD-Binary (nicht FreeCADCmd - JointObject.py
# braucht PySide/Gui):
#
#   /pfad/zu/FreeCAD -l repro_run.py
#
# (Bei einer selbst gebauten FreeCAD-Instanz mit venv-PySide6 muessen ggf.
# QT_PLUGIN_PATH/LD_LIBRARY_PATH gesetzt werden, siehe README.md im Elternordner.)
#
# Erwartung (Bug): alle 7 Panel-Positionen kollabieren auf Y=-85 (Position von
# "CNC_M1_011_P_Panel", dem geerdeten ersten Panel der Kette). Korrekt waeren
# 7 unterschiedliche Y-Werte (-85, -50, -15, 20, 55, 90, 125).
import os

import FreeCAD as App
import FreeCADGui as Gui

HERE = os.path.dirname(os.path.abspath(__file__))
doc = App.openDocument(os.path.join(HERE, "Assembly_repro.FCStd"))
App.setActiveDocument(doc.Name)
assembly = doc.getObject("CNC_M1_009_A_Y_Abstreifer")
panels = [o for o in doc.Objects if "Panel" in o.Name and o.TypeId == "App::Link"]

print("=== VOR dem Trigger (korrekte, individuelle Positionen) ===")
for p in panels:
    print(f"  {p.Name}: Y={p.Placement.Base.y:.3f}")

# Trigger: Assembly-Objekt im Baum doppelklicken (= in den Edit-Modus wechseln).
# Alternativ reproduziert auch:
#   Gui.ActiveDocument.ActiveView.setActiveObject('Assembly', assembly) + assembly.recompute(True)
# oder das Loeschen eines beliebigen, am Graphen beteiligten Objekts + doc.recompute().
assembly.ViewObject.doubleClicked()

print("\n=== NACH dem Trigger (BUG: alle Y-Werte kollabieren auf denselben Wert) ===")
for p in panels:
    print(f"  {p.Name}: Y={p.Placement.Base.y:.3f}")
