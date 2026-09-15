"""
build_box_bodies.py - baut die vier gemeinsamen, git-getrackten Koerper-Bibliotheksdateien
(BoxA/BoxB/BoxC/BoxD.FCStd) EINMALIG unter common/fixtures/.

Nutzerauftrag 2026-09-15 ("Tests naeher zur Realitaet... jede Koerper und jede Assembly soll
ein eigenes Datei bekommen damit xlinks spielen mit"): bisher wurden BoxA/BoxB/BoxC als
NATIVE Part::Box-Objekte direkt INNERHALB des jeweiligen Assembly-Dokuments angelegt - in
echter Mehrdatei-Nutzung (PDM-Alltag: Einzelteile als eigene Dateien, per XLink in Baugruppen
eingebunden) ist das unrealistisch und testet den externen-Koerper-Referenz-Pfad
(App::Link auf ein NICHT-Assembly-Dokument) ueberhaupt nicht - nur der bereits gut getestete
AssemblyLink-Pfad (Unterbaugruppe->Unterbaugruppe) war bisher abgedeckt.

Diese vier Dateien sind die KANONISCHEN Master-Koerper - jede enthaelt genau ein Part::Box
(Standardgroesse 10x10x10mm, siehe joint_test_utils.py::BOX_FACES, das von dieser Geometrie
ausgeht). Sie werden NICHT direkt von den einzelnen Testfaellen verlinkt (siehe
joint_test_utils.py::ensure_box_bodies_in() - jede Testfixture bekommt beim Bauen ihre EIGENE
Kopie dieser Dateien in ihr eigenes fixtures/-Verzeichnis, exakt nach demselben "gleicher
Dateiname, gleiches Verzeichnis"-Muster wie Sub/Grand/Mid schon immer kopiert werden - ein
XLink speichert einen relativen Pfad, der bei einer spaeteren Kopie in ein ANDERES
Verzeichnis sonst bricht, siehe nested_test_utils.py's "Wichtige Lektion zur
Kopie-Benennung"). "Boxen A, B, C duerfen fuer alle Tests dieselbe Datei benutzen" (Nutzer)
heisst: EIN gemeinsamer Ursprung, aus dem jede Testfixture ihre eigene, git-getrackte Kopie
zieht - nicht "jeder Test baut seine eigene Box neu".

Aufruf (Installation ist irrelevant, reiner Strukturaufbau ohne Solver-Aktivitaet):
    ./common/run-test.sh /home/maxx/freecad-sandbox/install common/build_box_bodies.py
"""
import os
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
FIXTURES_DIR = os.path.join(THIS_DIR, "fixtures")

BOX_LABELS = ["BoxA", "BoxB", "BoxC", "BoxD"]


def build_one(label):
    doc = App.newDocument(label)
    box = doc.addObject("Part::Box")
    box.Label = label
    doc.recompute()
    path = os.path.join(FIXTURES_DIR, f"{label}.FCStd")
    doc.saveAs(path)
    App.closeDocument(doc.Name)
    print(f"Fixture geschrieben: {path}")
    return path


def main():
    os.makedirs(FIXTURES_DIR, exist_ok=True)
    for label in BOX_LABELS:
        build_one(label)
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
