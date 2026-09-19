"""
Aequivalenz-Verifikation fuer den IdentityGraph-Umbau (Phase 0, siehe
/home/maxx/.claude/plans/enumerated-roaming-river.md).

Laeuft ueber JEDE bestehende Kinematik-Testfixture (dieselben, die auch run-all-tests.sh nutzt),
oeffnet+loest sie ganz normal ueber deren EIGENE load_fixture_and_solve()-Funktion (kein
Doppelbau der Fixture-Logik hier), und ruft danach auf JEDER im Prozess offenen
Assembly::AssemblyObject-Instanz AssemblyObject::verifyIdentityGraphEquivalence() auf (C++-seitige
Diagnosefunktion, siehe AssemblyObject.h/.cpp - vergleicht IdentityGraph::resolve()/
resolveJointRef() gegen canonicalizeForMbD()/resolveJointReference()).

Erwartung (siehe Plan-Datei, Phase 0):
- "MATCH"-Zeilen ueberall dort, wo keine Instanz-Duplikation im Spiel ist - das ist die grosse
  Mehrheit der bestehenden Fixtures.
- "DIVERGENCE (by design, dupliziert)"-Zeilen NUR in den beiden Duplikat-Instanz-Fixtures
  (test_fixed_duplicate_instance_flat/_nested) - das ist die ERWARTETE, im Plan begruendete
  Abweichung (canonicalizeForMbD()/resolveJointReference() geben bei Duplikation bewusst frueh
  auf, der neue Graph loest tiefengenerell weiter auf).
- KEINE "MISMATCH"-Zeile irgendwo - das waere ein Bug in der neuen IdentityGraph-Implementierung.

Aufruf (wie jeder andere Testfall, ueber run-test.sh):
    ./common/run-test.sh <freecad-install-dir> common/verify_identity_graph_equivalence.py

Exit-Code 0 nur wenn keine MISMATCH-Zeile auftrat (ueber alle Fixtures hinweg), sonst 1.
"""
import glob
import os
import runpy
import sys

import FreeCAD as App

COMMON_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(COMMON_DIR)
sys.path.insert(0, COMMON_DIR)


def discover_fixture_scripts():
    """Jeder Testordner test_*/test_*.py, der eine load_fixture_and_solve()-Funktion definiert
    (die eine Handvoll unvollstaendiger/verwaister Ordner wie
    test_fixed_duplicate_instance_nested/ - siehe Projekt-Memory - werden dadurch automatisch
    uebersprungen, ohne eine Ausnahmeliste pflegen zu muessen)."""
    scripts = []
    for path in sorted(glob.glob(os.path.join(ROOT_DIR, "test_*", "test_*.py"))):
        with open(path, "r", encoding="utf-8") as f:
            if "def load_fixture_and_solve" in f.read():
                scripts.append(path)
    return scripts


def close_all_documents():
    for name in list(App.listDocuments().keys()):
        try:
            App.closeDocument(name)
        except Exception:  # noqa: BLE001 - Aufraeumen soll nie den restlichen Lauf abbrechen
            pass


def run_one_fixture(script_path):
    """Fuehrt EIN Testfall-Skript aus (load_fixture_and_solve(), wie run-test.sh es fuer den
    eigentlichen Test auch tut), sammelt danach die Verifikationszeilen jeder offenen
    AssemblyObject-Instanz ein. Gibt (lines, error) zurueck - 'error' ist ein String bei
    Ausnahmen, sonst None."""
    test_dir = os.path.dirname(script_path)
    test_name = os.path.basename(script_path)[: -len(".py")]

    # Gleiches Muster wie run_under_gui.py: sys.path lokal fuer die Dauer dieses einen
    # Testfalls ergaenzen (Testskripte erwarten ihr eigenes Verzeichnis + common/ auf dem Pfad),
    # danach wieder entfernen, damit sich zwei Testfaelle nicht gegenseitig ueber sys.path
    # beeinflussen.
    sys.path.insert(0, test_dir)
    try:
        # Eigener Ausgabe-Pfad pro Fixture (sonst wuerden mehrere Faelle sich denselben
        # Default-Pfad teilen) - wir brauchen die Datei inhaltlich nicht, nur einen gueltigen,
        # schreibbaren Pfad, den load_fixture_and_solve() erwartet.
        os.environ["KINEMATIC_TEST_OUTPUT_FCSTD"] = os.path.join(
            "/tmp", f"identity_graph_verify_{test_name}.FCStd"
        )
        ns = runpy.run_path(script_path, run_name="__identity_graph_verify__")
        try:
            ns["load_fixture_and_solve"]()
        except Exception as exc:  # noqa: BLE001 - wir wollen die Ausnahme melden, nicht abbrechen
            return [], f"Ausnahme in load_fixture_and_solve(): {exc!r}"

        lines = []
        for doc in App.listDocuments().values():
            for obj in doc.Objects:
                if obj.isDerivedFrom("Assembly::AssemblyObject"):
                    prefix = f"{doc.Name}#{obj.Name}: "
                    for line in obj.verifyIdentityGraphEquivalence():
                        lines.append(prefix + line)
        return lines, None
    finally:
        sys.path.remove(test_dir)
        close_all_documents()


def main():
    scripts = discover_fixture_scripts()
    if not scripts:
        print("RESULT: FAIL (keine Testfixtures gefunden)")
        return 1

    total_match = 0
    total_divergence = 0
    mismatches = []
    errors = []

    for script_path in scripts:
        rel = os.path.relpath(script_path, ROOT_DIR)
        lines, error = run_one_fixture(script_path)
        if error:
            print(f"=== {rel}: FEHLER ===")
            print(f"  {error}")
            errors.append((rel, error))
            continue

        print(f"=== {rel} ({len(lines)} geprueft) ===")
        for line in lines:
            print(f"  {line}")
            # Jede Zeile hat die Form "<docname>#<objname>: <MATCH|DIVERGENCE|MISMATCH> ..."
            # (siehe classify()-Lambda in AssemblyObject::verifyIdentityGraphEquivalence()) -
            # Praefix abtrennen, bevor auf das erste Wort klassifiziert wird.
            _, _, payload = line.partition(": ")
            if payload.startswith("MISMATCH"):
                mismatches.append(f"{rel}: {line}")
            elif payload.startswith("DIVERGENCE"):
                total_divergence += 1
            elif payload.startswith("MATCH"):
                total_match += 1

    print()
    print(f"Zusammenfassung: {total_match} MATCH, {total_divergence} DIVERGENCE (erwartet bei "
          f"Instanz-Duplikation), {len(mismatches)} MISMATCH, {len(errors)} Fixture(s) mit Fehler")

    if errors:
        print("RESULT: FAIL (Fehler beim Laden/Loesen einzelner Fixtures)")
        return 1
    if mismatches:
        print("RESULT: FAIL (unerwartete MISMATCH-Zeile(n) - siehe oben)")
        for m in mismatches:
            print(f"  {m}")
        return 1

    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
