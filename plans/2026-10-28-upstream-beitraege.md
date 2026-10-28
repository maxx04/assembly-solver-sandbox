# Plan: Sandbox-Korrekturen schrittweise nach FreeCAD einbringen (Weg B)

Stand 2026-10-28. Nutzerentscheidung 2026-10-27: Weg B probieren („obwohl ich skeptisch bin“).

## Spielregeln

- FreeCAD nimmt KI-unterstützten Code mit Kennzeichnung an (z.B. Upstream-Commit `4218d22c3a`:
  „Assisted-by: gpt-6.1“). Texte von Issues/PRs schreibt der Nutzer selbst
  ([[feedback-freecad-ai-policy-human-written]]); ich liefere Faktenblatt, Patch, Test.
- Vor jeder Aussage über Vanilla gegen die aktuelle Weekly prüfen
  ([[feedback-verify-vanilla-before-external-publish]]).
- Kleine, einzeln prüfbare Pakete; mit dem kleinsten, klarsten anfangen und die Reaktion abwarten.
- Jeder Beitrag: Fehler in Vanilla nachgestellt, minimaler Patch gegen Upstream-`main`,
  Test oder Reproduktionsskript.

## Kandidaten

| # | Thema | Art | Umfang | Vanilla geprüft | Unterlagen |
|---|---|---|---|---|---|
| 1 | Starre Gruppe: Traceback beim Anlegen, Warnung bei jedem Öffnen | Python-Fix, 2 Stellen | sehr klein | ja (Weekly 2026.10.21) | `bugreports/rigid-group-restore-warning/` |
| 2 | Material-Editor schreibt leeren Bool, Bibliothek danach unlesbar; Dezimalkomma | Issue (Material-Modul, nicht unser Code) | – | ja | `bugreports/material-empty-bool-stoi/` |
| 3 | OndselSolver: `isConvergedToNumericalLimit()` falscher Index + Ganzzahldivision | C++, wenige Zeilen | klein | ja (früher) | `bugreports/ondselsolver-newton-convergence-check/` (#33672 wegen KI-Text geschlossen – neu mit eigenem Text) |
| 4 | OndselSolver: Schwellen-Pivotisierung im schnellen Löser | C++ | klein–mittel | ja (früher) | `bugreports/ondselsolver-markofast-pivot-threshold/` (#33673 dito) |
| 5 | Solve beachtet Gelenkgrenzen nicht (nur Ziehen prüft sie) | C++ | klein | laut Notiz auch Vanilla – neu prüfen | Sandbox-Commit „Grenz-Durchgang nach runPreDrag“ |
| 6 | Gelenk-Referenz durch Body-Feature („Link.BaseFeature.Edge9“ → 90° verdreht) | C++/Python | klein | laut Notiz Upstream-Bug – neu prüfen | Sandbox-Fix, Test `test_body_basefeature_reference_flat` |
| 7 | Drehfeld im Gelenk-Dialog springt bei 180° | Python/Gui | klein | Bugreport war vorbereitet | `bugreports/joint-offset-rotation-spinbox-jump/` |
| 8 | Drehformel beim Ziehen mischt lokale und globale Lage | C++ (ViewProviderAssembly) | klein–mittel | offen | TODO PRIO 2 |
| 9 | Link/Placement bei PartDesign-Feature (`getPlacementOf`) | C++ (src/App) | klein | aus FCProject, neu prüfen | `patches/2026.09.04-…getplacementof…` |
| 10 | OndselSolver-Dateileser: Zeilen in `assert()` gelesen (Release liest um eine Zeile versetzt), `readMarkers()` liest Rest der Datei | C++ | sehr klein | nachgestellt (Release-Build) | nur auf Zweig `tangent-joint` umgesetzt |
| 11 | Part_LinkArrays „Startchaos“ beim Laden | ? | ? | laut Notiz auch Vanilla | Notiz todo-early-solve-during-restore-warnings |
| 13 | Absturz beim Ziehen nach gescheitertem Solve: `preDrag()` ignoriert den Rueckgabewert von `solve()`, `doDragStep()` rechnet auf halb aufgebautem Modell (SIGSEGV in OndselSolver) | C++, wenige Zeilen | sehr klein | Code identisch in 1ec79da434; Absturz in Vanilla per GUI noch nachzustellen (Vanilla hat preDrag nicht in Python) | Test `test_drag_after_failed_solve` |
| 14 | OndselSolver `DispCompIecJecKeqc::calcPostDynCorrectorIteration()`: Nebendiagonale der Hesse-Matrix nach E(K) mit `at(i)` statt `at(j)` - Abstand Punkt-Ebene (u.a. TranslationConstraint) mit Ebene am beweglichen Teil divergiert ("iterNo > iterMax") | C++, ein Zeichen | sehr klein | ja, Weekly 2026.10.21 scheitert identisch | Test `test_distance_plane_on_moving_part` |
| 15 | Schrauben-/Zahnstangen-Gelenk wird ohne Meldung verworfen, wenn der Slider nicht direkt am Teil haengt, sondern an einem per Fixed damit verbundenen (`slidingPartIndex()`) | C++ | klein | ja, Weekly 2026.10.21: Mutter bewegt sich nicht (Matrix `screw_fixed_chain` flat) | Fall `screw_fixed_chain`, BG37 |
| 16 | Eindeutige Gelenk-Orientierung (Fixed/Slider/Dreh/Zylinder) statt 180-Grad-Wurzeln, Fortsetzung #32477 | C++ OndselSolver + FreeCAD-Variantenwahl | mittel | ja, Weekly 2026.10.21 landet auf 180° y/z | `bugreports/joint-orientation-unique-constraints/` (Solver-Patch auf 4be80ee sauber, baut) |
| 12 | Verschachtelte Baugruppen allgemein (Adressieren statt Kopieren, Erdung als Kontext, starre Gruppen über Grenze …) | großer Umbau | sehr groß | Vanilla falsch (Tests) | ARCHITECTURE.md – erst nach Vertrauensaufbau, vorher mit Maintainern besprechen |

## Vorgeschlagene Reihenfolge

1. **#1 Starre Gruppe** – klein, Python, klar nachweisbar, Faktenblatt fertig. Guter Testlauf, wie
   ein Beitrag aufgenommen wird.
2. **#2 Material** – nur ein Issue (Nutzertext), Material-Team entscheidet selbst über die Lösung.
3. **#10 OndselSolver-Leser** – sehr klein, eindeutig (Release vs. Debug); OndselSolver-Issues sind
   abgeschaltet, also direkt als PR gegen FreeCAD/OndselSolver bzw. über das Submodul.
4. **#3/#4 Solver-Konvergenz** – neu einreichen mit eigenem Text, Zahlen aus BG22 als Beleg.
5. **#5, #6, #7, #8, #9** – je nach Rückmeldung.
6. **#12** – nur nach Absprache mit den Assembly-Maintainern (z.B. Diskussion/Forum zuerst).

## Je Beitrag zu tun (Checkliste)

- [ ] In der aktuellen Weekly nachstellen (Skript im `bugreports/`-Ordner).
- [ ] Minimalen Patch gegen aktuelles Upstream-`main` erzeugen (eigener Zweig, nicht der
      Sandbox-Rundum-Patch).
- [ ] Upstream-Tests laufen lassen (`AssemblyTests`), eigenen Test ergänzen, wo sinnvoll.
- [ ] Faktenblatt aktualisieren; Nutzer schreibt Issue-/PR-Text.
- [ ] Nach Einreichung Status hier nachtragen.

## Offen

- Wo der Nutzer einreicht (eigener GitHub-Fork von FreeCAD nötig?), wer reviewt.
- Ob #10 zuerst vom Tangent-Zweig nach `solver-sandbox` übernommen wird.
