# Plan: restliche Gelenktypen in die Sandbox-Architektur übernehmen

Stand 2026-10-28. Nutzerauftrag: „Planung für die Übernahme der restlichen Joints in die Sandbox“.

## Ziel

Jeder Gelenktyp verhält sich in der Sandbox so wie die fünf bereits abgesicherten Typen (Fixed,
Revolute, Cylindrical, Slider, Ball): flach, verschachtelt (flexibel), doppelt verschachtelt,
mehrfach eingebaute Unterbaugruppe, beim Ziehen. Es gelten die bestehenden Regeln:
- „Adressieren statt Kopieren“: Teile und Marker über `nestingPrefix` und IdentityGraph auflösen
  (`resolvePartForMbD()`, `handleOneSideOfJoint()`), nie über die adressierungsblinde
  `getMovingPartFromRef()`.
- Solver schreibt nur Spiegel, nie Originale; Originale in Unterdokumenten bleiben unverändert.
- `.FCStd`-Kompatibilität, keine neuen persistenten Eigenschaften ohne Rückfrage.

## Bestandsaufnahme

| Gelenktyp | Tests heute | Sonderweg im Code | Einschätzung |
|---|---|---|---|
| Fixed, Revolute, Cylindrical, Slider, Ball | flach, verschachtelt, doppelt, Duplikate, Ziehen | – | erledigt |
| Tangent (Zweig `tangent-joint`) | flach, Solver-Tests | eigener Solver-Teil | Merge nach GUI-Abnahme |
| Parallel, Perpendicular, Angle | keine | keiner (reine Orientierung) | geringes Risiko, nur Tests |
| Distance (viele Unterarten, `DistanceType`) | keine | `getDistanceType()`, `swapJCS()` schreibt ins Gelenk | mittel – Swap bei verschachtelten Gelenken prüfen |
| RackPinion | keine | `getRackPinionMarkers()`: Teil schon über `resolvePartForMbD()`, Lagen aber über `getGlobalPlacement()` ohne Container-Kette; `slidingPartIndex()`, `swapJCS()` | hoch – halb umgestellt |
| Screw | keine | braucht zugehörigen Slider | mittel |
| Gears, Belt | keine | `findRotationCarrierForGearSide()`/`setGearJointCarrierMarkerIfAvailable()`: `getMovingPartFromRef()`, `getJointSideGlobalPlacement()`, kein `nestingPrefix` | hoch – adressierungsblind |

Querschnittsthema: `swapJCS(joint)` (in `getDistanceType()`, `getRackPinionMarkers()`,
Tangent) tauscht Referenz/Placement **im Gelenkobjekt selbst**. Gehört das Gelenk zu einer
Unterbaugruppe (über `nestingPrefix` geholt), wird damit das Original im Unterdokument
verändert. Zu prüfen, ob das in der Praxis passiert und ob es beim Speichern bleibt.

Drag-Rundumtest (`tools/drag_sweep.py`) zieht heute nur Slider, Revolute, Cylindrical.

## Phasen

Jede Phase: Tests erst analytisch, dann Vanilla (Weekly-AppImage), dann Sandbox
([[feedback-autonomous-headless-testing]]); volle Matrix + Drag-Rundumtest; Commit nach Rückfrage;
Patch nach PATCHES.txt-Ablauf.

### Phase 0 – Testgerüst für alle Gelenktypen
- Generischen Fixtur-Baukasten erweitern (`nested_test_utils.build_fixture_generic()`): Gelenktyp
  als Parameter, je Typ eine analytische Prüffunktion (Abstand, Winkel, Übersetzung
  Zahnrad/Riemen/Zahnstange/Schraube).
- Fixtur-Methodik: feste, versionierte Ausgangsdateien je Testfall
  ([[reference-nested-fixture-methodology]]).
- Drag-Rundumtest um die Typen mit Bewegung erweitern (Screw, RackPinion, Gears/Belt über ihr
  Trägergelenk).

### Phase 1 – Bestandsaufnahme gegen Vanilla
- Für jeden Typ flach + verschachtelt + doppelt verschachtelt + Duplikat: Vanilla und Sandbox
  laufen lassen, Ergebnisse als Tabelle festhalten. Zeigt, wo wir schon besser/gleich/schlechter
  sind, bevor Code angefasst wird.

### Phase 2 – Querschnitt `swapJCS()` bei verschachtelten Gelenken
- Nachweisen (Test: Original-Unterdokument vor/nach Solve vergleichen, wie
  `test_originals_untouched`).
- Falls bestätigt: Reihenfolge der Referenzen nur im Speicher tauschen (lokale Sicht für
  `makeMbdJoint()`), das Gelenkobjekt nicht schreiben – oder nur, wenn das Gelenk im aktiven
  Dokument liegt.

### Phase 3 – Orientierungsgelenke (Parallel, Perpendicular, Angle)
- Erwartung: nur Tests, kein Code. Sonst Befund → eigener Schritt.

### Phase 4 – Distance
- Je Unterart (Punkt–Punkt, Punkt–Linie, Punkt–Ebene, Linie–Linie, Ebene–Ebene,
  Zylinder/Kugel-Kombinationen …) ein flacher und ein verschachtelter Fall; Offset und Vorzeichen.

### Phase 5 – Kopplungsgelenke (RackPinion, Screw, Gears, Belt)
- Trägersuche und Markerbau auf `resolvePartForMbD()` + IdentityGraph + Container-Kette umstellen
  (wie `handleOneSideOfJoint()`), `nestingPrefix` durchreichen.
- Tests inkl. Ziehen und Simulation (Übersetzungsverhältnis über mehrere Schritte konstant).

### Phase 6 – Tangent zusammenführen
- Nach GUI-Abnahme durch den Nutzer: Zweig `tangent-joint` nach `solver-sandbox` (Fast-Forward
  bzw. Rebase auf aktuellen Stand), dann die Tangent-Tests in die Matrix.

## Reihenfolge und Aufwand (grob)

| Phase | Aufwand | Risiko |
|---|---|---|
| 0 Testgerüst | mittel | gering |
| 1 Bestandsaufnahme | gering–mittel | gering |
| 2 swapJCS | gering–mittel | mittel (betrifft mehrere Typen) |
| 3 Orientierung | gering | gering |
| 4 Distance | mittel | mittel |
| 5 Kopplung | hoch | hoch |
| 6 Tangent | gering | gering |

Vorschlag: 0 → 1 → 2 → 3 → 4 → 5, Tangent (6) unabhängig davon, sobald abgenommen.

## Stand 2026-10-28: Phase 0 umgesetzt, erste Bestandsaufnahme

Testgeruest `standalone-check/kinematic-tests/jointtype-matrix/` (`jointtype_matrix.py`,
`build_fixtures.py`, `run_matrix.py`), 12 Faelle x 4 Kontexte = 48 Fixturen unter `fixtures/`.
Noch nicht committet.

Erster Lauf (Sandbox `1ec79da434`+Patches / Vanilla Weekly 2026.10.21):

| Fall | Sandbox flat/nested/double/duplicate | Vanilla |
|---|---|---|
| parallel, perpendicular, angle, distance_pointpoint/pointplane/planeplane/lineplane | alle OK | flat OK, alle verschachtelten FEHLER |
| distance_lineline | OK / FEHLER / OK / FEHLER | flat OK, Rest FEHLER |
| gears | OK / OK / OK / FEHLER (Verankerung 2. Instanz) | AUSNAHME* |
| belt | OK / FEHLER / FEHLER / FEHLER (Verhaeltnis falsch, Verankerung) | AUSNAHME* |
| rackpinion | OK / OK / FEHLER / OK | AUSNAHME* |
| screw | alle OK | AUSNAHME* |

\* Vanilla hat `preDrag()`/`doDragStep()` nicht in Python - fuer den Vanilla-Vergleich der
Mechanismen eine statische Variante noetig (z.B. Antriebsteil verdrehen und `solve()`).

Erste Einordnung (noch zu pruefen):
- Mehrere „Verankerung … 180 Grad“-Fehler (distance_lineline duplicate, belt) passen zur
  bekannten Mehrdeutigkeit (todo-duplicate-instance-independent-180-degree-roots) - oder die
  Fixtur-Startlagen (LINK_DROP) sind zu weit weg; Startlagen naeher waehlen und erneut pruefen.
- rackpinion__double: gemessener Drehwinkel -338.96 Grad statt +24 - Messfehler im Test
  (`_angle_about()` normiert nicht auf -180..180), Ergebnis vermutlich korrekt. Test anpassen.
- belt nested/double: Verhaeltnis falsch trotz flach OK -> echter Kandidat (Phase 5:
  Traegersuche/Marker adressierungsblind).

## Stand 2026-10-27 (nachmittags): Ursachen geklaert, Matrix Sandbox 48/48 OK

- `_angle_about()` normiert jetzt auf (-pi, pi] -> rackpinion__double OK (war Messfehler).
- **Riemen und weiter Start:** Die Fixturen legten die Unterbaugruppe 25 Grad + 40 mm neben die
  Verankerung. Beim Riemen divergiert Newton dann, die Redundanz-Elimination verwirft
  Drehgleichungen der Erdungen und der aeusseren Verankerung, solve() meldet trotzdem rc=0.
  **Flacher Nachbau** (BoxA ungeerdet, Fixed an geerdete BoxC, Inhalt versetzt) zeigt dasselbe
  **auch in Vanilla** (Verankerung 79 Grad daneben, Vanilla verdreht dabei sogar die geerdete
  BoxC). Kein Verschachtelungsfehler, sondern Solver-Einzugsgebiet bei Gear/Belt-Kopplung.
  Zahnraeder (positives Verhaeltnis) konvergieren aus derselben Lage.
- **Echter Sandbox-Fehler dabei gefunden und behoben:** `computeGroundCorrection()` biegt nur
  EIN geerdetes Teil exakt zurueck; weitere geerdete Koerper (hier das Origin) bekamen nach dem
  divergierten Solve die verdrehte Lage und wurden mitgespeichert (5 Fixturen hatten ein
  verdrehtes Origin). Fix: `fixGroundedParts()` merkt sich alle fixierten Koerper
  (`groundedBodies`), `setNewPlacements()` schreibt sie nie.
- **duplicate:** Die zweite Instanz war an Face4 (+50 Grad) verankert, musste also immer um
  ~90 Grad und mehr gedreht werden (180-Grad-Mehrdeutigkeit). Fixturen jetzt: Startlage jeder
  Instanz = erfuellte Verankerung mal maessige Stoerung (`LINK_PERTURB`, 10 bzw. 12 Grad, wenige mm).

Ergebnis mit neu gebauten Fixturen:

| | flat | nested | double | duplicate |
|---|---|---|---|---|
| Sandbox, alle 12 Faelle | OK | OK | OK | OK |
| Vanilla, Paar-Gelenke | OK | FEHLER | FEHLER | FEHLER (ausser planeplane) |
| Vanilla, Mechanismen | AUSNAHME* | AUSNAHME* | AUSNAHME* | AUSNAHME* |

Vorzeichen der Uebersetzung in allen Kontexten gleich: Gears -, Belt +, RackPinion +, Screw -.
Volle Testreihe (run-all-tests.sh) mit dem Fix gruen.

Folgerung fuer Phase 5: Gears/Belt/RackPinion/Screw funktionieren verschachtelt bereits,
obwohl `findRotationCarrierForGearSide()` adressierungsblind ist - der Traeger-Marker greift
nur bei koaxialen Gelenk-Achsen (`gearJointNeedsCarrierMarker()`), die Matrix hat keinen
solchen Fall. Naechster Testfall: koaxiale Zahnraeder/Riemen (Traeger-Pfad) verschachtelt.
Offen ausserdem: Vanilla-faehige Mechanismus-Pruefung ohne preDrag (BoxD verdrehen + solve()).

### Nachtrag 2026-10-27 abends: koaxiale Zahnraeder (Traeger-Pfad)

Neuer Fall `gears_coaxial` (Upstream "Coaxial gear joint" #31155, Traeger-Marker noetig).
flat/nested/double liefen sofort, **duplicate scheiterte**: "Coaxial GearJoint requires a carrier
marker" - `findRotationCarrierForGearSide()` verglich ueber `getMovingPartFromRef()` (Original),
fand jedes Drehgelenk einmal je Instanz und hielt das fuer mehrdeutig.
Fix: Zahnrad- und Traegerseite ueber `resolvePartForMbD()` mit dem jeweiligen `nestingPrefix`,
Traeger-Marker ueber `handleOneSideOfJoint(..., prefix)`, Markername per `jointInstanceName()`
(sonst zwei gleichnamige Marker). `resolvePartForMbD()` dafuer public.
Matrix 52/52, volle Testreihe gruen (aus dem Build-Baum).

Absturz nach gescheitertem Solve geklaert: `preDrag()` ignorierte den Rueckgabewert von
`solve()`, `doDragStep()` rechnete auf dem halb aufgebauten Modell (SIGSEGV in
`DirectionCosineIeqcJec::calcPostDynCorrectorIteration`). Fix: Modell verwerfen wie im
Insel-Zweig. Regressionstest `test_drag_after_failed_solve` (koaxiale Zahnraeder ohne
gemeinsamen Traeger), Testreihe 37/37. Upstream-Kandidat #13.

### Phase 2 umgesetzt (2026-10-27): Solve schreibt kein Gelenk mehr um

Nachweis: neue Faelle `distance_pointfirst`/`distance_edgefirst` (Punkt bzw. Kante als
Referenz 1), gebaut ohne Solve (`SolveInJointCreation`/`SolveOnRecompute` aus - mit
Standardeinstellungen tauscht schon das eigene Dokument beim Anlegen/Recompute und speichert
das). Neue Pruefung in run_matrix: Reference1/2 + Placement1/2 aller Gelenke in Unterdokumenten.
Alter Code: "Gelenk-Originale veraendert" in nested/double/duplicate (sub#Joint Reference1/2,
Placement1/2).

Fix: `getDistanceType(joint, &swapSides)` meldet den Tausch nur noch; `makeMbdJoint()` nimmt
dann Referenz 2 als Marker I. Ebenso Screw (Slider zuerst) und `getRackPinionMarkers()`
(Zahnstange zuerst). Vertauschte Laengen-/Winkelgrenzen werden nur lokal sortiert, nicht
zurueckgeschrieben. Kein Solve-Pfad ruft `swapJCS()` mehr auf (Funktion bleibt fuer Aufrufer
ausserhalb). Hinweis fuer Phase 6: Zweig `tangent-joint` nutzt `swapJCS()` - beim Merge
auf dieselbe Sicht umstellen.

Ergebnis: Gelenk-Originale ueberall unveraendert, Testreihe 37/37, Matrix 59/60.
Offen (nicht Phase 2): `distance_edgefirst__duplicate` divergiert aus der Startlage (auch mit
altem Code). Allgemein: Flaeche am beweglichen Teil weit von dessen Ursprung + Abstand > 0
konvergiert nicht (Punkt-Ebene, auch Vanilla) - Newton dreht statt zu schieben. Eigener Punkt
(Solver-Robustheit, ggf. Upstream).

### OndselSolver-Fehler gefunden (2026-10-27): Ebene am beweglichen Teil

Das "Konvergenzproblem" aus Phase 2 war ein echter Rechenfehler in OndselSolver:
`DispCompIecJecKeqc::calcPostDynCorrectorIteration()` setzt die Nebendiagonale der zweiten
Ableitung nach den Euler-Parametern des Bezugsrahmens K mit `at(i)` statt `at(j)`. Betrifft jede
Translationsbedingung (z.B. Abstand Punkt-Ebene), deren Bezugsebene an einem beweglichen Teil
haengt. Minimal: Ebene an beweglicher BoxB, Punkt an geerdeter BoxA, exakter Start, Abstand 0 -> 8
scheitert (Sandbox alt und Vanilla). Fix ein Zeichen, Test `test_distance_plane_on_moving_part`
(alt/Vanilla FAIL, neu PASS). Upstream-Kandidat #14.

`distance_edgefirst__duplicate` war dagegen ein Fixturmangel: bei deckungsgleichen JCS steht die
Kante senkrecht auf der Ebene (singulaer fuer "parallel"), Start jetzt mit Kante in der Ebene
(`LINE_IN_PLANE`), auch fuer `distance_lineplane`. Matrix 60/60, Testreihe 38/38.

### Phase 1 abgeschlossen (2026-10-27): Vergleich mit Vanilla, alle 15 Faelle

Mechanismen jetzt auch per Simulation (`simulate_mechanism()`: Motion "Angular" am Drehgelenk
von BoxD, Messung erstes/letztes Bild) - Vanilla nimmt diesen Weg automatisch (kein preDrag in
Python), Sandbox per `JTM_MODE=sim` (20/20).

| | flat | nested | double | duplicate |
|---|---|---|---|---|
| Sandbox (installiert, Ziehen) | 15/15 | 15/15 | 15/15 | 15/15 |
| Vanilla Weekly 2026.10.21 | 15/15 | 0/15 | 0/15 | 1/15 (planeplane) |

Flach liefern beide dieselben Uebersetzungen und Vorzeichen (Gears/koaxial -0.5, Belt +0.5,
RackPinion 5 mm/rad, Screw 2 mm/U). Vanilla verschachtelt: Verankerung falsch, Simulation bewegt
BoxD nicht, double veraendert Originale. Den OndselSolver-Hesse-Fehler deckt die Matrix nicht ab
(Ebene stets am geerdeten Teil) - dafuer test_distance_plane_on_moving_part (Vanilla FAIL).

### 2026-10-28: Schrauben-Gelenk in BG37 ohne Wirkung (Nutzerfund)

BG37: Schraube `Joint016` Gewindestange100 <-> BackslashNut; die Mutter ist per Fixed am
Spindelhalter, dieser per Fixed am Linearlager, erst das Linearlager hat den Slider.
`slidingPartIndex()` suchte den Slider nur direkt am Teil -> 0 -> Gelenk ohne Meldung verworfen
(Simulation: Mutter steht). Fix: `getFixedGroupOf()` (starre Gruppe ueber Fixed), Slider an
irgendeinem Teil der Gruppe zaehlt; Teile ueber `resolvePartForMbD()` mit Praefix. Gilt auch fuer
RackPinion. BG37-Kopie: Mutter wandert 3.0000 mm bei 6 Umdrehungen x 0.5 mm. Neuer Matrixfall
`screw_fixed_chain` (alt und Vanilla FAIL, neu PASS). Matrix 64/64, Testreihe 38/38.
Upstream-Kandidat #15.
