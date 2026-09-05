# Minimaler, reproduzierbarer FreeCAD-Bug: Assembly::AssemblyLink.Rigid steuert, ob
# UtilsAssembly.getComponentReference() eine Joint-Referenz auf eine verschachtelte
# Unterbaugruppe kompakt (name=Unterbaugruppe) oder flach durchgereicht (name=Enkelkind)
# kodiert - Letzteres bricht, sobald die Struktur nochmal eine Ebene hoeher gespiegelt wird
# (AssemblyLink::synchronizeComponents()s objLinkMap kennt nur direkte Kinder).
#
# Baut zwei synthetische Baugruppen A (Sub, Rigid=True) und B (Sub, Rigid=False), jeweils mit
# einem verschachtelten Teil "InnerPart", und ruft UtilsAssembly.getComponentReference() direkt
# mit identischer Auswahl-Situation auf - zeigt den unterschiedlichen Rueckgabewert.

import os
import FreeCAD as App
import Part
import UtilsAssembly

SCRATCH = "/tmp/claude-1000/-home-maxx-Dokumente-FreeCAD-Development-FCProject/7ae7d107-34ed-4b15-9f8b-179cebc6a9db/scratchpad/repro_docs"
os.makedirs(SCRATCH, exist_ok=True)


def make_sub_assembly(name, rigid):
    """Baut eine Unterbaugruppe mit einem Part::Box als 'InnerPart' Kind."""
    sub_doc = App.newDocument(f"{name}_source")
    asm = sub_doc.addObject("Assembly::AssemblyObject", name)
    inner = sub_doc.addObject("Part::Box", "InnerPart")
    asm.addObject(inner)
    sub_doc.recompute()
    # AssemblyLink.LinkedObject ist ein App::PropertyXLink - braucht eine gespeicherte Datei.
    sub_doc.saveAs(f"{SCRATCH}/{sub_doc.Name}.FCStd")
    return sub_doc, asm, inner


def make_outer(name, sub_doc, sub_asm, rigid):
    """Baut eine aeussere Baugruppe, die sub_asm per AssemblyLink einbindet."""
    outer_doc = App.newDocument(f"{name}_outer")
    outer_doc.saveAs(f"{SCRATCH}/{outer_doc.Name}.FCStd")
    outer_asm = outer_doc.addObject("Assembly::AssemblyObject", "OuterAssembly")
    link = outer_doc.addObject("Assembly::AssemblyLink", "SubLink")
    link.LinkedObject = sub_asm
    link.Rigid = rigid
    outer_asm.addObject(link)
    outer_doc.recompute()
    outer_doc.recompute()  # zweiter Pass, damit synchronizeComponents() durchlaeuft
    return outer_doc, outer_asm, link


for rigid in (True, False):
    sub_doc, sub_asm, inner = make_sub_assembly("Sub", rigid)
    outer_doc, outer_asm, link = make_outer("Test", sub_doc, sub_asm, rigid)

    # Finde die im AssemblyLink gespiegelte lokale Kopie von InnerPart
    mirrored_inner = None
    for obj in link.Group:
        if obj.Name.startswith("InnerPart"):
            mirrored_inner = obj
            break

    print(f"\n=== Rigid={rigid} ===")
    print(f"  SubLink.Rigid = {link.Rigid}")
    print(f"  Gespiegeltes InnerPart gefunden: {mirrored_inner.Name if mirrored_inner else 'NEIN'}")

    if mirrored_inner:
        # Simuliert die Auswahl-Situation: root_obj=SubLink, sub_string="InnerPart.Face1"
        # (das ist exakt, was FreeCADs 3D-Auswahl beim Klick auf eine Flaeche im verschachtelten
        # Teil als SelectionObject liefert)
        component, new_sub = UtilsAssembly.getComponentReference(
            outer_asm, outer_asm, "SubLink.InnerPart.Face1"
        )
        print(f"  getComponentReference(OuterAssembly, 'SubLink.InnerPart.Face1') ->")
        print(f"    component = {component.Name if component else None}")
        print(f"    new_sub   = {new_sub!r}")
        if component and component.Name == "SubLink":
            print("    -> KOMPAKT: zeigt auf SubLink selbst - Spiegeln nach aussen funktioniert.")
        elif component and component.Name.startswith("InnerPart"):
            print(
                "    -> FLACH: zeigt direkt auf InnerPart (Enkelkind) - objLinkMap beim "
                "Spiegeln nach aussen kennt das nicht -> BUG."
            )

    App.closeDocument(outer_doc.Name)
    App.closeDocument(sub_doc.Name)
