# Frage an Assembly-Solver-Kenner: Fixed-Joint erzwingt Flächen-Deckung nicht zuverlässig

**Datum:** 2026-08-30
**Betroffene Version:** FreeCAD 26.3.0 (Git, selbst kompiliert), `Libs: 26.3.0devR48316`,
Commit `37d1c8253c8c9492b1efe3da73f1ffd55043a6e4` (main, 2026-08-29)
**Repro-Datei:** [halterbaugruppe-nutenstein-repro.zip](./halterbaugruppe-nutenstein-repro.zip)
— enthält zwei Ordner `vor/` und `nach/`, jeweils komplette, eigenständige Dateisätze
(`CNC3018_023_A_Halterbaugruppe.FCStd` + alle verlinkten Teile). `vor/` ist der Stand direkt
vor dem beschriebenen Teiletausch, `nach/` direkt danach - beide Ordner jeweils für sich
öffnen (nicht mischen, sonst finden die Links die falschen Ziele).

## Beobachtung

`CNC3018_023_A_Halterbaugruppe.FCStd` enthält ein `GroundedJoint` (auf die Halterung) und
genau einen `Fixed`-Joint namens `Joint`, der eine "Nutenstein"-Klemme an der Halterung
fixiert:

```
Reference1: CNC3018_006_B_Halter001_Link . "Pocket.Face10"   (Halter-Seite)
Reference2: CNC3018_018_B_M5_Nutenstein001 . "Face5"          (Nutenstein-Seite)
Offset2: Identity (Px=Py=Pz=0, keine Drehung)
```

Bei `Offset2 = Identity` sollte ein Fixed-Joint die beiden referenzierten Flächen exakt zur
Deckung bringen (gleiche Ebene, Normalen parallel/antiparallel, Abstand 0). Nach dem Solve
(`Assembly: Solve of '...' finished successfully`, **keine** Fehlermeldung, **keine**
"redundant joint"-Warnung für diesen Joint) wurde direkt per Skript nachgemessen:

```python
shape1 = ref1_obj.getSubObject("Pocket.Face10", retType=0)  # globale Shape
shape2 = ref2_obj.getSubObject("Face5", retType=0)
dist = shape1.distToShape(shape2)[0]
angle = shape1.Surface.Axis.getAngle(shape2.Surface.Axis)
```

Ergebnis:

- **Abstand zwischen den beiden Flächen: 6,42 mm** (statt 0)
- **Winkel zwischen den Flächennormalen: exakt 90°** (statt 0° oder 180°)

Der Solver meldet also Erfolg, aber die tatsächliche Geometrie erfüllt die Fixed-Joint-
Bedingung (Flächen-Deckung bei Identity-Offset) nicht. Das wurde zuvor bereits bei einer
ANDEREN Baugruppe mit einer Kreiskanten-Referenz beobachtet (dort zusätzlich mit
Non-Determinismus zwischen mehreren Ladevorgängen derselben Datei) - hier jetzt mit
**flachen, eindeutig orientierten Flächen** reproduziert, also unabhängig von
Kreisflächen-Mehrdeutigkeit.

## Die eigentliche Frage

1. Ist "Solve finished successfully" hier ein Fehlalarm - liegt die tatsächliche numerische
   Lösung (die die 6,42mm/90°-Abweichung zeigt) an einem lokalen Minimum, das der Solver als
   konvergiert akzeptiert, obwohl die Zwangsbedingung nicht erfüllt ist?
2. Spielt der mehrstufige Sub-Element-Pfad (`"Pocket.Face10"` - referenziert eine Fläche
   relativ zu einem BESTIMMTEN Feature in der Historie, nicht zwingend die aktuelle
   Tip-Shape) hier eine Rolle - wird beim Aufbau der Zwangsbedingung eventuell eine andere
   (veraltete/History-bezogene) Fläche verwendet als die, die `getSubObject(..., retType=0)`
   zur Laufzeit zurückgibt?
3. Gibt es einen bekannten Unterschied zwischen "Fixed-Joint-Referenz zeigt auf ein
   `App::Link`-Objekt, dessen `LinkedObject` ein `PartDesign::Body` ist" gegenüber einer
   direkten Body-Referenz, der bei der Zwangsbedingungs-Aufstellung relevant sein könnte?

## Zusammenhang mit vorherigem Befund

Am selben Tag wurde bei einer separaten Baugruppe (`FuehrungsBaugruppe400`, Fixed-Joint auf
eine Kreiskante) beobachtet, dass **dieselbe Eingabedatei bei mehreren unabhängigen
Ladevorgängen unterschiedliche Ergebnisse** lieferte (einmal geometrisch korrekt, einmal
sichtbar falsch) - dort mit der Vermutung, dass eine Kreiskante keine feste
Null-Drehungs-Referenz hat. Der hier dokumentierte Fall widerlegt diese Erklärung als
alleinige Ursache: flache, eindeutig orientierte Flächen zeigen denselben Grundfehler
(Fixed-Joint erzwingt die Deckung nicht), nur diesmal konsistent statt non-deterministisch.
Beide Befunde deuten auf ein tieferliegendes Problem in der Fixed-Joint-Lösung selbst hin,
nicht (nur) auf Referenz-Mehrdeutigkeit bei rotationssymmetrischer Geometrie.

## Repro-Schritte

1. `nach/CNC3018_023_A_Halterbaugruppe.FCStd` öffnen, komplett neu berechnen.
2. Report View: kein Fehler, kein redundant-joint-Hinweis für `Joint`.
3. In der Python-Konsole:
   ```python
   doc = App.ActiveDocument
   j = doc.getObject("Joint")
   s1 = j.Reference1[0].getSubObject(j.Reference1[1][0], retType=0)
   s2 = j.Reference2[0].getSubObject(j.Reference2[1][0], retType=0)
   print(s1.distToShape(s2)[0])              # -> ~6.42 statt 0
   print(s1.Surface.Axis.getAngle(s2.Surface.Axis))  # -> ~1.5708 rad (90°) statt 0/pi
   ```
4. Zum Vergleich: dieselbe Messung in `vor/CNC3018_023_A_Halterbaugruppe.FCStd`
   (Reference1 dort: `CNC3018_006_B_Halter . "Face14"`, sonst identischer Aufbau) ergibt
   **Abstand = 6,4e-14 mm (praktisch 0) und Winkel = 0,0°** - dort erfüllt derselbe
   Fixed-Joint-Typ die Deckungsbedingung also exakt. Der einzige Unterschied zwischen
   funktionierendem und kaputtem Fall ist die Ersatzteil-Seite der Referenz (andere
   Halterung, andere Fläche/Pfad) - die Nutenstein-Seite (`Face5`) ist in beiden Fällen
   identisch.

## TEILWEISE GEKLÄRT (2026-08-31) - Update: Fix allein reicht NICHT, tieferer Solver-Bug gefunden

**Live-Test des Nutzers (nach dem Fix unten, echte GUI, echter Teiletausch): Fehler bleibt.**
Ursache dafür ist eine ZWEITE, viel grundlegendere Baustelle - siehe Abschnitt "Update 2:
Solver schreibt Ergebnis gar nicht zurück" weiter unten. Der `_rewire_joint()`-Fix ist trotzdem
ein echter, notwendiger Bugfix und bleibt bestehen - er behebt nachweislich, dass
`findPlacement()` für den Halter_V1-Pfad eine Identitäts-Placement statt der echten
Face10-Geometrie lieferte. Er ist nur nicht hinreichend für das beobachtete Symptom.

### Update 1 (2026-08-31): Referenz-Bug in PartExchangeWindow.py gefunden und gefixt

**Root Cause gefunden, geometrisch bewiesen, gefixt.** Der Fehler liegt nicht im FreeCAD-
Kern, sondern in FCProjects eigenem `PartExchangeWindow.py::_rewire_joint()` - genau dieser
Code hat `Reference1` beim Halter→Halter_V1-Teiletausch erzeugt.

**Mechanismus:**

1. `Reference1`/`Reference2` sind bei jedem Assembly-Joint eine 2-elementige Sub-Namen-Liste:
   `[ElementName, VertexHint]`. FreeCADs eigener Kommentar dazu
   (`JointObject.handleInitialSelection()`): *"We add sub_name twice because the joints
   references have element name + vertex name and in the case of initial selection, both
   are the same."* - bei einer normalen Flächenauswahl sind also **immer beide Einträge
   identisch**.
2. `_rewire_joint()` (`python/PartExchangeWindow.py:822`, vor dem Fix) baute das aber als
   `[replacement_sub or "", ""]` - der zweite Eintrag blieb **leer** statt dupliziert zu
   werden.
3. FreeCADs `UtilsAssembly.findPlacement()` behandelt einen leeren zweiten Eintrag als
   Signal "ganzes Objekt ohne Sub-Element referenziert" (Sonderfall für LCS/Datum-Referenzen)
   und liefert dafür bewusst eine **Identitäts-Placement** zurück - unabhängig davon, ob der
   erste Eintrag ("Pocket.Face10") tatsächlich eine echte, gültige Fläche adressiert. Damit
   wird die Flächen-Geometrie beim Aufbau der Joint-Zwangsbedingung komplett ignoriert.
4. Direkt am ZIP nachgewiesen: `Document.xml` der `nach/`-Datei enthält exakt
   `<Sub value="Pocket.Face10" .../><Sub value=""/>` für `Reference1` UND
   `Placement1 = Identity` (beides 1:1 die Signatur dieses Bugs) - während `Reference2`
   (`Face5`, nie über PartExchange umgehängt) korrekt zwei identische `Face5`-Einträge und
   eine echte `Placement2` hat.
5. **Geometrisch bewiesen, dass der Fix ausreicht:** `Halter_V1.Face10` liegt (bei
   identischer Objekt-Placement wie die alte `Halter`) exakt (Differenz < 1e-13mm) an
   derselben globalen Position/Normalen wie vorher `Halter.Face14` - das Ersatzteil ist ein
   echter geometrischer Drop-in, nur die neue Pocket-Feature-Historie ändert den
   Element-Namen von `Face14` auf `Pocket.Face10`.

**Fix** (`python/PartExchangeWindow.py::_rewire_joint()`): zweiten Sub-Namen jetzt dupliziert
statt leer gesetzt (`[sub, sub]` statt `[sub, ""]`), analog zu FreeCADs eigener Konvention.

**Verifiziert:**
- Live in der Sandbox-GUI (Xvfb): mit dem Fix wird `Placement1` korrekt aus der echten
  Face10-Geometrie berechnet (nicht mehr Identity), und der Joint wird vom Solver als
  `1 joint(s)` (statt vorher `0 joint(s)` - komplett ignoriert) übernommen.
- Standalone-Aufruf von `UtilsAssembly.findPlacement()`/`getObject()` mit der korrigierten
  Referenz liefert eine reale, nicht-triviale Placement.
- Geometrischer Beweis (Face10 == alte Face14-Position) zeigt: ein frischer PartExchange-Lauf
  mit dem Fix würde die Flächen wieder exakt zur Deckung bringen (die Nachmessung an der
  BEREITS gespeicherten `nach/`-Datei zeigt weiterhin die alte Abweichung, weil dort auch das
  Nutenstein-Placement nie korrekt gelöst wurde, seit "0 joint(s)" seit dem Teiletausch - kein
  Widerspruch, nur ein Artefakt des Nachtestens an einer bereits verunreinigten Datei statt
  eines frischen Austauschs).

**Kein FreeCAD-Kernbug** (für DIESEN Teilbefund). `AssemblyPatternCreator.py`/
`PatternFeatures.py` haben denselben `[name, ""]`-Mustercode für LCS-Referenzen - dort
unschädlich, weil LCS-Sub-Namen ohnehin nie das Face/Edge/Vertex-Muster in
`getElementName()`/`extract_type_and_number()` matchen und daher auch mit korrekt
dupliziertem zweiten Eintrag in denselben (dort beabsichtigten) "ganzes Objekt"-Fallback
liefen - geprüft, kein Änderungsbedarf.

### Update 2 (2026-08-31): Solver schreibt Ergebnis gar nicht zurück - der eigentliche Blocker

Live-Test des Nutzers mit dem Fix aus Update 1: Fehler bleibt. Nachgestellt an einer frischen
Kopie des ECHTEN Projekt-Dokuments (nicht mehr am ZIP-Snapshot):

1. **Schon die Baseline ist kaputt:** Halter/Face14 <-> Nutenstein/Face5 (VOR jedem
   Teiletausch, mit augenscheinlich korrekten `Placement1`/`Placement2` und
   `Offset1`/`Offset2 = Identity`) zeigt in der echten, aktuellen Projektdatei **4,94mm**
   Abstand statt ~0 - abweichend vom ZIP-Snapshot (dort noch ~0). Die echte Datei ist also
   unabhängig von PartExchange bereits "verunreinigt".
2. **Entscheidender Beweis:** Nutenstein-Placement testweise manuell um (500,500,500)mm
   verschoben, danach `assembly.touch(); assembly.recompute(True)` aufgerufen - **exakt**
   das, was die Taste "Z" (Assembly_SolveAssembly) laut `CommandSolveAssembly.py` macht.
   Ergebnis: `.Placement` des Nutenstein-Links ändert sich dabei **überhaupt nicht, kein
   einziges Bit** - der Fixed-Joint bleibt bei 849mm Abstand hängen. Weder ein direkter
   `assembly.solve()`-Aufruf noch `touch()+recompute(True)` bewegen dieses Bauteil.
3. Das ist kein PartExchange-spezifisches Problem mehr, sondern ein allgemeinerer,
   grundlegenderer Solver-Kern-Befund: der Assembly-Solver schreibt seine berechnete Lösung
   (zumindest für dieses Bauteil/diesen Joint) offenbar gar nicht in die `Placement`-
   Eigenschaft zurück, unabhängig vom aufgerufenen Solve-Pfad. Passt exakt zum bereits
   dokumentierten Befund vom 2026-08-30 ("GroundedJoint-Teil springt rum ... keine
   kanonische Referenz ... Fehler klebt dauerhaft", siehe
   `project_fcproject_solver_data_fix_and_import_component`-Memory) und zur
   `project_fcproject_assemblylink_stale_placement_bug`-Memory.

**Nächster Schritt (noch nicht begonnen):** C++-Instrumentierung von
`AssemblyObject::solve()` (Assembly-Solver-Sandbox), um zu sehen, ob/wie die von Ondsel/MbD
berechnete Position überhaupt bis zur `Placement`-Property des Parts durchdringt - das ist
jetzt der eigentliche Blocker, nicht mehr der `_rewire_joint()`-Referenzbug.

### Update 3 (2026-09-01): ROOT CAUSE gefunden - `syncGroundedJoints()` löscht das
### GroundedJoint-Objekt bei einer Race Condition während des Dokument-Ladens

C++-Instrumentierung (temporäres `Base::Console().warning()`-Logging in
`freecad-source/src/Mod/Assembly/App/AssemblyObject.cpp`, noch NICHT als `.patch` erfasst,
noch NICHT committed) an `getGroundedParts()`, `setNewPlacements()`, `getMbDData()`,
`makeMbdJointOfType()`, `AssemblyObject::execute()` und `syncGroundedJoints()` selbst. Bauen
via `cmake --build build --target Assembly AssemblyGui` in `freecad-source`, danach
`AssemblyApp.so`/`AssemblyGui.so` manuell nach `install-claude-sandbox/lib/` kopiert (NIE
`cmake --install`, das trifft `/home/maxx/freecad/install`).

**Betriebsfalle dabei (selbst hineingelaufen, für künftige Sessions wichtig):** beim ersten
Rebuild nur `AssemblyApp.so` deployt, `AssemblyGui.so` vergessen - `getJoints()`s Signatur
hatte sich zwischen den beiden .so-Dateien unterschieden ("undefined symbol"), wodurch das
komplette `Assembly::AssemblyObject` beim Laden gar nicht erst erzeugt wurde
("Cannot create object 'CNC3018_023_A_Halterbaugruppe'") - alle Messungen in diesem Zustand
waren wertlos. **Immer beide `.so`-Dateien zusammen bauen und deployen.**

**Der eigentliche Fund:**

`AssemblyObject::getGroundedParts()` erkennt ein geerdetes Teil NICHT über das Vorhandensein
eines `GroundedJoint`-Objekts, sondern rein über das `ReadOnly`-Statusflag der
`Placement`-Property des Teils (`propPlc->isReadOnly()`). Dieses Flag wird von
`GroundedJoint.onDocumentRestored()` (Python, `JointObject.py`) beim Laden neu gesetzt -
Property-Status wie `ReadOnly` werden nicht zuverlässig aus der Datei restauriert, sondern
müssen beim Laden per Proxy-Callback neu angewendet werden.

`AssemblyObject::syncGroundedJoints()` (aufgerufen ganz am Anfang jedes `solve()`) gleicht
GroundedJoint-Objekte mit diesem Flag ab:
- `isReadOnly && !hasJoint` → legt ein neues GroundedJoint an (Selbstheilung).
- `!isReadOnly && hasJoint` → **löscht das bestehende GroundedJoint-Objekt** (Annahme: der
  Nutzer hat die Sperre manuell aufgehoben, das Joint ist verwaist).

**Das Problem:** `AssemblyObject`s eigenes `onChanged(&Group)` löst beim Laden (sobald Teile
in die `Group`-Property einsortiert werden) selbst schon ein `solve()` aus
(`updateSolveStatus()`). Ob zu diesem Zeitpunkt `GroundedJoint.onDocumentRestored()` das
`ReadOnly`-Flag schon gesetzt hat, ist **nicht deterministisch** - abhängig von der
Restore-Reihenfolge/Event-Loop-Verarbeitung. Live per C++-Log nachgewiesen:
- Minimaler Testfall (nur öffnen, sofort prüfen): Flag korrekt gesetzt
  (`['ReadOnly', 'LockDynamic']`), GroundedJoint bleibt erhalten.
- Testfall mit mehreren `assembly.touch(); assembly.recompute(True)`-Aufrufen (= genau das,
  was Taste "Z" macht) direkt nach dem Laden: Flag beim ersten `getGroundedParts()`-Aufruf
  noch NICHT gesetzt (`isReadOnly=0` für Halter UND Nutenstein, `getAssemblyComponents()`
  liefert dabei sogar zunächst 0 Teile - zusätzliche Cold-Load-Verzögerung) → `solve()`
  bricht mit "no grounded part found" ab. Am Ende dieses Testlaufs zeigt Halters
  `Placement`-Property-Status nur noch `['LockDynamic']` - das `ReadOnly`-Flag ist komplett
  verschwunden, nicht nur verzögert gesetzt.

**Wirkung:** einmal in diesem Fenster gefangen, greift `syncGroundedJoints()`s Lösch-Zweig
(sobald irgendwann `isReadOnly=false` UND das GroundedJoint-Objekt noch existiert
zusammentreffen) und entfernt das GroundedJoint dauerhaft aus dem Dokument. Ohne dieses
Objekt hat `GroundedJoint.onDocumentRestored()` beim NÄCHSTEN Laden nichts mehr, dessen Flag
es setzen könnte - der Selbstheilungs-Zweig (`isReadOnly && !hasJoint` → neu anlegen) greift
nur, wenn das Flag *irgendwie* doch noch True wird, was in den Testläufen nie beobachtet
wurde. Ergebnis: **dauerhaft "kein geerdetes Teil gefunden"**, unabhängig davon, wie oft
"Solve Assembly" (Z) gedrückt wird - exakt das vom Nutzer live beobachtete, permanent
klebende Symptom, und exakt der bereits am 2026-08-30 dokumentierte Befund
("GroundedJoint-Teil springt rum ... keine kanonische Referenz ... Fehler klebt dauerhaft").

**Einordnung:** dies ist ein waschechter FreeCAD-Kernbug (Race Condition zwischen C++- und
Python-Proxy-Restore-Reihenfolge), kein FCProject-eigener Fehler und auch keine
Fortsetzung des `_rewire_joint()`-Referenzbugs aus Update 1. Beide Funde bleiben unabhängig
gültig nebeneinander bestehen.

**Noch offen / nächste Schritte:**
1. Die temporären Debug-Logs sind noch im Worktree (`freecad-source`, nicht committed) -
   entweder zu einem echten Fix ausbauen oder vor dem nächsten Nutzer-Livetest wieder
   entfernen (reines Diagnose-Logging, keine Verhaltensänderung).
2. Echter Fix-Kandidat: `syncGroundedJoints()`s Lösch-Zweig entweder ganz während/kurz-nach
   dem Restore unterdrücken (nicht nur `isRestoring()`, das deckt nur die eigentliche
   Restore-Phase ab, nicht die kurz danach folgenden automatischen Solve-Versuche), oder
   `GroundedJoint.onDocumentRestored()` zuverlässig VOR dem ersten automatischen
   `onChanged(&Group)`-getriebenen Solve laufen lassen (Reihenfolge-Garantie), oder
   `syncGroundedJoints()`s Lösch-Kriterium um eine Bestätigung erweitern (z.B. nur löschen,
   wenn der Zustand über mehrere Solve-Zyklen hinweg stabil bleibt, nicht beim allerersten
   Versuch).
3. Noch nicht geprüft: ob/wie zuverlässig sich dieses Muster in der ECHTEN interaktiven GUI
   (nicht nur Xvfb-Skript) reproduzieren lässt, und ob es alle betroffenen Nutzer-Dateien
   erklärt oder nur einen Teil der historischen Solver-Bugreports.
