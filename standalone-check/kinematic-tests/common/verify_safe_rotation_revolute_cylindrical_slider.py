"""
Gemeinsames Sweep-Hilfsskript (nicht Teil der Testmatrix) - sweept den Perturbations-Winkel
fuer die Startrotation von BoxB durch, fuer Revolute/Cylindrical/Slider (analog zu
test_fixed_joint_flat/verify_safe_rotation.py, aber generisch fuer alle drei, da sie dieselbe
Grundstruktur haben). Siehe Projekt-Memory "reference-fixed-joint-180-degree-bug-rootcause"
fuer den Hintergrund.

Geometrie seit 2026-09-09: Edge9 (Kante) statt Face1 (Nutzerkorrektur: "fuer Revolute/
Cylindrical/Slider nimmt man eine Achse/Kante, keine Flaeche").

Ergebnis (2026-09-09, mit Edge-Geometrie neu vermessen):
- Revolute/Slider zeigen dasselbe saubere Zwei-Wurzel-Verhalten wie zuvor mit Face-Geometrie -
  sicher bei 30 Grad (Revolute kippt zwischen 60-75 und wieder zwischen 120-135 Grad, aber
  nicht monoton dazwischen; Slider zwischen 90-105 Grad).
- Cylindrical zeigt ein GRUNDSAETZLICH ANDERES Verhalten: kein sauberer Zwei-Wurzel-Fall,
  sondern ein kleiner, mit der Startabweichung wachsender Rest-Achsenfehler (~0.14 bei 10 Grad),
  der zwischen ~20 und ~30 Grad abrupt in eine echt falsche Konfiguration kippt (Achsenfehler
  0.8-2.0). test_cylindrical_joint_flat.py nutzt deshalb NUR 10 Grad (nicht 30) UND eine
  gelockerte Toleranz in check_cylindrical() - noch nicht root-cause-analysiert, siehe
  Projekt-Memory.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install common/verify_safe_rotation_revolute_cylindrical_slider.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
import joint_test_utils as jtu


CASES = [
    ("Revolute", 1, jtu.check_revolute,
     jtu.edge_jcs_placement("Edge9", flip=False, extra_z_rotation_deg=30),
     jtu.edge_jcs_placement("Edge9", flip=True, extra_z_rotation_deg=15, standoff=2)),
    ("Cylindrical", 2, jtu.check_cylindrical,
     jtu.edge_jcs_placement("Edge9", flip=False, extra_z_rotation_deg=30),
     jtu.edge_jcs_placement("Edge9", flip=True, extra_z_rotation_deg=15, standoff=2)),
    ("Slider", 3, jtu.check_slider,
     jtu.edge_jcs_placement("Edge9", flip=False, extra_z_rotation_deg=30),
     jtu.edge_jcs_placement("Edge9", flip=True, extra_z_rotation_deg=15, standoff=2)),
]


def try_angle(joint_type_index, check_fn, plc1, plc2, angle_deg):
    doc = App.newDocument(f"SweepFlat{joint_type_index}_{angle_deg}".replace(".", "_"))
    boxA = doc.addObject("Part::Box", "BoxA")
    boxB = doc.addObject("Part::Box", "BoxB")
    doc.recompute()
    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    assembly.addObject(boxA)
    assembly.addObject(boxB)
    doc.recompute()
    boxA.setPropertyStatus("Placement", "ReadOnly")

    jtu.make_joint(assembly, joint_type_index, boxA, boxB, plc1, plc2, elem1="Edge9", elem2="Edge9")

    target_plc = boxA.Placement.multiply(plc1).multiply(plc2.inverse())
    start_rotation = target_plc.Rotation.multiply(App.Rotation(App.Vector(1, 1, 1), angle_deg))
    boxB.Placement = App.Placement(App.Vector(999, -999, 999), start_rotation)
    doc.recompute()
    assembly.solve(False)
    doc.recompute()

    jcs1 = jtu.jcs_world(boxA, plc1)
    jcs2 = jtu.jcs_world(boxB, plc2)
    rel = jtu.relative_jcs(jcs1, jcs2)
    ok = check_fn(rel)
    App.closeDocument(doc.Name)
    return ok


def main():
    for name, type_index, check_fn, plc1, plc2 in CASES:
        print(f"=== {name} ===")
        for angle in [5, 10, 20, 30, 45, 60, 75, 90, 105, 120, 135, 150]:
            ok = try_angle(type_index, check_fn, plc1, plc2, float(angle))
            print(f"  {angle:>4} deg -> {'OK' if ok else 'FALSCHE WURZEL'}")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
