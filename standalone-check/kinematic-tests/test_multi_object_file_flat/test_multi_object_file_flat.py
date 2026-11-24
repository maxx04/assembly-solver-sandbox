"""
Mehrere Objekte in EINER Datei (2026-11-24, Frage zur FCProject-Regel "in einer Datei nur ein
Objekt"): der Solver ordnet Teile ueber Objekte zu, nicht ueber Dokumente.

  1. Platte und Quader in einer Teile-Datei, beide per App::Link in eine Baugruppe, der Quader
     zweimal, je ein Drehgelenk: Loesen, Ziehen (nur der gezogene Quader dreht), Quelle bleibt.
  2. Unter- und Ober-Baugruppe in DERSELBEN Datei, Koerper aus einer gemeinsamen Teile-Datei; die
     obere baut die untere flexibel ein und haelt deren BoxB per Fixed: Fixed und inneres
     Drehgelenk im Spiegel erfuellt, Original in der Unterbaugruppe unveraendert.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_multi_object_file_flat/test_multi_object_file_flat.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

V = App.Vector
OUT = os.path.join(THIS_DIR, "fcstd-output")


def bodies_in_one_file(out_dir):
    import JointObject
    import UtilsAssembly

    os.makedirs(out_dir, exist_ok=True)
    parts = App.newDocument("teile")
    plate = parts.addObject("Part::Box", "Platte")
    plate.Length, plate.Width, plate.Height = 80, 60, 5
    block = parts.addObject("Part::Box", "Quader")
    block.Length, block.Width, block.Height = 20, 10, 10
    block.Placement.Base = V(100, 0, 0)
    parts.recompute()
    parts.saveAs(os.path.join(out_dir, "teile.FCStd"))

    doc = App.newDocument("baugruppe")
    doc.saveAs(os.path.join(out_dir, "baugruppe.FCStd"))
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    links = {}
    for src in (plate, block, block):  # Quader zweimal eingebaut
        link = asm.newObject("App::Link", src.Name)
        link.LinkedObject = src
        links.setdefault(src.Name, []).append(link)
    lp, (lq1, lq2) = links["Platte"][0], links["Quader"]
    lq1.Placement.Base = V(10, 10, 30)
    lq2.Placement.Base = V(50, 30, 40)
    doc.recompute()
    jtu.ground_object(lp)
    rev = JointObject.JointTypes.index("Revolute")
    j1 = jtu.make_joint(asm, rev, lp, lq1, App.Placement(V(20, 20, 5), App.Rotation()), App.Placement(), "Face6", "Face5")
    j2 = jtu.make_joint(asm, rev, lp, lq2, App.Placement(V(60, 40, 5), App.Rotation()), App.Placement(), "Face6", "Face5")
    doc.recompute()
    rc = asm.solve()
    ok = rc == 0
    for name, link, want in (("Quader 1", lq1, V(20, 20, 5)), ("Quader 2", lq2, V(60, 40, 5))):
        d = (link.Placement.Base - want).Length
        print(f"{name}: Lage {link.Placement.Base}, Abstand zum Gelenkpunkt {d:.2e}")
        ok &= d < 1e-6
    print(f"Platte unbewegt: {lp.Placement.Base}")
    ok &= lp.Placement.Base.Length < 1e-9

    # Identitaeten: jeder Link eigener Solver-Koerper?
    try:
        print("IdentityGraph:", asm.exportIdentityGraphDot()[:0] or "ok")
    except Exception as e:  # noqa: BLE001
        print("IdentityGraph-Export:", e)

    # Ziehen: Quader 1 drehen (Drehgelenk) - Quader 2 darf sich nicht mitbewegen
    before2 = App.Placement(lq2.Placement)
    part, moving, ref, bypassed = asm.getDragJointOfPart(lq1)
    print("Zieh-Gelenk:", moving.Name if moving else None, "Teil:", part.Name if part else None)
    asm.preDrag([lq1])
    start = App.Placement(lq1.Placement)
    for k in range(1, 11):
        p = App.Placement(start)
        p.Rotation = App.Rotation(V(0, 0, 1), 9 * k).multiply(start.Rotation)
        lq1.Placement = p
        asm.doDragStep()
    asm.postDrag()
    moved2 = (lq2.Placement.Base - before2.Base).Length + abs(lq2.Placement.Rotation.Angle - before2.Rotation.Angle)
    print(f"nach Ziehen: Quader 1 Winkel {lq1.Placement.Rotation.Angle * 57.2958:.1f} Grad, Quader 2 bewegt {moved2:.2e}")
    ok &= moved2 < 1e-9 and abs((lq1.Placement.Base - V(20, 20, 5)).Length) < 1e-6
    print("Quelle unveraendert:", plate.Placement.Base, block.Placement.Base)
    ok &= block.Placement.Base == V(100, 0, 0)
    for d in list(App.listDocuments()):
        App.closeDocument(d)
    return ok


def assemblies_in_one_file(out_dir):
    import JointObject

    App.ParamGet("User parameter:BaseApp/Preferences/Document").SetBool("DuplicateLabels", True)
    os.makedirs(out_dir, exist_ok=True)
    parts = App.newDocument("teile")
    for name in ("BoxA", "BoxB", "BoxC"):
        b = parts.addObject("Part::Box", name)
        b.Length, b.Width, b.Height = 20, 10, 10
    parts.recompute()
    parts.saveAs(os.path.join(out_dir, "teile.FCStd"))

    doc = App.newDocument("zwei_baugruppen")
    doc.saveAs(os.path.join(out_dir, "zwei_baugruppen.FCStd"))
    sub = doc.addObject("Assembly::AssemblyObject", "Sub")
    la = sub.newObject("App::Link", "BoxA")
    la.LinkedObject = parts.getObject("BoxA")
    lb = sub.newObject("App::Link", "BoxB")
    lb.LinkedObject = parts.getObject("BoxB")
    lb.Placement.Base = V(30, 0, 0)
    doc.recompute()
    jtu.ground_object(la)
    rev = JointObject.JointTypes.index("Revolute")
    fixed = JointObject.JointTypes.index("Fixed")
    jtu.make_joint(sub, rev, la, lb, App.Placement(V(20, 5, 5), App.Rotation(V(0, 1, 0), 90)),
                   App.Placement(V(0, 5, 5), App.Rotation(V(0, 1, 0), 90)), "Face2", "Face1")
    doc.recompute()
    rc_sub = sub.solve()
    sub_b = App.Placement(lb.Placement)

    top = doc.addObject("Assembly::AssemblyObject", "Top")
    lc = top.newObject("App::Link", "BoxC")
    lc.LinkedObject = parts.getObject("BoxC")
    lc.Placement.Base = V(0, 50, 0)
    doc.recompute()
    jtu.ground_object(lc)
    sublink = top.newObject("Assembly::AssemblyLink", "Sub")
    sublink.LinkedObject = sub
    sublink.Rigid = False
    doc.recompute()
    mirrors = {c.Label: c for c in sublink.Group if c.TypeId == "App::Link"}
    print("Spiegel:", sorted(mirrors), "Sub-Gruppe:", [o.Label for o in sublink.Group])
    mb = mirrors.get("BoxB")
    ma = mirrors.get("BoxA")
    if mb is None or ma is None:
        print("keine Spiegel")
        return False
    # BoxB-Spiegel fest an BoxC, um 90 Grad gedreht (dreht damit BoxB um das innere Drehgelenk)
    jtu.make_joint(top, fixed, lc, mb, App.Placement(V(0, 0, 10), App.Rotation(V(1, 0, 0), 30)),
                   App.Placement(), "Face6", "Face5")
    doc.recompute()
    rc_top = top.solve()
    doc.recompute()
    want_b = lc.Placement.multiply(App.Placement(V(0, 0, 10), App.Rotation(V(1, 0, 0), 30)))
    gb = mb.getGlobalPlacement() if hasattr(mb, "getGlobalPlacement") else mb.Placement
    d_fixed = (sublink.Placement.multiply(mb.Placement).Base - want_b.Base).Length
    print(f"solve sub={rc_sub} top={rc_top}; Fixed erfuellt (BoxB-Spiegel am Ziel): {d_fixed:.2e}")
    # inneres Drehgelenk im Spiegel: Achsenpunkt von BoxA (20,5,5) und BoxB (0,5,5) gleich
    pa = sublink.Placement.multiply(ma.Placement).multVec(V(20, 5, 5))
    pb = sublink.Placement.multiply(mb.Placement).multVec(V(0, 5, 5))
    print(f"inneres Drehgelenk im Spiegel: Achspunkte {(pa - pb).Length:.2e}")
    unchanged = (lb.Placement.Base - sub_b.Base).Length < 1e-9 and lb.Placement.Rotation.isSame(sub_b.Rotation, 1e-12)
    print(f"Original-BoxB in Sub unveraendert: {unchanged}; Original-BoxA {la.Placement.Base}")
    print("Quellen:", [tuple(parts.getObject(n).Placement.Base) for n in ("BoxA", "BoxB", "BoxC")])
    ok = rc_sub == 0 and rc_top == 0 and d_fixed < 1e-6 and (pa - pb).Length < 1e-6 and unchanged
    for d in list(App.listDocuments()):
        App.closeDocument(d)
    return ok


def main():
    ok = bodies_in_one_file(os.path.join(OUT, "koerper"))
    ok &= assemblies_in_one_file(os.path.join(OUT, "baugruppen"))
    print("RESULT: PASS" if ok else "RESULT: FAIL (siehe oben)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
