"""
Zwei Drehgelenke zwischen dem geerdeten BoxA und BoxB (Regel C7, Befund 2026-11-17, Anlass
FreeCAD#32302):

- koaxial: beide Gelenke auf derselben Achse (Scharnier mit zwei Lagern) - BoxB bleibt um die
  Achse frei drehbar, beide Gelenke muessen erfuellt sein.
- parallel: Achsen Edge9 und Edge10 - BoxB liegt fest (genau auf BoxA).

Die Gelenk-Koordinatensysteme beider Kisten sind gleich gerichtet, damit die Ziellage (BoxB auf
BoxA) gleich gerichtete Achsen hat und check_revolute() streng pruefen kann.

BoxB startet schraeg verdreht (Achse nicht parallel zur Gelenkachse). Vorher entschied
OndselSolver in der unfertigen Startlage, welche Bedingungen des zweiten Gelenks redundant sind,
verwarf dabei die in der Endlage benoetigte und das Redundanz-Gedaechtnis hielt die Auswahl fest:
BoxB blieb um den Startanteil um die Achse verdreht ("Joint001 nicht erfuellt").

Aufruf: ./common/run-test.sh <freecad-install-dir> test_two_revolutes_flat/test_two_revolutes_flat.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402

REVOLUTE = 1
TOL = 1e-6
STARTS = [((1, 1, 0), 25), ((1, 2, 3), 60), ((0, 1, 1), 40)]


def run(case, axis, deg, index):
    path = os.path.join(THIS_DIR, "fcstd-output", f"{case}{index}", "model.FCStd")
    doc, asm, a, b = jtu.new_flat_two_box_assembly(f"TR{case}{index}", path)
    b.Placement = App.Placement(App.Vector(3, 20, 4), App.Rotation(App.Vector(*axis), deg))
    edge2, standoff = ("Edge9", 3) if case == "koaxial" else ("Edge10", 0)
    pairs = [
        (jtu.edge_jcs_placement("Edge9"), jtu.edge_jcs_placement("Edge9"), "Edge9"),
        (
            jtu.edge_jcs_placement(edge2, standoff=standoff),
            jtu.edge_jcs_placement(edge2, standoff=standoff),
            edge2,
        ),
    ]
    for p1, p2, edge in pairs:
        jtu.make_joint(asm, REVOLUTE, a, b, p1, p2, edge, edge)
    doc.recompute()
    rc = asm.solve()
    ok = rc == 0
    worst = 0.0
    for p1, p2, _edge in pairs:
        rel = jtu.relative_jcs(jtu.jcs_world(a, p1), jtu.jcs_world(b, p2))
        worst = max(worst, rel.Base.Length)
        ok &= jtu.check_revolute(rel)
    states = list(asm.getRedundancyStates()) if hasattr(asm, "getRedundancyStates") else []
    conflicts = [s for s in states if "Konflikt" in s]
    ok &= not conflicts
    if case == "parallel":
        ok &= b.Placement.Base.Length < TOL and abs(b.Placement.Rotation.Angle) < 1e-6
    print(
        f"{case} Start {deg} Grad um {axis}: rc={rc}, Gelenkabstand {worst:.2e} mm, "
        f"Widerspruch {conflicts or '-'} -> {'OK' if ok else 'FEHLER'}"
    )
    doc.save()
    App.closeDocument(doc.Name)
    return ok


def main():
    ok = True
    i = 0
    for case in ("koaxial", "parallel"):
        for axis, deg in STARTS:
            ok &= run(case, axis, deg, i)
            i += 1
    print("RESULT: PASS" if ok else "RESULT: FAIL (siehe Ausgabe oben)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
