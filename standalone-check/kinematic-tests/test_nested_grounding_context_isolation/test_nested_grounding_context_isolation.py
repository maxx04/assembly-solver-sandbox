"""
test_nested_grounding_context_isolation.py - Regressionstest fuer "Erdung ist Kontext-Eigenschaft"
(Plan plans/2026-10-03-erdung-als-kontext-eigenschaft.md).

Anlass (live an BG43/BG67/BG47 gefunden, 2026-10-03): ein Solve/Recompute der AEUSSEREN Baugruppe
schaltete die eigene Erdung einer verschachtelten Unterbaugruppe ab, indem es das ReadOnly-Flag
der Placement IM DOKUMENT DER UNTERBAUGRUPPE loeschte. Danach galt die Unterbaugruppe auch fuer
sich allein als ungeerdet (alle Joints mit Part_Detached-Overlay).

Allgemein statt fuer eine bestimmte BG (Nutzerauftrag "mit System, fuer alle verschachtelten BGs"):
laeuft ueber ALLE vorhandenen verschachtelten Fixturen (*_nested_flex, *_double_nested_flex,
*_duplicate_instance_nested) - jede hat eine Unterbaugruppe mit eigener Erdung, die ueber einen
aeusseren Joint an eine aeussere Erdung angebunden ist, also genau diese Topologieklasse.

Fixturen werden nur GELESEN: je Fixture wird der komplette fixtures/-Ordner mit den ORIGINALEN
Dateinamen in einen eigenen Ausgabe-Unterordner kopiert (siehe nested_test_utils.py, "Wichtige
Lektion zur Kopie-Benennung") und nur die Kopie geoeffnet.

Prueft je Fixture, nach Recompute + zwei Solves der obersten Baugruppe:
  1. Erdung jeder Unterbaugruppe ist unveraendert (ObjectToGround-Placement weiterhin ReadOnly).
  2. Jede Unterbaugruppe ist fuer sich allein vollstaendig verbunden (isPartConnected() fuer
     beide Referenzen jedes ihrer Joints) - das Kriterium hinter dem Part_Detached-Overlay.
  3. Die oberste Baugruppe behandelt die inneren Erdungsziele NICHT als eigene Erdung (sie sind
     ueber den aeusseren Joint an die aeussere Erdung angebunden).
  4. Kein Drift: zweiter und dritter Solve aendern keine Placement (> 1e-6 mm).

Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import glob
import os
import shutil
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_ROOT = os.path.dirname(THIS_DIR)
sys.path.insert(0, os.path.join(TESTS_ROOT, "common"))
import nested_test_utils as ntu  # noqa: E402  (setzt u.a. DuplicateLabels)

TEST_NAME = os.path.basename(THIS_DIR)
FIXTURE_PATTERNS = ("*_nested_flex", "*_double_nested_flex", "*_duplicate_instance_nested")
TOL_DRIFT = 1e-6


def output_root():
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    if base:
        return os.path.splitext(base)[0]
    return os.path.join(THIS_DIR, "fcstd-output", "default")


def fixture_dirs():
    dirs = set()
    for pattern in FIXTURE_PATTERNS:
        for d in glob.glob(os.path.join(TESTS_ROOT, pattern)):
            if glob.glob(os.path.join(d, "fixtures", "*_grand.FCStd")):
                dirs.add(d)
    return sorted(dirs)


def is_read_only(obj):
    return any(
        pn in obj.PropertiesList and "ReadOnly" in obj.getPropertyStatus(pn)
        for pn in ("Placement", "LinkPlacement")
    )


def assembly_of(doc):
    for obj in doc.Objects:
        if obj.TypeId == "Assembly::AssemblyObject":
            return obj
    return None


def grounded_joints(doc):
    return [o for o in doc.Objects if hasattr(o, "ObjectToGround") and o.ObjectToGround]


def regular_joints(doc):
    return [
        o for o in doc.Objects
        if hasattr(o, "Reference1") and hasattr(o, "Reference2") and o.Reference1 and o.Reference2
    ]


def link_placements():
    result = {}
    for doc in App.listDocuments().values():
        for obj in doc.Objects:
            if obj.TypeId == "App::Link":
                result[f"{doc.Name}#{obj.Name}"] = obj.Placement
    return result


def max_drift(before, after):
    worst, worst_key = 0.0, None
    for key, plc in before.items():
        other = after.get(key)
        if other is None:
            continue
        d = (plc.Base - other.Base).Length
        if d > worst:
            worst, worst_key = d, key
    return worst, worst_key


def close_all_documents():
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)


def check_fixture(fixture_dir):
    name = os.path.basename(fixture_dir)
    out_dir = os.path.join(output_root(), name)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    shutil.copytree(os.path.join(fixture_dir, "fixtures"), out_dir)

    grand_path = glob.glob(os.path.join(out_dir, "*_grand.FCStd"))[0]
    grand_doc = App.openDocument(grand_path)
    top = assembly_of(grand_doc)
    if top is None:
        return [f"{name}: keine Baugruppe im grand-Dokument"]

    grand_doc.recompute()
    top.solve(False)
    before = link_placements()
    top.solve(False)
    top.solve(False)
    drift, drift_key = max_drift(before, link_placements())

    errors = []
    sub_docs = [d for d in App.listDocuments().values() if d is not grand_doc and assembly_of(d)]
    if not sub_docs:
        errors.append(f"{name}: keine Unterbaugruppen-Dokumente geladen")

    for doc in sub_docs:
        asm = assembly_of(doc)
        for gj in grounded_joints(doc):
            target = gj.ObjectToGround
            if not is_read_only(target):
                errors.append(f"{name}: Erdung von {doc.Name}#{target.Name} wurde abgeschaltet")
            if top.isPartGrounded(target):
                errors.append(
                    f"{name}: oberste Baugruppe behandelt {doc.Name}#{target.Name} als eigene Erdung"
                )
        for joint in regular_joints(doc):
            for prop in ("Reference1", "Reference2"):
                part = getattr(joint, prop)[0]
                if not asm.isPartConnected(part):
                    errors.append(
                        f"{name}: {doc.Name}#{joint.Name}.{prop} ({part.Name}) gilt allein als "
                        "nicht verbunden (Part_Detached)"
                    )

    if drift > TOL_DRIFT:
        errors.append(f"{name}: Drift {drift:.3e} mm bei {drift_key}")

    print(f"  {name}: {len(sub_docs)} Unterbaugruppe(n), Drift {drift:.1e} mm, "
          f"{'OK' if not errors else 'FEHLER'}")
    close_all_documents()
    return errors


def main():
    dirs = fixture_dirs()
    if not dirs:
        print("RESULT: FAIL (keine verschachtelten Fixturen gefunden)")
        return 1

    close_all_documents()
    all_errors = []
    for d in dirs:
        all_errors.extend(check_fixture(d))

    for e in all_errors:
        print(f"  FEHLER: {e}")
    if all_errors:
        print(f"RESULT: FAIL ({len(all_errors)} Fehler in {len(dirs)} Fixturen)")
        return 1
    print(f"RESULT: PASS ({len(dirs)} Fixturen)")
    return 0
