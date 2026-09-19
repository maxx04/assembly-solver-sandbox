"""
Self-contained reproduction of a display/round-trip bug in the Assembly Joint editing
dialog's "Rotation" ("Drehung") spinbox for a joint's Offset2.

Root cause: Base::Rotation::getYawPitchRoll() always returns its yaw component wrapped to
the canonical range (-180 deg, +180 deg]. JointObject.py's TaskAssemblyCreateJoint class
uses this function in TWO places on the SAME live-editing session:

  1. onRotationChanged() (the spinbox's write path): takes whatever raw value the spinbox
     currently holds (which can be ANY real number, e.g. 190, 270, 400 - Qt spinboxes do not
     canonicalize user input) and calls Offset2.Rotation.setYawPitchRoll(yaw, ...).

  2. updateOffsetWidgets() (the spinbox's read-back/sync path, called after every property
     change/recompute): reads the value back via Offset2.Rotation.getYawPitchRoll()[0] and
     writes THAT back into the spinbox's own "rawValue" property.

Because step 2 always yields a value in (-180, 180], any user input outside that range gets
silently rewritten to its wrapped equivalent the next time the widgets resync - producing a
visible, discontinuous jump in the displayed angle (e.g. entering 190 makes the spinbox jump
to display -170) even though the underlying 3D rotation itself changes perfectly smoothly and
continuously. Because a downstream part's own displayed orientation is typically computed as
some other rotation MINUS this offset (e.g. "90 - Offset2.yaw" is a common case for a
Fixed joint whose Placement1/2 come from a face normal), the visible "jump point" is not
necessarily at +-180 for the joint's own Offset2 spinbox, but can appear shifted to a
different pair of angles from the operator's point of view (in the real-world case that
uncovered this, the visible jump appeared to sit at 90/270 degrees rather than +-180,
purely because of this downstream sign/offset relationship - the underlying wrap itself is
always exactly at +-180 in Offset2's own frame).

This script does not require any GUI - it reproduces the exact function calls the dialog
itself performs (Rotation.setYawPitchRoll()/getYawPitchRoll()) and shows the discontinuity
directly on a real Assembly Fixed Joint's Offset2 property.

Run via: FreeCADCmd OffsetRotationSpinboxJumpRepro.py
(or paste into the FreeCAD Python console with a document open).
"""
import FreeCAD as App
import Part

doc = App.newDocument("OffsetRotationSpinboxJumpRepro")
asm = doc.addObject("Assembly::AssemblyObject", "Assembly")

boxA = doc.addObject("Part::Box", "BoxA")
asm.addObject(boxA)
boxB = doc.addObject("Part::Box", "BoxB")
boxB.Placement = App.Placement(App.Vector(200, 0, 0), App.Rotation())
asm.addObject(boxB)
doc.recompute()

import UtilsAssembly
import JointObject

jointGroup = UtilsAssembly.getJointGroup(asm)
ground = jointGroup.newObject("App::FeaturePython", "GroundedJoint")
JointObject.GroundedJoint(ground, boxA)
doc.recompute()

joint = jointGroup.newObject("App::FeaturePython", "Joint")
JointObject.Joint(joint, JointObject.JointTypes.index("Fixed"))
joint.Reference1 = (boxA, ["Face2", "Face2"])
joint.Reference2 = (boxB, ["Face1", "Face1"])
doc.recompute()


def dialog_set_rotation(joint_obj, yaw_the_user_typed):
    """Exactly what TaskAssemblyCreateJoint.onRotationChanged() does."""
    ypr = joint_obj.Offset2.Rotation.getYawPitchRoll()
    joint_obj.Offset2.Rotation.setYawPitchRoll(yaw_the_user_typed, ypr[1], ypr[2])


def dialog_readback_for_spinbox(joint_obj):
    """Exactly what TaskAssemblyCreateJoint.updateOffsetWidgets() writes into the
    rotationSpinbox's "rawValue" property afterwards."""
    return joint_obj.Offset2.Rotation.getYawPitchRoll()[0]


print("=== Simuliert: Nutzer dreht die 'Drehung'-Spinbox schrittweise inkrementell hoch ===")
for typed_value in [0, 45, 90, 135, 179, 180, 185, 190, 200, 225, 270, 315, 359]:
    dialog_set_rotation(joint, typed_value)
    readback = dialog_readback_for_spinbox(joint)
    deviation = readback - typed_value
    flag = "  <-- SPRUNG (Anzeige != Eingabe)" if abs(deviation) > 0.001 else ""
    print(
        f"Nutzer tippt {typed_value:6.1f} deg  ->  Dialog liest zurueck und zeigt an: "
        f"{readback:8.3f} deg (Abweichung: {deviation:7.1f} deg){flag}"
    )

print()
print("Erwartet: die Spinbox-Anzeige sollte kontinuierlich mit dem eingegebenen Winkel")
print("mitwachsen (0, 45, 90, ..., 359 - oder aequivalent immer im selben 360-Grad-Fenster")
print("relativ zum vorherigen Wert). Tatsaechlich: sobald der eingegebene Winkel 180 Grad")
print("ueberschreitet, wird die naechste Rueckgabe von getYawPitchRoll() in (-180, 180]")
print("gewickelt und die Anzeige springt schlagartig ins Negative.")

doc.saveAs("OffsetRotationSpinboxJumpRepro.FCStd")
print("Gespeichert: OffsetRotationSpinboxJumpRepro.FCStd (letzter Zustand: Offset2 Drehung = 359 deg eingegeben)")
