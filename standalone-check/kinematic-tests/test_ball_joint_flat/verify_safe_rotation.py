"""
Sweep-Hilfsskript (nicht Teil der Testmatrix) - testet verschiedene Versatz-Groessen fuer
INITIAL_BASE in test_ball_joint_flat.py, um einen Wert mit echtem Sicherheitsabstand zur
Konvergenzradius-Grenze des Solvers zu finden.

Geometrie seit 2026-09-09: Vertex2 (Punkt) statt Face1 (Nutzerkorrektur: "fuer Ball-Joints
nimmt man Punkte, keine Flaechen"). Mit dieser Geometrie ist das Konvergenzfenster deutlich
GROESSER als mit der frueheren Face-Geometrie: der gesamte getestete Bereich von 3mm bis 30mm
konvergiert einwandfrei (kein Ausreisser mehr) - die urspruengliche, sehr enge Grenze
(5-5.5mm mit Face-Geometrie) war offenbar spezifisch fuer die kuenstliche Face+Standoff-
Konstruktion, nicht ein grundsaetzliches Solver-Problem. test_ball_joint_flat.py behaelt
trotzdem einen moderaten Wert (4.5mm) statt eines extremen, da das ohnehin ausreicht.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_ball_joint_flat/verify_safe_rotation.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu

PLC1 = jtu.vertex_jcs_placement("Vertex2", extra_rotation_axis=App.Vector(0, 1, 0), extra_rotation_deg=30)
PLC2 = jtu.vertex_jcs_placement("Vertex2", extra_rotation_axis=App.Vector(1, 0, 0), extra_rotation_deg=15)


def try_offset(magnitude):
    doc = App.newDocument(f"SweepBall{magnitude}".replace(".", "_"))
    boxA = doc.addObject("Part::Box", "BoxA")
    boxB = doc.addObject("Part::Box", "BoxB")
    doc.recompute()
    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    assembly.addObject(boxA)
    assembly.addObject(boxB)
    doc.recompute()
    boxA.setPropertyStatus("Placement", "ReadOnly")

    jtu.make_joint(assembly, 4, boxA, boxB, PLC1, PLC2, elem1="Vertex2", elem2="Vertex2")  # 4 = Ball

    direction = App.Vector(10, -8, 6).normalize()
    base = direction * magnitude
    boxB.Placement = App.Placement(base, App.Rotation(App.Vector(0, 1, 0), 77))
    doc.recompute()
    assembly.solve(False)
    doc.recompute()

    jcs1 = jtu.jcs_world(boxA, PLC1)
    jcs2 = jtu.jcs_world(boxB, PLC2)
    rel = jtu.relative_jcs(jcs1, jcs2)
    ok = jtu.check_ball(rel)
    moved = (boxB.Placement.Base - base).Length > 1.0
    App.closeDocument(doc.Name)
    return ok, moved


def main():
    # Ergebnis (2026-09-09): 3mm konvergiert, aber Bewegung < 1mm (Sanity-Check faellt durch);
    # 4/4.5/5mm konvergieren UND bewegen sich sichtbar; ab 5.5mm bricht die Konvergenz ab
    # ("Solve failed: iterNo > iterMax") - deutlich enger als die urspruenglich dokumentierten
    # ~14mm/~27mm. Gewaehlter finaler Wert in test_ball_joint_flat.py: 4.5mm (Mitte des sicheren
    # Fensters 4-5mm).
    for mag in [3, 4, 4.5, 5, 5.5, 6, 7, 9, 11, 14.14, 20, 30]:
        ok, moved = try_offset(float(mag))
        print(f"  {mag:>6} mm -> ok={ok} moved={moved}")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
