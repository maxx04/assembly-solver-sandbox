# Kontrollversuch 1: gleiche Datei wie repro_real_file.py, aber ALLE Joints vorher entfernt.
# Ergebnis: ensureIdentityPlacements() bleibt dann ein No-Op - zeigt, dass Joints im Dokument
# eine notwendige Bedingung fuer den Bug sind.
import sys, os

RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_nojoints.txt")
if os.environ.get("_RAN_NOJOINTS"):
    sys.exit(0)
os.environ["_RAN_NOJOINTS"] = "1"

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

    log("VOR Joint-Entfernung:")
    for l in panels:
        log(f"  {l.Name}: {l.Placement.Base}")

    # Alle Joints + JointGroup entfernen
    jg = doc.getObject("Joints")
    if jg:
        for j in list(jg.Group):
            doc.removeObject(j.Name)
        doc.removeObject(jg.Name)
    gj = doc.getObject("GroundedJoint")
    if gj:
        doc.removeObject(gj.Name)

    doc.recompute()

    log("\nNACH Joint-Entfernung (vor ensureIdentityPlacements):")
    for l in panels:
        log(f"  {l.Name}: {l.Placement.Base}")

    assembly.ensureIdentityPlacements()

    log("\nNACH ensureIdentityPlacements() OHNE jegliche Joints:")
    for l in panels:
        log(f"  {l.Name}: {l.Placement.Base}")

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
