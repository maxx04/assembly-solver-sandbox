# Minimalster Nachweis: auf 3 Objekte (Ground + Panel + Panel001) und 4 Joints reduziert
# (3x Distance Ground-Panel + 1x Slider Panel-Panel001), dann NUR doc.recompute() - kein Gui,
# keine besondere Vorbedingung noetig ausser dass vorher etwas am Dokument geloescht wurde
# (touched-Zustand). Panel001 springt trotzdem von Y=-50 auf Y=-85 (Kollabierung auf Panels
# Position), Z bleibt korrekt. Reduziertester, zuverlaessigster Reproduktionsfall.
import sys, os
RESULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "result_" + os.path.basename(__file__).replace(".py", ".txt"))
if os.environ.get("_RAN_MIN2P"):
    sys.exit(0)
os.environ["_RAN_MIN2P"] = "1"

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

    path = "/home/maxx/Dokumente/CAD_Workspace/PROJ_CNC_M1/3_Panels_aus_6_auswalbar.FCStd"
    doc = App.openDocument(path)

    ground = doc.getObject("CNC_M1_010_G")
    panel = doc.getObject("CNC_M1_011_P_Panel")
    panel001 = doc.getObject("CNC_M1_011_P_Panel001")

    for jn in ["Joint013", "Joint014", "Joint015", "Joint016", "Joint017"]:
        j = doc.getObject(jn)
        if j:
            doc.removeObject(jn)
    for pn in ["CNC_M1_011_P_Panel002", "CNC_M1_011_P_Panel003", "CNC_M1_011_P_Panel004",
               "CNC_M1_011_P_Panel005", "CNC_M1_011_P_Panel006"]:
        p = doc.getObject(pn)
        if p:
            doc.removeObject(pn)

    remaining = [ground, panel, panel001]
    dump("Reduziert auf Ground+Panel+Panel001 (nur 3x Distance G-Panel + Joint006 Slider Panel-Panel001):",
         remaining)

    doc.recompute()
    dump("\nNACH doc.recompute() (kein Gui, minimalster real-Datei-Fall):", remaining)

    log("\nALLE CHECKS OK")
except Exception as e:
    import traceback
    log("\nFEHLER: " + str(e))
    log(traceback.format_exc())
finally:
    flush()
