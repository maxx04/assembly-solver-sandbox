"""
Einmaliges Hilfsskript (KEIN Teil der automatisierten Testmatrix, kein test_*.py-Name) -
sweept den Perturbations-Winkel fuer die Startrotation von test_fixed_joint_flat.py's Fixture
durch, um empirisch zu bestimmen, ab welchem Winkel die Startrotation "naeher" an der falschen
(180 Grad verdrehten) statt der richtigen Wurzel liegt - siehe Projekt-Memory
"reference-fixed-joint-180-degree-bug-rootcause" fuer den vollen Hintergrund.

Nutzt exakt PLC1/PLC2 aus test_fixed_joint_flat.py, baut fuer jeden Testwinkel eine eigene
In-Memory-Baugruppe (kein Fixture-Datei-Schreiben), loest, und meldet die Rotationsdifferenz.

Aufruf:
    ./run-test.sh /home/maxx/freecad-sandbox/install verify_fixed_joint_flat_safe_rotation.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import test_fixed_joint_flat as tfjf


def try_angle(angle_deg):
    doc = App.newDocument(f"SafeRotSweep{angle_deg}")
    boxA = doc.addObject("Part::Box", "BoxA")
    boxB = doc.addObject("Part::Box", "BoxB")
    doc.recompute()

    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    assembly.addObject(boxA)
    assembly.addObject(boxB)
    doc.recompute()
    boxA.setPropertyStatus("Placement", "ReadOnly")

    import UtilsAssembly
    import JointObject

    joint_group = UtilsAssembly.getJointGroup(assembly)
    joint = joint_group.newObject("App::FeaturePython", "Joint")
    JointObject.Joint(joint, 0)
    if App.GuiUp:
        JointObject.ViewProviderJoint(joint.ViewObject)

    joint.Reference1 = (boxA, ["Face1", "Face1"])
    joint.Reference2 = (boxB, ["Face1", "Face1"])
    joint.Detach1 = True
    joint.Detach2 = True
    joint.Placement1 = tfjf.PLC1
    joint.Placement2 = tfjf.PLC2

    target_plc = boxA.Placement.multiply(tfjf.PLC1).multiply(tfjf.PLC2.inverse())
    start_rotation = target_plc.Rotation.multiply(App.Rotation(App.Vector(1, 1, 1), angle_deg))
    boxB.Placement = App.Placement(App.Vector(999, -999, 999), start_rotation)
    doc.recompute()

    assembly.solve(False)
    doc.recompute()

    expected = boxA.Placement.multiply(tfjf.PLC1).multiply(tfjf.PLC2.inverse())
    actual = boxB.Placement
    rot_diff = abs(
        expected.Rotation.multiply(actual.Rotation.inverted()).Angle
    ) * 180.0 / 3.141592653589793

    App.closeDocument(doc.Name)
    return rot_diff


def main():
    print(f"{'Winkel (deg)':>14} | {'Rotations-Differenz (deg)':>26} | Ergebnis")
    print("-" * 60)
    first_bad = None
    for angle in [5, 10, 20, 30, 45, 60, 75, 90, 105, 120, 135, 150, 160, 170, 175]:
        rot_diff = try_angle(float(angle))
        ok = rot_diff < 1.0
        print(f"{angle:>14} | {rot_diff:>26.6f} | {'OK' if ok else 'FALSCHE WURZEL'}")
        if not ok and first_bad is None:
            first_bad = angle

    print()
    if first_bad is None:
        print("Kein Umschlagpunkt im getesteten Bereich (5-175 Grad) gefunden.")
    else:
        print(f"Umschlagpunkt liegt zwischen dem letzten OK-Wert und {first_bad} Grad.")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
