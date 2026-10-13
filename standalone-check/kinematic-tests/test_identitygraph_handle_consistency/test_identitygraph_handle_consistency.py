"""
test_identitygraph_handle_consistency.py - Regressionstest fuer die IdentityGraph-Invariante
"ein Solver-Koerper = genau ein IdentityHandle" (todo-identitygraph-duplicate-handle-split).

Anlass (2026-10-03, per exportIdentityGraphDot() am Fixture test_fixed_duplicate_instance_nested
gefunden): resolveJointRef() lieferte fuer einen Joint INNERHALB einer duplizierten Instanz
dasselbe Teil als anderes Handle (Template = Spiegel statt Quellobjekt) als ein Joint der
aeusseren Ebene. Beide materialisieren zum selben Solver-Koerper - der Solver rechnet also
richtig, aber alle handle-basierten Graph-Funktionen (neighbors(), isGrounded(), exportDot())
sahen zwei getrennte Teile.

Laeuft ueber ALLE Testfaelle mit fixtures/-Ordner (flach, verschachtelt, doppelt verschachtelt,
Duplikat-Instanzen). Je Fixture wird der komplette fixtures/-Ordner mit Original-Dateinamen in
einen eigenen Ausgabe-Unterordner kopiert und nur die Kopie geoeffnet (siehe
nested_test_utils.py, "Wichtige Lektion zur Kopie-Benennung"). Geprueft wird die oberste
Baugruppe UND jede geladene Unterbaugruppe fuer sich, jeweils nach Recompute + Solve.

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
import nested_test_utils as ntu  # noqa: E402,F401  (setzt u.a. DuplicateLabels)

ntu.allow_duplicate_labels()


def output_root():
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    if base:
        return os.path.splitext(base)[0]
    return os.path.join(THIS_DIR, "fcstd-output", "default")


def top_document_file(fixtures_dir):
    grand = glob.glob(os.path.join(fixtures_dir, "*_grand.FCStd"))
    if grand:
        return os.path.basename(grand[0])
    candidates = [
        f for f in glob.glob(os.path.join(fixtures_dir, "*.FCStd"))
        if not os.path.basename(f).startswith("Box")
    ]
    return os.path.basename(candidates[0]) if len(candidates) == 1 else None


def fixture_dirs():
    result = []
    for d in sorted(glob.glob(os.path.join(TESTS_ROOT, "test_*"))):
        fixtures = os.path.join(d, "fixtures")
        if os.path.isdir(fixtures) and top_document_file(fixtures):
            result.append(d)
    return result


def assembly_of(doc):
    for obj in doc.Objects:
        if obj.TypeId == "Assembly::AssemblyObject":
            return obj
    return None


def close_all_documents():
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)


def check_fixture(fixture_dir):
    name = os.path.basename(fixture_dir)
    out_dir = os.path.join(output_root(), name)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    shutil.copytree(os.path.join(fixture_dir, "fixtures"), out_dir)

    top_doc = App.openDocument(os.path.join(out_dir, top_document_file(out_dir)))
    top = assembly_of(top_doc)
    if top is None:
        close_all_documents()
        return [f"{name}: keine Baugruppe im obersten Dokument"]
    top_doc.recompute()
    top.solve(False)

    errors = []
    checked = 0
    for doc in [top_doc] + [d for d in App.listDocuments().values() if d is not top_doc]:
        asm = assembly_of(doc)
        if asm is None:
            continue
        checked += 1
        for line in asm.verifyIdentityGraphHandleConsistency():
            errors.append(f"{name} [{doc.Name}]: {line}")

    print(f"  {name}: {checked} Baugruppe(n) geprueft, {'OK' if not errors else 'FEHLER'}")
    close_all_documents()
    return errors


def main():
    dirs = fixture_dirs()
    if not dirs:
        print("RESULT: FAIL (keine Fixturen gefunden)")
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
