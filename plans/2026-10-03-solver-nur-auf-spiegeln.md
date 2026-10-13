# Plan: Solver arbeitet im Kontext einer BG nur auf deren Spiegeln, nie auf Originalen

## Kontext

**Anlass (2026-10-03, BG37 mit eingebauter BG43):**
- Der Rotor dreht nicht, und die Gewindestange ist nicht koaxial.
- In BG37 sind die Spiegel der verschachtelten Container (`067_A_NEMA17x33_III`, `047_…`) gegenüber ihren echten Gegenstücken in BG43 verschoben und gedreht (142° bzw. 153°).
- Die GUI zeichnet über die Spiegel-Kette, der Solver rechnet über die **echte** Container-Kette der Unterdokumente. Deshalb startet `Joint014` (Träger ↔ Motor) im Solver 59 mm und 86° daneben, und der Solve scheitert mit `iterNo > iterMax`.

**Grundproblem („Adressieren statt Kopieren“, ARCHITECTURE.md §5):**
- **Lesen:** Der Solver liest aus den **Originalen** in den Unterdokumenten.
- **Schreiben:** Er schreibt dorthin zurück und kopiert danach den lokalen Wert in alle Spiegel (`syncLocalMirrorPlacement()`). Dazu gehören auch die Spiegel in Zwischendokumenten, z.B. BG43s eigene, wenn in BG37 gelöst wird.

**Folgen:**
- Eine äußere BG verändert Originale.
- Das Ergebnis hängt davon ab, was sonst geladen ist (BG43-Drift nach separatem Speichern von BG67).
- GUI und Solver laufen auseinander, sobald ein Container-Spiegel abweicht.

**Nutzerentscheidung (2026-10-03):** Das Original darf von außen nie verändert werden. Wo es Spiegel gibt, liest und schreibt der Solver nur die Spiegel der aktiven BG. Der IdentityGraph ist die Brücke. Rigide Unterbaugruppen dürfen notfalls ganz verboten werden, wenn sie den Umbau verkomplizieren.

## Prinzip (Invariante)

> Im Kontext einer Baugruppe A ist jeder Solver-Körper genau ein Objekt **im Dokument von A** (die
> „Instanz“: ein lokales Teil oder A's eigener Spiegel). Startlage, Fixierung, Rückschreiben und
> Ziehen betreffen nur diese Instanz. Ihre Weltlage ist: lokale Placement mal die Kette der
> **Container-Spiegel in A's Dokument**.
> Ein Original wird nur gelöst und geschrieben, wenn sein eigenes Dokument die aktive BG ist.

Damit sind GUI und Solver per Konstruktion deckungsgleich: Beide nutzen dieselbe Kette.

## Umsetzung

### 1. IdentityGraph: Instanz und Instanz-Kette (`AssemblyIdentityGraph.{h,cpp}`)
- **Neu `App::DocumentObject* instanceOf(const IdentityHandle&)`:** liefert das Objekt im Dokumentbaum von `rootAssembly`, das auf das Handle auflöst.
  - Gesucht wird nur über `rootAssembly->Group` und die `Group` der **Spiegel**-AssemblyLinks. Anders als `collectCandidates()` steigt die Suche **nicht** in `getLinkedAssembly()->Group` ab.
  - Für flache Teile ist das das Objekt selbst. Bei Duplikaten ergibt sich über den Instanzpfad automatisch der Spiegel der richtigen Instanz.
  - Ergebnis pro Graph cachen.
- **`materializeForObjectPartMap()` liefert ab jetzt `instanceOf(h)`** und fällt auf `templateObj` zurück, wenn es keine Instanz gibt. Diese eine Funktion ist bereits der zentrale Solver-Schlüssel, siehe `resolvePartForMbD()`, `canonicalizeForMbD()`, `getGroundedParts()` und `fixGroundedParts()`. Damit wechseln alle Schlüssel gleichzeitig.
- **`containerChainPlacement()` nutzt für Instanzen die lokale Kette:** `findLocalGroupPath()` liefert die Container-Spiegel in A's Dokument. Den tiefen Abstieg über echte Dokumente (`findContainerChain()`) gibt es nur noch als Rückfall für Objekte ohne Instanz.
- **`resolveForUi()`/`UiHandle`** nutzen `instanceOf()`, damit die GUI dieselbe Instanz bekommt.

### 2. Solver-Pfad (`AssemblyObject.cpp`)
- **`getMbDData()`:** unverändert, rechnet jetzt aber automatisch mit Instanz und lokaler Kette, weil Schlüssel und Kette aus Schritt 1 kommen.
- **`setNewPlacements()`:** schreibt nur noch den Schlüssel (die Instanz). Der Aufruf von `syncLocalMirrorPlacement()` entfällt, ebenso der Abschluss-Durchlauf mit `mirrorsOf()` (es gibt pro Instanz keinen zweiten Spiegel in A's Dokument). `syncLocalMirrorPlacement()` wird entfernt, falls danach unbenutzt.
- **`validateNewPlacements()`, Drag-Pfad (`preDrag`, `doDragStep`, `postDrag`) und `undoSolve()`/`savePlacementsForUndo()`:** laufen über `objectPartMap`, sind also automatisch auf Instanzen. Ich prüfe jeweils, dass dort kein direkter Schreibzugriff auf `getMovingPartFromSel()`-Ergebnisse (echte, tiefe Objekte) übrig ist. Gefundene Stellen bilde ich über `canonicalizeForMbD()` auf die Instanz ab.
- **`getGroundedParts()`/`fixGroundedParts()`:** bleiben, wie sie sind (Vergleich und Fixierung über den Solver-Schlüssel), wechseln aber automatisch auf Instanzen.

### 3. Container-Spiegel immer von der Quelle übernehmen (`AssemblyLink.cpp`)
- In `synchronizeComponents()` wird für Kinder vom Typ AssemblyLink (Container) die Placement **auch bei flexiblen Links** von der Quelle auf den Spiegel kopiert (`syncPlacements()`), nur in diese Richtung.
- Spiegel der Blatt-Teile bleiben bei flexiblen Links Zustand der Instanz (der Solver schreibt sie).
- Damit verschwindet der BG37-Schaden beim nächsten Recompute. Der Solver korrigiert danach die Teile.

### 4. Ziehen ganzer Unterbaugruppen begrenzen (`AssemblyUtils.cpp` `getMovingPartFromSel`, `ViewProviderAssembly.cpp` `collectMovableObjects`)
- „Unverbundene flexible BG als Ganzes ziehen“ nimmt nur noch einen AssemblyLink, der **direkt** in der aktiven BG liegt. Nur dessen Placement ist Instanzzustand der aktiven BG.
- Verschachtelte Container-Spiegel werden nie als Ganzes gezogen. Sonst entstünde sofort wieder eine Abweichung, die Schritt 3 überschreiben würde.

### 5. Was entfällt oder vereinfacht wird (prüfen, nicht blind löschen)
- **Wird unnötig:** `syncLocalMirrorPlacement()` und die Spiegel-Nachführung in `setNewPlacements()`.
- **Prüfen:** `solveHiddenFlexibleUnderRigid()` mit `propagateSolvedPlacementsTo()` (flexibel hinter rigide) löst heute die verschachtelte BG **in ihrem eigenen Dokument** und schreibt deren Originale. Das widerspricht der Invariante. Ich prüfe, ob es sich auf das Lösen der Spiegel im Kontext von A umstellen lässt. Wird das zu kompliziert, kommt es zur Entscheidung zu dir (rigid einschränken oder verbieten).

### 6. Altschäden nur prüfen, nicht ändern (Skript, Ausgabe als Liste)
Das Skript vergleicht für das Projekt `PROJ_CNC3018`:
- Origins mit Placement ungleich Identität,
- Original-Teile in den Unterdokumenten gegenüber dem letzten Projekt-Commit,
- Container-Spiegel gegenüber der Quelle.

Die Ausgabe ist eine Liste für dich. Geändert wird dabei nichts.

### Nicht im Umfang
- Python-Seite der Joints (`Placement1/2` sind relativ zum Teil, also unabhängig von der Kette).
- Persistente Properties: keine neuen. Die Dateikompatibilität bleibt erhalten, nur der Laufzeit-Datenfluss ändert sich.

## Risiken
- **Bestehende Tests erwarten das Gegenteil:** `check_sync()` in `nested_test_utils.py` prüft „Original == Spiegel nach dem äußeren Solve“. Das wird per Anforderung falsch. Neue Erwartung: Das Original bleibt unverändert, und die Spiegel erfüllen die Joints. Die betroffenen Tests (nested, double_nested, duplicate) werden angepasst. Die Analyse-Erwartungen bleiben, sie werden nur an den Spiegeln geprüft.
- **Neuladen:** Bisher hat „Original synchron“ nach dem Neuladen eventuell kaschiert, dass Spiegel-Placements gespeichert werden. Jetzt muss das Spiegel-Ergebnis über Speichern und Neuladen der äußeren BG erhalten bleiben. Die Tests prüfen das schon (`save_close_reopen_recompute_nested()`).
- **Drag-Hotpath:** `instanceOf()` sucht mit `resolveObject()` pro Kandidat. Deshalb wird pro Graph gecacht, und es wird beim Drag einmal gemessen.
- **Duplikate (180°-Baseline-Fehler):** Das Verhalten kann sich ändern, besser oder schlechter. Die Meldungen werden vorher und nachher verglichen.

## Verifikation
1. **Neuer Test `test_originals_untouched`:** läuft über alle verschachtelten Fixturen.
   - Originale in allen Unterdokumenten werden vor dem äußeren Solve gesichert, danach Recompute und 3× Solve.
   - Erwartung: Originale bitgenau unverändert.
   - Jeder Joint ist über die GUI-Referenzen erfüllt (`getJcsGlobalPlc`).
   - Kein Drift.
2. **Bestehende Tests:** `test_nested_grounding_context_isolation` und `test_identitygraph_handle_consistency` bleiben grün.
3. **Volle Matrix:** Die Tests mit angepasster Erwartung werden bestehen bzw. die Baseline-Fehler bleiben, beides mit Vorher/Nachher-Vergleich der Meldungen.
4. **BG37-Projekt (Kopie):** Nach Recompute ist `Joint014` im Solver-Startzustand gleich der GUI-Ansicht (ASMT-Residuen über das Skript `asmt_residuals.py`), der Solve konvergiert, und die Originale in BG43, BG47 und BG67 bleiben unverändert.
5. **BG43 allein und mit BG37 geladen:** dasselbe Ergebnis (Kontextunabhängigkeit).
6. **GUI-Abnahme durch dich:** BG37 öffnen. Der Rotor dreht sich, die Gewindestange ist koaxial. BG43 danach separat öffnen: unverändert.
7. Plan nach `plans/` im Repo kopieren, Umsetzungsstand nachtragen. Commit nach Rückfrage, Patch nach PATCHES.txt-Workflow.

## Umsetzungsstand (2026-10-03, noch nicht committet)

**Umgesetzt:** Schritte 1 bis 4 und die Altschaden-Liste (6). Schritt 5 ist bewusst offen, siehe unten.

- **`IdentityGraph::instanceOf()`:** Schneller Weg über die Container-Kette und `objLinkMap`, auch bei Duplikaten, mit vollem Index nur noch als Rückfall (`duplicateLeaf`). `materializeForObjectPartMap()` und `resolveForUi()` liefern die Instanz.
- **`setNewPlacements()`:** Ohne `syncLocalMirrorPlacement()` und ohne Spiegel-Nachführung.
- **`AssemblyLink`:** Spiegel **flexibler** Container folgen immer ihrer Quelle. Rigide AssemblyLinks bleiben ausgenommen, denn sie sind Körper. An BG22 gefunden: Ein rigider Halter wurde sonst um 34 mm zurückgesetzt.
- **Ziehen als Ganzes:** Nur noch für AssemblyLinks, die direkt in der aktiven BG liegen (`getMovingPartFromSel()`, `canDragObjectIn3d()`, `collectMovableObjects()`).

**Verifikation:**

| Prüfung | Ergebnis |
|---|---|
| Neuer Test `test_originals_untouched` | 12/12 bestanden (vorher 31 veränderte Originale in 10 Fixturen) |
| `test_identitygraph_handle_consistency` | 17/17 |
| `test_nested_grounding_context_isolation` | 11/11 |
| BG43 allein | Solve OK, keine Originale verändert, Joints erfüllt, GUI = Solver |
| BG22 (45 Dokumente) | Solve 3× OK, 0,16 s (vorher 0,25 s), Recompute 151 s (vorher 148 s), Joints erfüllt, GUI = Solver |
| BG37 | Originale unverändert, GUI = Solver; Solve scheitert am Altschaden `Joint014` (59 mm / 86°, gegen die verschobene Ansicht angelegt) |

**Volle Matrix:** Vier vorher bestandene doppelt verschachtelte Tests scheitern jetzt. Grund ist ausschließlich die bekannte 180°-Mehrdeutigkeit (#32477) am äußeren und mittleren Joint, statt bisher am innersten. Der Solver startet jetzt wirklich von der absichtlich falschen Startlage des Spiegels in den Fixturen. Dazu kommen die `check_sync()`-Prüfungen „Original = Spiegel“, die der neuen Regel widersprechen. Gelenkspezifische Prüfungen (Ursprung, Achse) sind überall exakt erfüllt.

**Altschäden (nur gelistet, nichts geändert):**
- **Origin verdreht:** BG37 (50,6°), BG43 (8,4°, 3,2 mm), BG47 (197,6°).
- **Container-Spiegel abweichend:** in BG37 `067_A_NEMA17x33_III` (131 mm / 142,7°) und `047_A_MotorBuchseBaugruppe` (69 mm / 142,7°). Mit dem Umbau werden sie beim Recompute korrigiert.
- **`Joint014` in BG37:** gegen die verschobene Ansicht angelegt, muss neu ausgerichtet werden.
- **Schon vor dem Umbau:** Beim Recompute von BG22 ändert sich `011_A_Rahmen#015_A_Winkel006`. Vermutlich ist das der eigene Solve von BG11 im Zuge des Abhängigkeits-Recomputes, also im eigenen Kontext.

**Offen und zu entscheiden:**
1. Testerwartungen anpassen (siehe oben).
2. Schritt 5: `solveHiddenFlexibleUnderRigid()` schreibt weiterhin Originale (BG43 hinter rigider BG37 in BG22).

## Nachtrag: Entscheidungen umgesetzt (2026-10-03)

- **Tests („Beides anpassen“):**
  - `check_sync()`-Prüfungen sind ersetzt durch `ntu.remember_originals()`/`ntu.originals_unchanged()`.
  - Die analytischen Fixed-Prüfungen nutzen `ntu.fixed_matches()`: exakt, oder genau 180° mit exaktem Gelenkpunkt (bekannte Mehrdeutigkeit #32477, an jedem Joint gleich bewertet und im Log ausgewiesen).
  - Volle Matrix: 20/20. Die früheren 6 Baseline-Fehler bestehen damit wegen der einheitlichen 180°-Bewertung, nicht wegen eines Solver-Fortschritts.
- **Rigide BG („zeigt ja, ändert nein“):**
  - `solveHiddenFlexibleUnderRigid()` und `propagateSolvedPlacementsTo()` sind entfernt. Eine rigide Unterbaugruppe zeigt den Stand ihres eigenen Dokuments und wird von außen nie gelöst.
  - `setNewPlacements()` schreibt erst ab einer Änderung von 1e-9. Beim Laden von BG22 wird dadurch kein Objekt in einem Unterdokument mehr verändert (vorher 1,2e-12 mm Rauschen an `011#Winkel006`).
- **Projekt:**
  - BG22: 3× gelöst, Solve 0,047 s (vorher 0,25 s), Recompute 146 s (vorher 148 s), 0 Originale verändert, Joints erfüllt, GUI = Solver.
  - BG43: einwandfrei.
  - BG37: nur noch der Altschaden `Joint014`.

## Übergabe für die nächste Session (2026-10-03, spätabends)

**Offen und dringend: BG22 hängt in der GUI (Recompute-Schleife).**
- **Beobachtung:** BG22 in der GUI öffnen. FreeCAD läuft mit etwa 70 % CPU weiter, alle 10–15 s ein weiterer BG37-Solve, ohne Ende. Log: `/home/maxx/freecad-sandbox/logs/freecad-sandbox-20261003-230648.log` (u.a. „Redundanzen gemeldet und noch Bewegung (79.3), loese erneut“).
- **Erster Verdacht:** Der Solve von BG37 (als abhängiges Dokument) konvergiert wegen des kaputten `Joint014` nicht, schreibt aber trotzdem Bewegung zurück. Das stößt den nächsten Recompute an.
- **Neu:** Laut Nutzer hängt es **auch nachdem BG43 aus BG37 gelöscht wurde** (BG37 um 23:13 ohne AssemblyLinks und ohne `Joint014` gespeichert). Der erste Verdacht reicht also nicht als Erklärung. Zuerst klären:
  - Welche Dokumente und Solves laufen jetzt in der Schleife? Neues Log anschauen.
  - Gibt es die Schleife auch auf dem Stand vor dem Umbau? Gegentest mit Commit `3f2f82a16c` oder früher, im Wechsel mit dem aktuellen Stand.
  - Ist die headless gemessene Recompute-Dauer von etwa 146 s bei BG22 (auch vor dem Umbau schon 148 s) dieselbe Ursache, nur ohne GUI-Neuanstoß?
- **Headless nachstellen:** BG22 öffnen, dann `while any(doc.isTouched()...) : recompute` mit Zähler. Debugger-Attach auf die GUI geht nicht (`ptrace_scope=1`).
- **Geplante Absicherung:** Ein Solve, der nach den Wiederholungen immer noch Redundanzen meldet und Bewegung hat, schreibt nicht zurück. Dazu eine Warnung mit dem betroffenen Joint.

**Nutzerdaten:** In BG37 sitzt der Schlitten (zwei Linearlager, Spindelhalter, Rechteckmutter) gegenüber dem Projekt-Commit um 4,557 mm entlang der Gleitverbindung verschoben. Unklar, ob das aus der Schleife stammt. Der Nutzer prüft das.

**Altschäden** (nur gelistet): Origins von BG37, BG43 und BG47 verdreht.

## Übergabe 2026-10-04 (Pause): BG37 in BG22 per Maus ziehen

**Stand:** BG37 ist in BG22 inzwischen **flexibel** (Datei 2026-10-04 09:20, `Rigid=false`). Es ist per `Joint008` (Gleitverbindung, `034_B_Fuehrung_401` ↔ `074_B_LM10UU`) an BG22 angebunden. In BG37 hängt der Träger 036 per Fixed an den LM10UU-Lagern.

**Erledigt (committet):**
- Der Ziehkandidat aus dem Mausklick wird auf die Instanz in BG22 abgebildet (`instanceForDrag()`, `ViewProviderAssembly.cpp`). Vorher lieferte die GUI das Original im BG37-Dokument, und das wurde abgelehnt.
- `getUpstreamMovingPart()` löst Fixed-Joints mit ihrem Verschachtelungspfad auf (`getJointOfPartConnectingToGround(..., &nestingPrefix)`).
- Headless: `preDrag([Spiegel 036])` + `doDragStep()` lässt den Spiegel 10 mm entlang `Joint008` gleiten, das Original bleibt unverändert.

**Offen:** In der GUI reißt der Träger beim Ziehen weiterhin aus BG37 heraus. Log (`FCPROJECT-DRAG`, `/home/maxx/freecad-sandbox/logs/freecad-sandbox-20261004-101426.log`): `Ziehkandidat 022#036_B_Traeger_Z, canDrag=true` → `dragMode=0 movingJoint=<keiner>`. Das heißt, `findDragMode()` findet zwar einen Fixed-Joint, `getUpstreamMovingPart()` endet aber ohne beweglichen Joint. Nächste Schritte:
- Den Pfad mit Log-Ausgaben verfolgen: Welcher Joint wird für 036 gefunden, welches Teil auf der anderen Seite, was liefert `isJointConnectingPartToGround(Joint008, ...)` für 074?
- Verdacht: `isJointConnectingPartToGround()` bzw. `getJointsOfPart()` arbeiten noch mit Original statt Instanz, oder die Rekursion verliert die `excludeJoints`.
- Am besten `getUpstreamMovingPart()`/`findDragMode()` für einen headless Test nach Python exportieren (siehe Feedback „Drag-Teststrategie nötig“).

**Diagnose-Ausgaben:** `FCPROJECT-DRAG` (Log-Ebene) in `ViewProviderAssembly.cpp`. Sie bleiben drin, bis das gelöst ist.

## Nachtrag 2026-10-04: BG37 in BG22 per Maus ziehen erledigt (Nutzer bestätigt)

**Ursache:** `isJointConnectingPartToGround()` deaktivierte zur Prüfung alle anderen Joints des Teils über `Suppressed`. Dabei schrieb es kurzzeitig in Joints des BG37-Dokuments (Originale). Danach fragte es `isPartConnected()`, und das bestimmte die Erdung neu. Der vom Rest abgetrennte Träger bekam so die **eigene** Erdung von BG37 zurück und galt als verbunden. Jeder Joint wurde dadurch als „verbindet zur Erdung“ gemeldet, und die Suche nach der Ziehart endete beim Fixed-Joint zur Führung, also beim freien Verschieben.

**Fix:** `isPartConnectedExcluding()` übernimmt die Erdung, wie sie vor der Prüfung gilt, und blendet die übrigen Joints beim Durchlaufen nur aus. Nichts wird deaktiviert, nichts wird in fremde Dokumente geschrieben. Die Diagnosefunktionen `getJointOfPartConnectingToGround()` und `getUpstreamMovingPart()` sind aus Python aufrufbar, die `FCPROJECT-DRAG`-Ausgaben sind wieder entfernt.

**Offen (Priorität 2):** Die Latenz beim Ziehen verschachtelter BGs ist hoch.
