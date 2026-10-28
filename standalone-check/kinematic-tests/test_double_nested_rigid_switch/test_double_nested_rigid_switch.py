"""
Regressionstest: Starr/Flexibel-Umschalten einer Unterbaugruppe, die selbst eine weitere
flexible Unterbaugruppe enthaelt (zwei Verschachtelungsebenen).

Anlass (2026-10-28, PROJ_CNC3018): BG37 in BG22 auf starr geschaltet - der Schrittmotor in
BG37 > BG43 > BG67 sprang 273 mm, beim Zurueckschalten flogen DC-Motor und Z-Schlitten weg
(Gelenkgrenze verletzt). onChanged(Rigid) glich nur die direkten Kinder ab; die verschachtelten
Container darunter liefen nicht, ihre Teile behielten die alten lokalen Lagen und wanderten mit
der neuen Container-Lage mit. Fix: AssemblyLink::onChanged(Rigid) gleicht die verschachtelten
Container von oben nach unten ab (updateNestedLinkContents()).

Fixtur: die von test_fixed_double_nested_flex (Sub -> Mid -> Grand, je ein Fixed-Gelenk):
    Mid:   BoxD (geerdet) + SubLink(Sub: BoxA, BoxB)
    Grand: BoxC (geerdet) + MidLink(Mid)
Geprueft in Grand:
  1. MidLink starr: SubB-Spiegel liegt relativ zum BoxD-Spiegel wie in Mid selbst.
  2. MidLink wieder flexibel + Solve: Solve ok, Lage relativ zu BoxD unveraendert.

Aufruf:
    ./common/run-test.sh <freecad-install-dir> test_double_nested_rigid_switch/test_double_nested_rigid_switch.py
"""
import os
import shutil
import sys
import tempfile

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_ROOT = os.path.dirname(THIS_DIR)
FIXTURES = os.path.join(TESTS_ROOT, "test_fixed_double_nested_flex", "fixtures")
TOL_POS = 1e-6
TOL_DEG = 1e-4


def assembly_of(doc):
    return [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]


def find_world(container, name, plc=App.Placement()):
    """Weltlage des Objekts 'name' (Label) unter 'container', ueber die AssemblyLink-Kette."""
    for obj in container.Group:
        if "Placement" not in obj.PropertiesList:
            continue
        world = plc.multiply(obj.Placement)
        if obj.Label == name:
            return world
        if obj.TypeId == "Assembly::AssemblyLink":
            found = find_world(obj, name, world)
            if found is not None:
                return found
    return None


def relation(container, ref, target):
    a, b = find_world(container, ref), find_world(container, target)
    if a is None or b is None:
        return None
    return a.inverse().multiply(b)


def same(label, want, got):
    if got is None:
        print(f"{label}: Objekt nicht gefunden -> FEHLER")
        return False
    dp = (want.Base - got.Base).Length
    dr = abs(want.Rotation.multiply(got.Rotation.inverted()).Angle) * 57.29577951308232
    dr = min(dr, 360.0 - dr)
    ok = dp <= TOL_POS and dr <= TOL_DEG
    print(f"{label}: Abweichung {dp:.6f} mm / {dr:.6f} Grad -> {'OK' if ok else 'FEHLER'}")
    return ok


def main():
    work = tempfile.mkdtemp()
    for name in os.listdir(FIXTURES):
        if name.endswith(".FCStd"):
            shutil.copy(os.path.join(FIXTURES, name), work)
    grand = App.openDocument(os.path.join(work, "test_fixed_double_nested_flex_grand.FCStd"))
    mid = App.getDocument("test_fixed_double_nested_flex_mid") if "test_fixed_double_nested_flex_mid" in App.listDocuments() else None
    asm = assembly_of(grand)
    grand.recompute()
    rc = asm.solve()
    if mid is None:
        mid = [d for n, d in App.listDocuments().items() if n.endswith("_mid")][0]
    # Mid in seinem eigenen Kontext loesen: "starr" uebernimmt Mids Anordnung, die muss das
    # mittlere Gelenk erfuellen (die Fixtur-Datei ist ungeloest gespeichert).
    raw = relation(assembly_of(mid), "BoxD", "BoxB")
    assembly_of(mid).solve()
    want = relation(assembly_of(mid), "BoxD", "BoxB")
    moved = (raw.Base - want.Base).Length
    print(f"Mid-Datei gespeichert -> geloest: {moved:.6f} mm")
    mid_link = [o for o in asm.Group if o.TypeId == "Assembly::AssemblyLink"][0]
    ok = rc == 0

    mid_link.Rigid = True
    grand.recompute()
    ok &= same("Starr: SubB relativ zu BoxD wie in Mid", want, relation(mid_link, "BoxD", "BoxB"))

    mid_link.Rigid = False
    grand.recompute()
    rc = asm.solve()
    print(f"Flexibel: solve rc={rc}")
    ok &= rc == 0
    ok &= same("Flexibel: SubB relativ zu BoxD wie in Mid", want, relation(mid_link, "BoxD", "BoxB"))

    print("RESULT: PASS" if ok else "RESULT: FAIL")
    return 0 if ok else 1
