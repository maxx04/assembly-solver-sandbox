# FreeCAD-Kernbug: Slider-Joint-Ketten verlieren ihre Position bei erzwungenem Kalt-Recompute

**Status: Ursache eingegrenzt, GitHub-Issue-Text fertig (siehe unten), noch nicht gepatcht/eingereicht.**

## ⚠️ Korrektur (2026-08-09, zweite Untersuchungsrunde)

Die erste Version dieser Untersuchung verdächtigte `AssemblyObject::ensureIdentityPlacements()`.
**Das war falsch** - Testartefakt der Aufrufreihenfolge im ersten Reproduktionsskript
(`repro_real_file.py`, dort historisch belassen). `ensureIdentityPlacements()` ist nachweislich
ein reiner No-Op für alle Objekte in der betroffenen Datei (`ElementCount=0` bei allen Links,
die Funktion greift laut Quellcode nur bei `ElementCount>0`). Isoliert bestätigt in
`repro_trigger1_doubleclick.py` und `repro_trigger2_setactiveobject.py`.

## ✅ Eigenständiges, portables Reproduktionspaket (`standalone_repro/`)

`standalone_repro/` enthält ein **funktionierendes, ortsunabhängiges** Repro-Paket:
`Assembly_repro.FCStd` + die zwei referenzierten Quelldateien (`CNC_M1_010_G.FCStd`,
`CNC_M1_011_P_Panel.FCStd`), plus `repro_run.py` zum automatischen Auslösen. Getestet von
einem komplett neuen Dateipfad aus (`/tmp/...`), reproduziert zuverlässig.

**Wichtige Erkenntnis beim Bauen dieses Pakets:** Ein `App::Link.LinkedObject`-Neuverweis +
`recompute()` (z. B. um Links auf lokale Kopien umzubiegen) **heilt den Bug unabsichtlich** -
vermutlich weil dabei die intern zwischengespeicherten Joint-`Placement1`/`Placement2`-Werte
neu berechnet und dadurch wieder konsistent mit der aktuellen Geometrie werden. Eine **reine
Byte-Kopie** der Originaldateien (kein Öffnen, kein Neuverknüpfen, kein Speichern) ist daher der
einzig zuverlässige Weg, das Bug-auslösende Datei-Innenleben unverändert zu übertragen - zum
Glück speichert FreeCAD hier ohnehin schon einfache, ordner-relative Pfade
(`file="CNC_M1_010_G.FCStd"`), sodass eine reine Kopie aller 3 Dateien in einen neuen,
gemeinsamen Ordner ausreicht.

Das spricht übrigens auch für die Ursachen-Hypothese: die zwischengespeicherten
Joint-Placement-Werte im Original scheinen mit der aktuellen Geometrie **absichtlich
inkonsistent** zu sein (weil sie seit dem letzten interaktiven Ziehen nie neu berechnet wurden)
- genau das bringt der Kalt-Solve durcheinander.

## Symptom

Beim Bearbeiten/Erstellen eines Joints (z. B. einer Gleitverbindung) in einer Baugruppe mit
bereits vorhandenen Joints springen andere, nicht am Joint beteiligte Bauteile dauerhaft auf
eine falsche Position. Nutzer-Repro: eine Kette aus 7 `App::Link`-Panels (PDM-Muster: jedes
Bauteil sein eigenes `.FCStd`-Dokument), über 6 Gleitverbindungen verbunden - beim Öffnen
*jedes* Joint-Dialogs sprangen mehrere Panels auf dieselbe Position.

## Tatsächliche Ursache (eingegrenzt, Mechanismus nicht abschließend bewiesen)

Der Auslöser ist **kein einzelner Funktionsaufruf**, sondern folgendes Muster:

1. Irgendetwas markiert das Dokument als "touched" (Objekt in den Edit-Modus bringen via
   `ViewObject.doubleClicked()`/`setActiveObject()`, ein Objekt löschen, o. ä.).
2. Der **nächste** `recompute()` ist dadurch kein No-Op mehr, sondern ein **kompletter
   "Kalt-Solve"** des gesamten Baugruppen-Graphen (im Gegensatz zu einem `assembly.solve(False)`,
   der die bereits zwischengespeicherten JCS-Werte nutzt und immer sauber bleibt).
3. Bei diesem Kalt-Solve haben Slider-Joints **ohne explizit gesetzten Distanz-/Offset-Wert**
   keine Information, um die zuvor durch interaktives Ziehen entstandene Position
   wiederherzustellen, und kollabieren auf eine (beliebige, aber constraint-gültige)
   Default-Lösung - hier: Koinzidenz mit der Nachbarposition in der Kette.

**Wichtig:** Das ist eine Arbeitshypothese für den *Mechanismus*, kein bewiesener Fakt - der
Trigger selbst (Punkte 1+2) ist dagegen mehrfach unabhängig bestätigt (siehe Kontrollmatrix).

## Reproduktion (Kontrollmatrix, zweite Runde)

| Skript | Setup | Ergebnis |
|---|---|---|
| `repro_trigger1_doubleclick.py` | `assembly.ViewObject.doubleClicked()` GANZ ALLEIN, kein weiterer Aufruf | ❌ **Kollabiert bereits** |
| `repro_trigger2_setactiveobject.py` | Nur `setActiveObject('Assembly', ...)`, kein recompute danach | ✅ Bleibt korrekt |
| `repro_trigger2_setactiveobject.py` | `setActiveObject(...)` **+ danach** `assembly.recompute(True)` | ❌ **Kollabiert, byte-identisch zu doubleClicked()** |
| `repro_trigger3_delete_recompute.py` | Kein Gui/ActiveObject/doubleClicked - nur `doc.removeObject(...)` + `doc.recompute()` | ❌ **Kollabiert ebenfalls** (dritter, unabhängiger Trigger) |
| `repro_minimal_2panel.py` | Reduziert auf Ground+2 Panels, 4 Joints, nur `doc.recompute()` | ❌ **Kollabiert zuverlässig - minimalster Fall** |
| `repro_control_solve_direct_stays_clean.py` | Direkter `assembly.solve(False)`-Aufruf (umgeht Dependency-Graph) | ✅ **Bleibt immer sauber** |
| `repro_control_no_joints.py` | Alle Joints entfernt | ✅ No-Op |
| `repro_control_plain_recompute.py` | Reines `recompute()` auf frisch geöffneter, konvergierter Datei | ✅ No-Op (nichts ist "touched") |
| `repro_control_minimal_synthetic.py` | Synthetische Links ohne Joints | ✅ No-Op |

→ Gemeinsamer Nenner aller fehlschlagenden Fälle: **Dokument wird "touched", danach ein
vollständiger Graph-Recompute.** Nicht `ensureIdentityPlacements()`, nicht der Solver an sich
(`solve(False)` bleibt sauber).

## Offene Fragen

- **Rein synthetischer Nachbau (generische `Part::Box`/`PartDesign::Body`, gleiche Topologie)
  reproduziert NICHT** (mehrere Versuche, u. a. `test_minimal_v3_topology.py`,
  `test_final_theory.py` - nicht ins Repo übernommen, da nur Negativbefunde ohne Repro-Wert).
  Irgendein Detail der echten Bauteil-Geometrie/-Referenzen scheint zusätzlich nötig zu sein
  (Kandidat: der erste Distance-Joint der echten Datei referenziert eine benutzerdefinierte
  Datum-Ebene in einem "Skelett_Body", keine einfache `Origin`-Ebene).
- **Dokumentübergreifende Architektur (PDM-Muster) scheint notwendig** - identische Topologie
  im selben Dokument (getestet mit direkten `Part::Box` und mit lokalen `App::Link`)
  reproduziert nicht. Auch das aber nicht isoliert von der übrigen echten Geometrie bestätigt.
- Exakte Codestelle im Solver (`AssemblyObject::solve()`/`jointParts()`/`OndselSolver`), die die
  Default-Lösung beim Kalt-Solve wählt, ist nicht instrumentiert/lokalisiert (bräuchte einen
  lokalen Rebuild mit Logging - bewusst nicht gemacht, siehe unten).

## Nicht gemacht

C++-Instrumentierung/Rebuild von `AssemblyObject.cpp` - als zu aufwendig/riskant im Verhältnis
zum bereits erreichten Erkenntnisstand eingeschätzt. Nächster nötiger Schritt, falls die
Wurzelursache abschließend geklärt werden soll: `solve()`/`jointParts()`/`fixGroundedParts()`
in einer lokalen Kopie mit Logging der Joint-Placement1/2-Werte vor/nach jedem
`findPlacement()`-Aufruf versehen, neu bauen, Trigger 2 gegen den `repro_minimal_2panel.py`-Fall
laufen lassen.

## GitHub-Issue-Entwurf (Englisch, noch NICHT eingereicht)

> Titel und Text unten sind zum Einreichen vorbereitet - Freigabe durch den Nutzer nötig,
> bevor irgendetwas an FreeCAD/FreeCAD auf GitHub gepostet wird.

---

**Title:** Assembly: chained Slider joints across App::Link parts silently lose their solved position on the next graph recompute (position collapses onto neighbouring part)

**FreeCAD version:** 26.3.0, Libs: 26.3.0devR47833 (git main, commit `0475b124bef1705ad45a548fc0314ada56915d93`, built 2026/07/26)

**Environment:** Ubuntu 24.04 (kernel 7.0.0), self-built from source (CMake), Qt6/PySide6.

**Steps to reproduce (attached files, confirmed reliable):**

1. Download and unzip the attached `standalone_repro.zip` anywhere.
2. Open `Assembly_repro.FCStd` in FreeCAD.
3. Note the 7 `...Panel...` parts each have a distinct Y position (-85, -50, -15, 20, 55, 90,
   125 mm).
4. Double-click the `CNC_M1_009_A_Y_Abstreifer` assembly object in the tree to enter Edit mode
   (or run the included `repro_run.py` with `FreeCAD -l repro_run.py`, which does the same
   thing headlessly and prints before/after positions).

No joint dialog needs to be opened and nothing needs to be dragged - entering Edit mode alone
is enough to trigger it.

**Expected behaviour:** all 7 panels keep their distinct, previously-saved Y positions - none
of the Slider joints between them have an explicit Distance/Offset value, so their last-saved
`Placement` (from earlier interactive dragging) should be treated as a valid, stable
equilibrium.

**Actual behaviour:** all 7 panels' Y position collapses onto -85 (the position of the first,
fully-grounded panel in the chain) as soon as Edit mode is entered - even though nothing was
dragged and no joint property was changed. The axis perpendicular to the slide (Z in this file)
is unaffected. We also confirmed two other, independent triggers produce the byte-identical
result: `Gui.ActiveDocument.ActiveView.setActiveObject('Assembly', assemblyObj)` followed by
`assembly.recompute(True)`; and deleting an unrelated object in the document and calling
`doc.recompute()`.

Symptom as originally observed by an end user (before we reduced it to the attached file):
finishing a Joint-creation/edit dialog with OK snapped the two jointed parts to this collapsed
state; Cancel snapped the *entire* assembly (all parts in the chain) to it - both are instances
of the same "forced cold recompute after the dialog's transaction commit/abort" trigger.

**Reproduced with:**
- The attached `standalone_repro.zip` (real production part geometry, reduced project) - 100%
  reliable across 3 independent trigger mechanisms, confirmed from multiple different
  filesystem locations.
- A reduced 2-panel extract of the same assembly (Ground + 2 `App::Link` parts + 4 joints) -
  100% reliable, no GUI required (plain `App::Document.recompute()` after `removeObject()` on
  an unrelated object is sufficient).

**Important finding while preparing this report:** re-linking the `App::Link` objects to local
copies of their source files and recomputing **silently "heals" the bug** - the re-saved file
no longer reproduces, even with identical topology and identical `Placement` values. Only an
untouched, byte-for-byte copy of the original files (no re-open, no re-link, no re-save)
preserves whatever triggers it. This suggests the affected joints' cached `Placement1`/
`Placement2` values are stale relative to the current part positions in the *original* file
(never refreshed since an earlier interactive drag), and it's specifically that staleness a
cold re-solve trips over - but we could not confirm this mechanism at the source level (see
below).

**Not yet reproduced:** a fully synthetic, from-scratch file using only generic
`Part::Box`/plain `PartDesign::Body` geometry (same joint topology, same cross-document
`App::Link` architecture, manually set `Placement` values) did **not** trigger the collapse in
several attempts - consistent with the "stale cache" theory above (a freshly-built file's joint
caches are never stale to begin with), but not confirmed as the actual mechanism.

**Suspected area:** `src/Mod/Assembly/App/AssemblyObject.cpp`, `solve()` and its helpers
(`jointParts()`, `fixGroundedParts()`), and/or the underlying `OndselSolver`
(`src/3rdParty/OndselSolver/`) - specifically how an unconstrained/underdetermined joint DOF is
seeded/resolved on a full re-solve versus how it was left by an interactive drag.

---

## Dateien in diesem Ordner

- `repro_trigger1_doubleclick.py` - `doubleClicked()` allein reproduziert bereits
- `repro_trigger2_setactiveobject.py` - Bisektion: `setActiveObject()` + `recompute()` ist der Kern
- `repro_trigger3_delete_recompute.py` - dritter, Gui-unabhängiger Trigger (Objekt löschen)
- `repro_minimal_2panel.py` - minimalster, zuverlässigster Fall (Ground + 2 Panels)
- `repro_control_solve_direct_stays_clean.py` - Kontrolle: `solve(False)` bleibt immer sauber
- `repro_control_no_joints.py`, `repro_control_plain_recompute.py`,
  `repro_control_minimal_synthetic.py` - Kontrollen aus der ersten Runde, weiterhin gültig
- `repro_real_file.py` - **historisch**, ursprünglicher (fehlgeleiteter) Reproduktionsversuch,
  siehe Korrektur-Hinweis oben und im Datei-Header
- `standalone_repro/` (+ `standalone_repro.zip`) - das einzige **wirklich eigenständige** Paket,
  siehe eigener Abschnitt oben. **Das ist die Datei, die an den GitHub-Issue angehängt werden
  sollte** - alle anderen Skripte hier brauchen weiterhin den privaten Originalpfad.

Alle Skripte AUSSER denen in `standalone_repro/` greifen auf die private Nutzerdatei
`3_Panels_aus_6_auswalbar.FCStd` zu (nicht im Repo enthalten) - Pfad im Skript anpassen, falls
du sie selbst nachvollziehen willst.

## Umgebung zum Ausführen

`FreeCADCmd` kann `JointObject.py` nicht importieren (PySide6-Konflikt mit dem projekteigenen
venv). Skripte, die Joints/`Gui`/`TaskAssemblyCreateJoint` brauchen, laufen mit dem vollen
`FreeCAD`-Binary + Offscreen-Qt:

```bash
source /home/maxx/Dokumente/FreeCAD-Development/.venv/bin/activate
PYSIDE_QT="${VIRTUAL_ENV}/lib/python3.12/site-packages/PySide6/Qt"
export QT_PLUGIN_PATH="${PYSIDE_QT}/plugins"
export LD_LIBRARY_PATH="${PYSIDE_QT}/lib:/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONNOUSERSITE=1
export PYTHONPATH="${VIRTUAL_ENV}/lib/python3.12/site-packages"
export QT_QPA_PLATFORM=offscreen
timeout 60 /home/maxx/freecad/install/bin/FreeCAD -l repro_trigger1_doubleclick.py
```

Skripte ohne Joint-Erzeugung/Gui-Zugriff (`repro_control_plain_recompute.py`,
`repro_control_minimal_synthetic.py`, `repro_trigger3_delete_recompute.py`) laufen einfacher
unter `FreeCADCmd` mit nur `PYTHONPATH`/`PYTHONNOUSERSITE` gesetzt.

FreeCAD-Version: `26.3.0, Libs: 26.3.0devR47833` (selbst gebaut, siehe `../../CMakeLists.txt`
und Projekt-Memory für den Build-Kontext).
