"""
build_fixture.py - baut die festen Ausgangsdateien fuer test_nested_limit_change_in_original.py
EINMALIG: Mid (BoxA geerdet -[Slider, Grenze]- BoxB bei Gelenkwert START_VALUE), Grand (BoxC
geerdet -[Fixed]- BoxA-Instanz, Mid flexibel eingebaut).

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_nested_limit_change_in_original/build_fixture.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
import nested_test_utils as ntu  # noqa: E402
from test_nested_limit_change_in_original import (  # noqa: E402
    LIMIT, OUTER_PLC1, OUTER_PLC2, SLIDER_PLC1, SLIDER_PLC2, START_VALUE, fixture_paths)


def main():
    import UtilsAssembly
    paths = fixture_paths()
    os.makedirs(os.path.dirname(paths["mid"]), exist_ok=True)
    mid_doc = App.newDocument("LimitChangeMid")
    mid_doc.saveAs(paths["mid"])
    box_paths = jtu.ensure_box_bodies_in(os.path.dirname(paths["mid"]), ["BoxA", "BoxB"])
    boxA = jtu.link_external_box(mid_doc, "BoxA", box_paths["BoxA"])
    boxB = jtu.link_external_box(mid_doc, "BoxB", box_paths["BoxB"])
    mid_asm = mid_doc.addObject("Assembly::AssemblyObject", "Assembly")
    mid_asm.addObject(boxA)
    mid_asm.addObject(boxB)
    UtilsAssembly.getJointGroup(mid_asm)
    mid_doc.recompute()
    jtu.ground_object(boxA)
    joint = jtu.make_joint(mid_asm, 3, boxA, boxB, SLIDER_PLC1, SLIDER_PLC2)
    joint.EnableLengthMin = True
    joint.LengthMin = LIMIT[0]
    joint.EnableLengthMax = True
    joint.LengthMax = LIMIT[1]
    jcs = boxA.Placement.multiply(SLIDER_PLC1)
    jcs.Base = jcs.Base + jcs.Rotation.multVec(App.Vector(0, 0, START_VALUE))
    boxB.Placement = jcs.multiply(SLIDER_PLC2.inverse())
    mid_doc.recompute()
    rc = mid_asm.solve()
    print(f"Mid solve rc={rc}")
    mid_doc.save()

    grand_doc, grand_asm, boxC, midlink = ntu.new_grand_assembly_with_sublink(
        "LimitChangeGrand", mid_asm, paths["mid"], paths["grand"], box_name="BoxC", link_name="MidLink"
    )
    midlink.Rigid = False
    grand_doc.recompute()
    ia = [o for o in midlink.Group if o.Label == "BoxA"][0]
    jtu.make_joint(grand_asm, 0, boxC, ia, OUTER_PLC1, OUTER_PLC2, elem1="Face4", elem2="Face3")
    grand_doc.recompute()
    rc = grand_asm.solve()
    print(f"Grand solve rc={rc}")
    grand_doc.save()
    mid_doc.save()
    print("RESULT: PASS" if rc == 0 else "RESULT: FAIL")
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
