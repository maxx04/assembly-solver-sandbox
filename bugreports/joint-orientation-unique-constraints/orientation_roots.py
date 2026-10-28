"""
Repro: on which root do OndselSolver's Fixed, Slider and Revolute joints land?

Flat assembly: grounded box A, free box B, one joint between the top face of A and the bottom
face of B (JCS z axes pointing at each other, i.e. FreeCAD's "reversed" face-to-face case).
B starts rotated by several angles about different axes; after solve() the relative rotation of
the two JCS is classified:
  Fixed/Slider: identity, 180 deg about x (FreeCAD's "reversed"), 180 about y, 180 about z
  Revolute:     z axes same direction or opposite
Run in FreeCAD (main()), e.g. with the sandbox runner:
  ./common/run-test.sh <freecad> /path/to/orientation_roots.py
"""
import math

import FreeCAD as App

STARTS = [  # (axis, angle in degrees) of the start rotation of B, about its JCS origin
    ((1, 0, 0), 0), ((1, 0, 0), 60), ((1, 0, 0), 120), ((0, 1, 0), 120),
    ((0, 0, 1), 120), ((0, 0, 1), 170), ((1, 1, 0), 150), ((1, 1, 1), 100),
]
TYPES = {"Fixed": 0, "Revolute": 1, "Slider": 3}


def classify(rel, joint_type):
    z = rel.Rotation.multVec(App.Vector(0, 0, 1))
    if joint_type == "Revolute":
        return "z same" if z.z > 0 else "z OPPOSITE"
    names = {"identity": App.Rotation(), "180 x": App.Rotation(App.Vector(1, 0, 0), 180),
             "180 y": App.Rotation(App.Vector(0, 1, 0), 180), "180 z": App.Rotation(App.Vector(0, 0, 1), 180)}
    best = min(names, key=lambda n: names[n].inverted().multiply(rel.Rotation).Angle)
    return best


def run(joint_type, axis, angle):
    import JointObject
    import UtilsAssembly

    doc = App.newDocument("Roots")
    asm = doc.addObject("Assembly::AssemblyObject", "Assembly")
    a = doc.addObject("Part::Box", "A")
    b = doc.addObject("Part::Box", "B")
    asm.addObject(a)
    asm.addObject(b)
    doc.recompute()
    g = UtilsAssembly.getJointGroup(asm).newObject("App::FeaturePython", "GroundedJoint")
    JointObject.GroundedJoint(g, a)
    j = UtilsAssembly.getJointGroup(asm).newObject("App::FeaturePython", "Joint")
    JointObject.Joint(j, TYPES[joint_type])
    if App.GuiUp:
        JointObject.ViewProviderJoint(j.ViewObject)
    j.Reference1 = (a, ["Face6", "Face6"])
    j.Reference2 = (b, ["Face5", "Face5"])
    j.Detach1 = True
    j.Detach2 = True
    jcs1 = App.Placement(App.Vector(5, 5, 10), App.Rotation())                       # top of A, z up
    jcs2 = App.Placement(App.Vector(5, 5, 0), App.Rotation(App.Vector(1, 0, 0), 180))  # bottom of B, z down
    j.Placement1 = jcs1
    j.Placement2 = jcs2
    # B: JCS coincident ("identity" root), then turned about the JCS origin
    turn = App.Placement(App.Vector(), App.Rotation(App.Vector(*axis), angle))
    b.Placement = a.Placement.multiply(jcs1).multiply(turn).multiply(jcs2.inverse())
    doc.recompute()
    rc = asm.solve()
    rel = a.Placement.multiply(jcs1).inverse().multiply(b.Placement.multiply(jcs2))
    App.closeDocument(doc.Name)
    return rc, classify(rel, joint_type)


def main():
    for joint_type in TYPES:
        for axis, angle in STARTS:
            rc, root = run(joint_type, axis, angle)
            print(f"{joint_type:8s} start {angle:3d} deg about {axis}: rc={rc} -> {root}")
    print("RESULT: PASS")
    return 0
