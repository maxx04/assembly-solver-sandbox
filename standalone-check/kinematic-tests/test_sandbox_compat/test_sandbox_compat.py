"""
Befehl "Für FreeCAD prüfen / normalisieren" (SandboxCompat.py, Plan
plans/2026-11-05-datei-normalisieren.md) an zwei Projekten:

1. Verschachtelt: test_revolute_nested_flex (GrandTop -> flexible SubLink -> Revolute innen).
   Erwartung: keine Bewegung in normalem FreeCAD; nach dem Normalisieren hat SubLink die
   Gelenkkopie, die normales FreeCAD selbst anlegt (FreeCAD 1.1.4 tut das beim Öffnen NICHT -
   gemessen 2026-11-05).
2. Punkt auf Kurve (test_point_on_curve_flat, Fall 0): statisch als "rot" erkannt; Gegenprobe zeigt
   einen Sprung; nach dem Normalisieren (Gelenk in der Kopie deaktiviert) keine Bewegung mehr.

Gegenprobe mit normalem FreeCAD: bevorzugt die stabile Version (/home/maxx/freecad/App_stable),
sonst die Weekly. Ist keins vorhanden, werden nur die statischen Teile geprüft.

Aufruf: ./common/run-test.sh <freecad-install-dir> test_sandbox_compat/test_sandbox_compat.py
"""
import os
import shutil
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(THIS_DIR)
sys.path.insert(0, os.path.join(ROOT, "common"))
sys.path.insert(0, os.path.join(ROOT, "test_revolute_nested_flex"))
sys.path.insert(0, os.path.join(ROOT, "test_point_on_curve_flat"))

import SandboxCompat as SC  # noqa: E402

REFERENCES = (
    "/home/maxx/freecad/App_stable/freecad.App",
    "/home/maxx/freecad/App_weekly/freecad.App",
)


def _closeAll():
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)


def build_nested(base):
    os.environ["KINEMATIC_TEST_OUTPUT_FCSTD"] = os.path.join(base, "nested.FCStd")
    import test_revolute_nested_flex as t

    grand_doc = t.load_fixture_and_solve()[0]
    for doc in grand_doc.getDependentDocuments(True):
        if doc.FileName:
            doc.save()
    path = grand_doc.FileName
    _closeAll()
    return path


def build_point_on_curve(base):
    import test_point_on_curve_flat as t

    t.THIS_DIR = base
    original_close = App.closeDocument

    def close_and_save(name):
        App.getDocument(name).save()
        original_close(name)

    App.closeDocument = close_and_save
    try:
        t.run_case(0, *t.CASES[0])
    finally:
        App.closeDocument = original_close
    return os.path.join(base, "fcstd-output", "case0", "case.FCStd")


def worst_move(report):
    worst = 0.0
    for r in report.reference or []:
        for _label, dist, angle in r.get("moves") or []:
            worst = max(worst, dist + angle / 10.0)
    return worst


def main():
    out = os.path.join(THIS_DIR, "fcstd-output", "run")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    reference = next((r for r in REFERENCES if os.path.exists(r)), "")
    print(f"Gegenprobe mit: {reference or '(keins - nur statisch)'}")
    ok = True

    def expect(cond, text):
        nonlocal ok
        print(("OK     " if cond else "FEHLER ") + text)
        ok &= bool(cond)

    # ---- 1. verschachtelt
    grand = build_nested(os.path.join(out, "nested"))
    doc = App.openDocument(grand)
    report = SC.checkProject(doc, reference)
    expect(not report.errors, f"verschachtelt prüfen ohne Fehler {report.errors}")
    expect(len(report.project.files) >= 4, f"Projekt eingesammelt: {len(report.project.files)} Dateien")
    if reference:
        expect(worst_move(report) == 0.0, "verschachtelt: keine Bewegung in normalem FreeCAD")
    target = os.path.join(out, "nested_FreeCAD")
    report = SC.normalizeProject(doc, target, reference, True)
    expect(not report.errors, f"verschachtelt normalisieren ohne Fehler {report.errors}")
    expect(os.path.exists(os.path.join(target, os.path.basename(grand))), "Kopie liegt im Zielordner")
    if reference:
        copies = [v for r in report.finish or [] for v in (r.get("jointCopies") or {}).values()]
        expect(copies and all(n == e and e > 0 for _l, n, e in copies), f"Gelenkkopien angelegt {copies}")
        missing = [m for r in report.reference or [] for m in SC._missingCopies(r)]
        expect(not missing, f"Gegenprobe: keine fehlenden Gelenkkopien {missing}")
        expect(worst_move(report) == 0.0, "Gegenprobe nach Normalisieren: keine Bewegung")
    _closeAll()

    # ---- 2. Punkt auf Kurve
    poc = build_point_on_curve(os.path.join(out, "poc"))
    doc = App.openDocument(poc)
    report = SC.checkProject(doc, reference)
    kinds = [f["kind"] for f in report.static]
    expect("pointOnCurve" in kinds, f"Punkt auf Kurve statisch erkannt {kinds}")
    if reference:
        expect(worst_move(report) > 1.0, f"Gegenprobe zeigt Sprung ({worst_move(report):.3f})")
    report = SC.normalizeProject(doc, os.path.join(out, "poc_FreeCAD"), reference, True)
    expect(not report.errors, f"Punkt auf Kurve normalisieren ohne Fehler {report.errors}")
    if reference:
        expect(worst_move(report) == 0.0, "nach Deaktivieren in der Kopie: keine Bewegung")
    _closeAll()
    # Original unverändert: Gelenk weiter aktiv
    doc = App.openDocument(poc)
    joint = [o for o in doc.Objects if hasattr(o, "JointType")][0]
    expect(not joint.Suppressed, "Original-Gelenk bleibt aktiv")
    _closeAll()

    print("RESULT: PASS" if ok else "RESULT: FAIL (siehe Ausgabe oben)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
