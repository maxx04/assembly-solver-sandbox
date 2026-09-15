"""
Einmaliges Hilfsskript (KEIN Teil der automatisierten Testmatrix) - sweept den Perturbations-
Winkel fuer mirror_boxB's Startrotation in build_fixture_test_fixed_nested_flex.py durch, NACH
dem Fix fuer den inneren Joint (subB wird jetzt vorab korrekt platziert) - da diese Aenderung
die Systemdynamik fuer den aeusseren Joint veraendert hat (Rigid=False AssemblyLink loest
vermutlich innere+aeussere Zwangsbedingungen gemeinsam), muss der sichere Perturbations-Bereich
fuer den AEUSSEREN Joint NEU bestimmt werden - der alte, isoliert getestete Wert (30 Grad, siehe
verify_fixed_joint_flat_safe_rotation.py) gilt hier nicht mehr unveraendert.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_fixed_nested_flex/verify_safe_rotation.py

**Zwischenstand (2026-09-09), Untersuchung ZURUECKGESTELLT (Nutzerauftrag):** ALLE getesteten
Winkel (0 bis 120 Grad) schlagen fehl, auch der exakte Zielwinkel (0 Grad Perturbation)! Ein
Debug-Ausdruck zeigte: `mirror_boxB.Placement` hat schon VOR der expliziten Ueberschreibung
unten (direkt nach dem automatischen Presolve bei Joint-Erstellung) exakt denselben Wert wie
das spaetere Endergebnis - die explizite Ueberschreibung scheint beim SPIEGEL-Objekt (anders
als beim normalen Objekt im flachen Test) wirkungslos zu sein, vermutlich weil ein Link-
Synchronisierungsmechanismus sie sofort wieder ueberschreibt, bevor der eigentliche solve()-
Aufruf greift. Das fruehere scheinbare "Erfolgsergebnis" (BoxB korrekt bei 30 Grad, VOR dem
subB-Fix in build_fixture_test_fixed_nested_flex.py) war vermutlich Zufall, gekoppelt an subB's
(damals noch falschen) Wert, nicht das Ergebnis der Rotations-Vorbelegung. Naechster Schritt bei
Wiederaufnahme: klaeren, warum/ob Placement-Zuweisungen an ein AssemblyLink-Spiegelobjekt anders
behandelt werden als an ein normales Top-Level-Objekt.
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu
import nested_test_utils as ntu
from test_fixed_nested_flex import INNER_PLC1, INNER_PLC2, OUTER_PLC1, OUTER_PLC2


def try_angle(angle_deg):
    sub_path = f"/tmp/sweep_sub_{angle_deg}.FCStd"
    grand_path = f"/tmp/sweep_grand_{angle_deg}.FCStd"
    sub_doc, sub_asm, subA, subB = ntu.new_sub_assembly_doc(f"SweepSub{angle_deg}".replace(".", "_"), sub_path)
    jtu.make_joint(sub_asm, 0, subA, subB, INNER_PLC1, INNER_PLC2)
    subB.Placement = subA.Placement.multiply(INNER_PLC1).multiply(INNER_PLC2.inverse())
    sub_doc.recompute()

    grand_doc, grand_asm, boxC, sublink = ntu.new_grand_assembly_with_sublink(
        f"SweepGrand{angle_deg}".replace(".", "_"), sub_asm, sub_path, grand_path
    )
    mirror_boxA = ntu.get_mirror(sublink, subA.Label)
    mirror_boxB = ntu.get_mirror(sublink, subB.Label)

    outer_joint = jtu.make_joint(grand_asm, 0, boxC, mirror_boxB, OUTER_PLC1, OUTER_PLC2)
    outer_joint.Label = "Joint"
    for child in grand_asm.OutList:
        if child.TypeId == "Assembly::JointGroup":
            child.Label = "Joints"
    grand_doc.recompute()

    target_plc = boxC.Placement.multiply(OUTER_PLC1).multiply(OUTER_PLC2.inverse())
    start_rotation = target_plc.Rotation.multiply(App.Rotation(App.Vector(1, 1, 1), angle_deg))
    mirror_boxB.Placement = App.Placement(App.Vector(999, -999, 999), start_rotation)
    grand_doc.recompute()
    grand_asm.solve(False)
    grand_doc.recompute()

    boxB_expected = boxC.Placement.multiply(OUTER_PLC1).multiply(OUTER_PLC2.inverse())
    boxA_expected = boxB_expected.multiply(INNER_PLC2).multiply(INNER_PLC1.inverse())
    boxB_actual = mirror_boxB.Placement
    boxA_actual = mirror_boxA.Placement

    def rot_diff(expected, actual):
        return abs(
            expected.Rotation.multiply(actual.Rotation.inverted()).Angle
        ) * 180.0 / 3.141592653589793

    rd_b = rot_diff(boxB_expected, boxB_actual)
    rd_a = rot_diff(boxA_expected, boxA_actual)

    App.closeDocument(grand_doc.Name)
    App.closeDocument(sub_doc.Name)
    return rd_b, rd_a


def main():
    print(f"{'Winkel':>8} | {'BoxB-Diff':>10} | {'BoxA-Diff':>10} | Ergebnis")
    print("-" * 55)
    for angle in [0, 5, 10, 20, 30, 45, 60, 90, 120]:
        rd_b, rd_a = try_angle(float(angle))
        ok = rd_b < 1.0 and rd_a < 1.0
        print(f"{angle:>8} | {rd_b:>10.4f} | {rd_a:>10.4f} | {'OK' if ok else 'FALSCHE WURZEL'}")

    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
