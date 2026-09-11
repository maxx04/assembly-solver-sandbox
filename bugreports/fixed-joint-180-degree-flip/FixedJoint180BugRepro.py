"""
Minimale, eigenstaendige Reproduktion (keine Abhaengigkeiten ausserhalb von FreeCAD selbst):

Assembly Fixed-Joint kann nach dem Loesen um 180 Grad um die gemeinsame Joint-Achse verdreht
sein, statt die per Placement1/Placement2 vorgegebene Zielausrichtung zu erreichen - reproduzierbar
unabhaengig von der Groesse der vorherigen Fehlplatzierung (5mm bis 999mm liefern IDENTISCH
dasselbe falsche Ergebnis), was zeigt, dass es sich nicht um ein Konvergenzradius-Problem
handelt, sondern um eine echte Mehrdeutigkeit in der Zwangsbedingung selbst.

Placement1/Placement2 hier sind bewusst so gewaehlt, dass ihre lokalen Z-Achsen ENTGEGENGESETZT
zueinander zeigen - das ist GENAU die Konfiguration, die entsteht, wenn zwei einander
zugewandte Flaechen zweier Bauteile ueber eine "Fixed"-Verbindung verbunden werden sollen
(reale Baugruppen-Situation: zwei Flaechen sollen aufeinander zu liegen kommen, nicht
ineinander ueberlappen - siehe UtilsAssembly.py's flipPlacement()/arePlacementSameDir(), die
genau dafuer sorgen sollen).

Root Cause (im Quellcode nachvollzogen, src/3rdParty/OndselSolver/OndselSolver/):
    FixedJoint::initializeGlobally() erzwingt die Rotation ueber genau 3 Bedingungen:
        DirectionCosineConstraintIqcJqc(frmI, frmJ, 1, 0)   =>  Y_I . X_J == 0
        DirectionCosineConstraintIqcJqc(frmI, frmJ, 2, 0)   =>  Z_I . X_J == 0
        DirectionCosineConstraintIqcJqc(frmI, frmJ, 2, 1)   =>  Z_I . Y_J == 0
    Diese drei Kreuz-Achsen-Orthogonalitaetsbedingungen legen die Rotation NICHT eindeutig
    fest: sowohl "X_J=X_I, Y_J=Y_I, Z_J=Z_I" (korrekt) ALS AUCH "X_J=-X_I, Y_J=-Y_I, Z_J=+Z_I"
    (180 Grad um die gemeinsame Z-Achse gedreht) erfuellen alle drei Gleichungen exakt gleich
    gut. Das Newton-Verfahren konvergiert zu welcher der zwei Wurzeln naeher am Startwert
    liegt - bei einer "flip"-Konfiguration (wie sie fuer eine reale Flaeche-auf-Flaeche-Montage
    noetig ist) liegt die FALSCHE Wurzel oft naeher an einer neutralen/identischen
    Ausgangsrotation.

    RevoluteJoint/CylindricalJoint haben dieselbe strukturelle Schwaeche (nur 2 statt 3
    Bedingungen: Z_I.X_J==0, Z_I.Y_J==0 - laesst Z_J=+-Z_I offen).
"""
import sys
import FreeCAD as App


def main():
    doc = App.newDocument("FixedJoint180Bug")

    boxA = doc.addObject("Part::Box", "BoxA")
    boxB = doc.addObject("Part::Box", "BoxB")
    doc.recompute()

    assembly = doc.addObject("Assembly::AssemblyObject", "Assembly")
    assembly.addObject(boxA)
    assembly.addObject(boxB)
    doc.recompute()

    boxA.setPropertyStatus("Placement", "ReadOnly")  # BoxA erden

    import UtilsAssembly
    import JointObject

    joint_group = UtilsAssembly.getJointGroup(assembly)
    joint = joint_group.newObject("App::FeaturePython", "Joint")
    JointObject.Joint(joint, 0)  # 0 = "Fixed"
    if App.GuiUp:
        JointObject.ViewProviderJoint(joint.ViewObject)

    joint.Reference1 = (boxA, ["Face1", "Face1"])
    joint.Reference2 = (boxB, ["Face1", "Face1"])
    joint.Detach1 = True
    joint.Detach2 = True

    # Placement1: JCS auf BoxAs Face1 (Zentrum (0,5,5), Normale (-1,0,0)) - die "Anker"-Seite.
    plc1 = App.Placement(App.Vector(0, 5, 5), App.Rotation(App.Vector(0, 0, 1), App.Vector(-1, 0, 0)))
    # Placement2: JCS auf BoxBs Face1, aber mit ENTGEGENGESETZTER (geflippter) Normalen-
    # Richtung (+1,0,0 statt -1,0,0) - das ist die Konfiguration, die zwei aufeinander
    # zuliegende (nicht ineinander verschachtelte) Flaechen ergibt.
    plc2 = App.Placement(App.Vector(0, 5, 5), App.Rotation(App.Vector(0, 0, 1), App.Vector(1, 0, 0)))
    joint.Placement1 = plc1
    joint.Placement2 = plc2

    boxB.Placement = App.Placement(App.Vector(999, -999, 999), App.Rotation())
    doc.recompute()
    assembly.solve(False)
    doc.recompute()

    expected = boxA.Placement.multiply(plc1).multiply(plc2.inverse())
    actual = boxB.Placement
    pos_diff = (expected.Base - actual.Base).Length
    rot_diff = abs(expected.Rotation.multiply(actual.Rotation.inverted()).Angle) * 180.0 / 3.141592653589793

    print("BoxB.Placement (aus Placement1/Placement2 analytisch erwartet):", expected)
    print("BoxB.Placement (tatsaechlich vom Solver berechnet):           ", actual)
    print(f"Positions-Differenz: {pos_diff:.6f} mm")
    print(f"Rotations-Differenz: {rot_diff:.6f} deg  <-- sollte 0 sein, ist aber 180")

    out_path = "/tmp/FixedJoint180BugRepro_safemode.FCStd"
    doc.saveAs(out_path)
    print("Gespeichert:", out_path)

    if rot_diff > 90:
        print("BUG REPRODUZIERT.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
