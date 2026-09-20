# Assembly-Solver Sandbox

Editier- und Diff-Sandkasten rund um das **Assembly**-Workbench-Modul von
[FreeCAD](https://github.com/FreeCAD/FreeCAD) (`src/Mod/Assembly`) und den darin eingebundenen
**OndselSolver** (Mehrkörper-/Newton-Raphson-Engine).

## Warum

FreeCADs Assembly-Modul ist noch jung und hat einige nicht-triviale Bugs in der
Verschachtelungs-/Duplikations-Logik (verlinkte Unterbaugruppen, mehrfach eingefügte Instanzen,
Ziehen im 3D-Raum). Dieses Repo ist der Ort, an dem solche Bugs live an echten Baugruppen gefunden,
minimal nachgebaut, root-cause-analysiert und gefixt werden - mit dem Ziel, die Fixes upstream nach
FreeCAD zurückzutragen.

## Was hier drin ist

- **`src/Mod/Assembly/`** - der eigentliche C++/Python-Quellcode, den wir bearbeiten.
- **`bugreports/`** - dokumentierte Einzelbefunde mit minimaler Reproduktion, jeweils eigener
  Unterordner (Repro-Datei(en) + Beschreibung, teils als Vorlage für einen Upstream-Issue).
- **`patches/`** - fertige, verifizierte Fixes als eigenständige `.patch`-Dateien, byte-genau gegen
  einen bestimmten FreeCAD-Stand geprüft.
- **`docs/ARCHITECTURE.md`** - lebendes Nachschlagewerk: wie das Modul aktuell funktioniert und wo
  die laufenden Umbauten stehen.
- **`docs/JOURNAL.md`** - Chronik: jeder Bug/Fix mit Herleitung und Verifikation, in
  Entstehungsreihenfolge.
- **`standalone-check/`** - Testinfrastruktur für headless/GUI-Kinematik-Tests gegen eine echte
  FreeCAD-Installation.

## Kontext

Dieses Repo ist ein schlanker, orphan Sync-Branch (`sandbox-main` → GitHub `main`) ohne die volle
FreeCAD-Historie. Die eigentliche Arbeit passiert in einem separaten, lokalen Sparse-Checkout des
FreeCAD-Hauptrepos (Branch `solver-sandbox`, nie gepusht) und wird per Cherry-Pick hierher
synchronisiert (`update-sandbox.sh`).

---

Hauptsächlich bearbeitet mit [Claude Code](https://claude.com/claude-code) (Anthropic).
