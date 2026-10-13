"""
build_fixture.py - baut die festen Ausgangsdateien fuer test_body_basefeature_reference_flat.py
EINMALIG: ein Dokument mit PartDesign-Body (BaseFeature = Zylinder, Placement 90 Grad um Y) und
eine Baugruppe mit geerdeter Box + Link auf den Body, Fixed-Joint auf "BaseFeature.EdgeN".

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install test_body_basefeature_reference_flat/build_fixture.py
"""
import os
import sys

import FreeCAD as App
import Part

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import joint_test_utils as jtu  # noqa: E402
from test_body_basefeature_reference_flat import TEST_NAME, fixture_paths  # noqa: E402


def main():
    import JointObject
    import UtilsAssembly

    paths = fixture_paths()
    os.makedirs(os.path.dirname(paths["body"]), exist_ok=True)

    body_doc = App.newDocument(f"{TEST_NAME}_body")
    geo = body_doc.addObject("Part::Feature", "Geo")
    geo.Shape = Part.makeCylinder(4, 12)
    geo.Placement = App.Placement(App.Vector(1.5, 0, 4.25), App.Rotation(App.Vector(0, 1, 0), 90))
    body = body_doc.addObject("PartDesign::Body", "Body")
    body.BaseFeature = geo
    body_doc.recompute()
    base_feature = [o for o in body_doc.Objects if o.TypeId == "PartDesign::FeatureBase"][0]
    circle = [i for i, e in enumerate(base_feature.Shape.Edges, 1) if e.Curve.TypeId == "Part::GeomCircle"][0]
    body_doc.saveAs(paths["body"])

    doc = App.newDocument(f"{TEST_NAME}_asm")
    doc.saveAs(paths["asm"])
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    box = doc.addObject("Part::Box", "Grundteil")
    box.Length = box.Width = box.Height = 20
    asm.addObject(box)
    link = doc.addObject("App::Link", "Body_Link")
    link.LinkedObject = body
    asm.addObject(link)
    doc.recompute()
    jtu.ground_object(box)

    joint_group = UtilsAssembly.getJointGroup(asm)
    joint = joint_group.newObject("App::FeaturePython", "Joint")
    JointObject.Joint(joint, 0)
    if App.GuiUp:
        JointObject.ViewProviderJoint(joint.ViewObject)
    sub = f"{base_feature.Name}.Edge{circle}"
    joint.Reference1 = (box, ["Face6", "Face6"])
    joint.Reference2 = (link, [sub, sub])
    joint.Detach1 = False
    joint.Detach2 = False
    joint.Proxy.updateJCSPlacements(joint)
    doc.recompute()
    print(f"Referenz {link.Name} {sub}, Placement2 {joint.Placement2}")
    body_doc.save()
    doc.save()
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
