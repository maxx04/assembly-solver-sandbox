"""Rundumlauf Planar (Abstand Ebene-Ebene) und Parallel: 4 Drehachsen x 11 Startwinkel x beide
Referenz-Reihenfolgen (2026-11-21). Erwartet: Loesung auf der NAEHEREN Seite (zI.zJ hat das
Vorzeichen der Startlage), Achsen parallel, beim Abstand 0 mm.

Vor 2026-11-21 scheiterten 37 von 176 Faellen (Planar sprang auf die andere Seite oder divergierte);
Ursachen: Richtungskosinusse erlauben zJ = +-zI, und mit Kippwinkeln (atan2, unabhaengig von |e|)
lief |e| der Euler-Parameter auseinander - behoben durch Projektion auf |e| = 1 je Newton-Schritt
(OndselSolver PartFrame::setqsu/setqsulam).

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_planar_parallel_sweep_flat/test_planar_parallel_sweep_flat.py
"""
import os, sys
import FreeCAD as App
T=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, os.path.join(T,"common")); sys.path.insert(0, os.path.join(T,"test_planar_parallel_orientation_flat"))
import joint_test_utils as jtu
import test_planar_parallel_orientation_flat as t
def main():
    bad=[]; n=0
    for axis in [(1,0,0),(0,1,0),(1,1,0),(1,2,3)]:
      for theta in [0,20,45,70,85,95,110,130,150,170,180]:
        for swap in (False, True):
          for jtype in (t.DISTANCE, t.PARALLEL):
            n+=1
            path=os.path.join(os.path.dirname(os.path.abspath(__file__)),"fcstd-output","c.FCStd")
            doc, asm, A, B = jtu.new_flat_two_box_assembly(f"PS{n}", path)
            tilt=App.Placement(App.Vector(3,-2,4), App.Rotation(App.Vector(*axis), theta))
            B.Placement = A.Placement.multiply(t.PLC1).multiply(tilt).multiply(t.PLC2.inverse())
            if swap: j=jtu.make_joint(asm, jtype, B, A, t.PLC2, t.PLC1, "Face5","Face6")
            else: j=jtu.make_joint(asm, jtype, A, B, t.PLC1, t.PLC2, "Face6","Face5")
            z1=jtu.jcs_world(A,t.PLC1).Rotation.multVec(App.Vector(0,0,1)); z2=jtu.jcs_world(B,t.PLC2).Rotation.multVec(App.Vector(0,0,1))
            exp = 1 if z1.dot(z2)>0 else -1
            if jtype==t.DISTANCE: j.Distance=0.0
            doc.recompute(); rc=asm.solve(); asm.solve()
            z1=jtu.jcs_world(A,t.PLC1).Rotation.multVec(App.Vector(0,0,1)); z2=jtu.jcs_world(B,t.PLC2).Rotation.multVec(App.Vector(0,0,1))
            dot=z1.dot(z2); dist=abs((jtu.jcs_world(B,t.PLC2).Base-jtu.jcs_world(A,t.PLC1).Base).dot(z1))
            ok = rc==0 and abs(abs(dot)-1)<1e-6 and (dot>0)==(exp>0) and (jtype!=t.DISTANCE or dist<1e-5)
            if not ok: bad.append((("Abstand" if jtype==t.DISTANCE else "Parallel"),axis,theta,swap,rc,round(dot,4),exp,dist))
            App.closeDocument(doc.Name)
    print("SWEEP faelle", n, "fehler", len(bad))
    for b in bad: print("BAD", b)
    print("RESULT: PASS" if not bad else "RESULT: FAIL")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
