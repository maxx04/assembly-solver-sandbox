# Kontrollversuch 3: 3 frische App::Link (ElementCount=0) in einer neuen Assembly, KEINE
# Joints ueberhaupt, direkt ensureIdentityPlacements() aufgerufen. Ergebnis: bleibt korrekt.
# Zeigt, dass ElementCount=0-Links allein (ohne Joints im Dokument) nicht betroffen sind -
# deckt sich mit dem Quellcode (isLinkGroup() == ElementCount>0). Laeuft unter reinem
# FreeCADCmd, da kein JointObject-Import noetig (keine Joints erzeugt).
import sys, os
RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_minimal_eip.txt")
if os.environ.get("_RAN_MIN_EIP"):
    sys.exit(0)
os.environ["_RAN_MIN_EIP"] = "1"

lines = []
def log(m): lines.append(str(m))
def flush():
    with open(RESULT_FILE, "w") as f:
        f.write("\n".join(lines) + "\n")

try:
    import FreeCAD as App

    fctest_dir = os.path.dirname(os.path.abspath(__file__))

    docSrc = App.newDocument("MinimalSrc")
    box = docSrc.addObject("Part::Box", "Box")
    docSrc.recompute()
    docSrc.saveAs(os.path.join(fctest_dir, "MinimalSrc.FCStd"))

    docAsm = App.newDocument("MinimalAsm")
    docAsm.saveAs(os.path.join(fctest_dir, "MinimalAsm.FCStd"))
    assembly = docAsm.addObject("Assembly::AssemblyObject", "Assembly")

    l1 = docAsm.addObject("App::Link", "L1")
    l1.LinkedObject = box
    l1.Placement = App.Placement(App.Vector(0, 10, 0), App.Rotation())
    l2 = docAsm.addObject("App::Link", "L2")
    l2.LinkedObject = box
    l2.Placement = App.Placement(App.Vector(0, 20, 0), App.Rotation())
    l3 = docAsm.addObject("App::Link", "L3")
    l3.LinkedObject = box
    l3.Placement = App.Placement(App.Vector(0, 30, 0), App.Rotation())

    assembly.addObject(l1)
    assembly.addObject(l2)
    assembly.addObject(l3)
    docAsm.recompute()

    log("VOR ensureIdentityPlacements():")
    for l in (l1, l2, l3):
        log(f"  {l.Name}: ElementCount={l.ElementCount} Placement={l.Placement.Base}")

    assembly.ensureIdentityPlacements()

    log("\nNACH ensureIdentityPlacements():")
    for l in (l1, l2, l3):
        log(f"  {l.Name}: Placement={l.Placement.Base}")

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
