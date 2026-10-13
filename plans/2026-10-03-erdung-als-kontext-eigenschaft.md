# Plan: Erdung ist Kontext-Eigenschaft – verschachtelte BGs nie mehr im Fremd-Dokument „entsperren“

## Kontext

**Symptom (BG67, 2026-10-03):** In der aktiven BG67 tragen alle Joints das lila Overlay `Part_Detached`
(JointObject.py `getOverlayIcons()` → `isPartConnected(Reference1) == False`), obwohl 048 per
GroundedJoint verankert ist.

**Root Cause (headless reproduziert, verboseLog):** `AssemblyObject::syncGroundedJoints()`
(AssemblyObject.cpp ~4560–4705, „Doppel-Anker-Erkennung“, 2026-09-29) schaltet bei jedem
Solve/Recompute einer **äußeren** BG (BG43) überzählige Erdungen **verschachtelter** BGs ab, indem es
per `GroundedJoint.Proxy.setReadOnly(j, False)` das ReadOnly-Flag der Placement **im Dokument der
Unterbaugruppe** (BG67, BG47) löscht. ReadOnly ist aber genau das Signal, aus dem
`getGroundedParts()` (Zeile 2242–2250, Vanilla-Logik) jeder Ebene ihre Erdung ableitet. Folge: BG67
gilt danach auch **für sich allein** als ungeerdet (`getGroundedParts()` = nur `Origin`), bis das
Dokument neu geladen wird (`GroundedJoint.onDocumentRestored()` setzt ReadOnly wieder).

Messwerte: Direkt nach dem Öffnen von BG43 sind alle 10 BG67-Joints verbunden. Nach
`BG43.recompute()` ist es keiner mehr, und das bleibt auch nach `BG67.solve()` so. Dasselbe gilt für
BG47 (045).

**Verletzte Regeln:** [[requirement-activated-bg-independent-of-loaded-context]],
[[requirement-original-template-plc-never-changed]] (eine äußere BG darf den Zustand einer
eingebundenen BG nicht verändern).

**Zusätzlicher Defekt in derselben Logik:** `sameTargetOnly` wird pro *Cluster* statt pro Paar
ausgewertet. BG43 erdet seinen Spiegel von 048, BG67 das echte 048. Beide werden nachweislich auf
dasselbe Objekt kanonisiert (`canonicalizeForMbD(mirror048) == real048`). Weil aber BG47s 045 im
selben Cluster hängt, landet der Fall trotzdem im Zweig „verschiedene Ziele“.

**Ziel:** Ob eine Erdung wirkt, ergibt sich **rein aus dem Solve-Kontext** und wird nie in ein
Dokument geschrieben. Jede BG behält immer ihre eigene Erdung. Eine äußere BG **ignoriert** beim
Einsammeln überzählige innere Erdungen nur für ihren eigenen Solve. Das gilt allgemein für jede
Verschachtelungstiefe und Topologie, ohne Sonderfälle für bestimmte BGs.

## Prinzip (Invariante)

> Ein Dokument-Objekt wird von einer Baugruppe nur dann in seinem **Erdungszustand** verändert, wenn
> der GroundedJoint in **diesem selben Dokument** liegt und der Nutzer ihn bedient.
> `getGroundedParts()` ist eine **reine Funktion** (ohne Seiteneffekte) des aktuellen Kontexts.

Rekursive Komposition: Jede Ebene liefert über `getGroundedParts()` ihre bereits **aufgelösten,
wirksamen** Wurzeln. Die äußere Ebene löst nur Konflikte zwischen ihren **lokalen** Wurzeln und den
wirksamen Wurzeln ihrer direkten flexiblen Unter-BGs auf. Damit ist jede Tiefe automatisch abgedeckt
(BG37→BG43→BG67 genauso wie die doppelt verschachtelten Fixturen).

## Umsetzung

### 1. `getGroundedParts()` – Konfliktauflösung als reiner Filter (AssemblyObject.cpp ~2239–2370)

Der vorhandene 2026-09-04-Filter („verschachtelte Wurzel verwerfen, wenn sie von der lokalen Erdung
aus erreichbar ist“) wird zu einer **Cluster-Auflösung** verallgemeinert. Ablauf:

1. **Lokale Wurzeln** (Tiefe 0) bleiben unverändert: ReadOnly-Komponenten aus
   `getAssemblyComponents(this)` (die Spiegel-Links liegen im eigenen Dokument, ihr ReadOnly stammt
   nur von eigenen GroundedJoints, siehe Probe), deaktivierte Datums und `Origin`.
2. **Verschachtelte Wurzeln:** Für jede nicht-rigide Unter-BG `nested->getGroundedParts()` (rekursiv,
   also bereits aufgelöst) ohne `nested->Origin`, jeweils mit Markierung der Herkunfts-Unter-BG.
3. **Kanonisieren:** Alle Wurzeln laufen über `canonicalizeForMbD()`. Gleiche kanonische Identität
   bedeutet ein Eintrag; die lokale Wurzel hat Vorrang (deckt den bisherigen Fall (a) „dasselbe Teil
   doppelt geerdet“ ohne Tie-Break ab).
4. **Cluster** über `traverseAndMarkConnectedParts()` mit `getJoints()` bilden, wie bisher.
5. **Pro Cluster:**
   - Mit lokaler Wurzel: alle lokalen Wurzeln behalten (mehrere lokal geerdete Teile sind
     Nutzerabsicht, Vanilla-Verhalten), alle verschachtelten Wurzeln des Clusters verwerfen.
   - Nur verschachtelte Wurzeln aus **einer** Unter-BG: alle behalten (die Unter-BG hat sich selbst
     schon aufgelöst).
   - Nur verschachtelte Wurzeln aus **mehreren** Unter-BGs: nur die Wurzeln der ersten Unter-BG in
     `Group`-Reihenfolge behalten (deterministisch), die übrigen verwerfen. Das ist der bisherige
     Fall (b) „zwei unabhängig verankerte Strukturen + Kreuz-Joint“ für den Fall, dass keine äußere
     Erdung im Cluster liegt.
6. Die Rigid-Cluster-Propagation am Ende bleibt unverändert.

Wiederverwendet werden die vorhandenen Bausteine `canonicalizeForMbD()`,
`traverseAndMarkConnectedParts()`, `isObjInSetOfObjRefs()`, `getSubAssemblies()`/`getLinkedAssembly()`
und `getJoints()` (inkl. Drag-Cache aus Patch 16). Neu ist nur eine kleine anonyme Hilfsfunktion
`resolveGroundRootConflicts()` im selben `.cpp`, die eine Root-Liste in eine gefilterte Liste
überführt (ohne neue Member, ohne neue Properties).

### 2. `syncGroundedJoints()` – Abschalt-Block entfernen (AssemblyObject.cpp ~4545–4708)

- Den kompletten Doppel-Anker-Block entfernen (`allRoots`, `collectGroundedJointsRecursive`, Cluster,
  `setReadOnly(j, False)` per `runString`, `groundedMap.erase`). Seine Wirkung übernimmt Schritt 1
  ohne Seiteneffekt.
- `struct GroundedRootInfo` und `collectGroundedJointsRecursive()` (~Zeile 446–490) entfallen, falls
  sie danach unbenutzt sind.
- Der Erzeugungszweig („ReadOnly, aber kein Joint irgendwo → neuen GroundedJoint anlegen“) bleibt
  unverändert. `collectGroundedPartsRecursive()` bleibt dafür bestehen.
- Den Kommentar zum entfernten Lösch-Zweig (2026-10-02) auf den neuen Stand bringen: Es wird weder
  gelöscht noch entsperrt.

### 3. Sichtbarkeit für den Nutzer

Der bisherige Hinweis „schaltet redundante Erdung … ab“ wurde einmalig beim Umschalten ausgegeben.
`getGroundedParts()` ist ein Hot Path (pro Mausereignis beim Drag über `isPartConnected()`), deshalb
gehört keine Ausgabe dorthin. Stattdessen gibt `solve()` nach `fixGroundedParts()` einmal pro Solve
eine Meldung aus:
`Assembly: '<BG43>' ignoriert verschachtelte Erdung '<BG67#GroundedJoint>' (Ziel …) – im Kontext dieser Baugruppe übernimmt '<…>'.`
Die Hilfsfunktion aus Schritt 1 liefert dafür optional die verworfenen Wurzeln mit. Die Meldung geht
als `message` aus, nicht als `warning`, weil das kein Fehler ist.

### 4. Diagnosehilfe beibehalten

`isPartConnected(obj, verboseLog=False)` in AssemblyObjectPyImp.cpp und AssemblyObject.pyi (heute
eingebaut, aber nicht committet) bleibt drin, weil der neue Test sie nutzt.

### Nicht im Umfang (bewusst)

- Dass der kombinierte Solve von BG43 Placements in die echten Objekte von BG47/BG67 schreibt, bleibt
  der offene Architekturpunkt aus [[requirement-activated-bg-independent-of-loaded-context]] und wird
  hier nicht angefasst.
- `solveHiddenFlexibleUnderRigid()` → `nested->solve()` nutzt die eigene Erdung der Unter-BG. Das
  ist korrekt und bleibt so.
- Die Python-Seite (`GroundedJoint.setReadOnly`, `onDocumentRestored`) bleibt unverändert.

## Risiken / zu prüfen

- **BG37→BG43(rigid)→BG67 mit Kreuz-Fixed-Joint 048↔036** (Anlass des Patches vom 2026-09-29): Hier
  muss die äußere Erdung (036) gewinnen. Das ist nach Schritt 5 „Cluster mit lokaler Wurzel“ der Fall.
  Prüfung: Instabilität (32477-Addendum) und Motor-Kollaps dürfen nicht zurückkommen.
- **Python-Code, der Placements verschachtelter Teile per `obj.Placement = …` setzt:** ReadOnly
  bleibt dort jetzt gesetzt und kann ein Python-Setattr blockieren. Vor dem 2026-09-29-Patch war es
  ebenso. Prüfen per grep in JointObject.py/UtilsAssembly.py (Joint-Anlage/Offset-Ausrichtung im
  äußeren Kontext) und im GUI-Test „Joint zwischen BG43-Teil und BG47-Teil anlegen“.
- **Performance:** Die Cluster-Bildung ist O(#Wurzeln × Traversierung); die Wurzeln sind wenige. Beim
  Drag einmal mit BG43 messen (subjektiv, Log-Zeitstempel).

## Verifikation

1. **Build:** `cmake --build standalone-check/build --target build-and-deploy`.
2. **Probe BG43/BG67/BG47** (Skripte in der Scratchpad, Muster `probe_bg67c.py` / `probe_drift.py`):
   - nach `BG43.recompute()` + 2× `solve()`: `BG67.isPartConnected()` ist für alle 10 Joints True,
     `BG47` ebenso;
   - ReadOnly auf `BG67#048` und `BG47#045` bleibt gesetzt;
   - `BG43.isPartGrounded(real 045)` ist False (im BG43-Kontext ignoriert);
   - kein Drift über 1e-6 mm zwischen solve2 und solve3;
   - die Meldung „ignoriert verschachtelte Erdung“ erscheint, „schaltet … ab“ nicht mehr.
3. **Neuer allgemeiner Regressionstest** `standalone-check/kinematic-tests/test_nested_grounding_context_isolation/`:
   - läuft über **alle** vorhandenen `*_nested_flex`- und `*_double_nested_flex`-Fixturen (Sub mit
     eigenem geerdetem BoxA, äußere BG mit geerdetem BoxC + Joint = genau diese Topologieklasse);
   - prüft für jede Ebene nach dem äußeren Solve: Die Erdung der Unter-BG ist unverändert (ReadOnly),
     `sub.isPartConnected()` ist für alle Sub-Teile True, die äußere Ebene meldet das innere BoxA als
     nicht geerdet, und es gibt keinen Drift über zwei Solves;
   - Methodik wie [[reference-nested-fixture-methodology]]: Fixturen werden nur gelesen, nicht
     umbenannt.
4. **Volle Kinematik-Matrix** `./common/run-all-tests.sh /home/maxx/freecad-sandbox/install`: Die
   Baseline von 6 bekannten Fehlern darf sich nicht verschlechtern.
5. **BG37-Projekt** `PROJ_CNC3018/CNC3018_037_A_TraegerBaugruppe_Z.FCStd` öffnen (Kreuz-Joint-Fall): solve 3×, keine Instabilität, Motor intakt.
6. **GUI-Abnahme durch dich:** BG43 + BG67 öffnen, BG43 recomputen, BG67 aktivieren. Kein lila
   Overlay mehr, und die Drehverbindung lässt sich drehen. Konsolen-Log mitprüfen.
7. Danach nachfragen wegen Commit; nach dem Commit die Patches gemäß PATCHES.txt-Workflow
   nachziehen (`check-patches.sh`).

## Umsetzungsstand (2026-10-03)

**Umgesetzt wie geplant, mit drei Abweichungen:**

1. **Durchgängig IdentityGraph statt Einzelfunktionen** (Nutzerwunsch während der Umsetzung):
   `getGroundedParts()` baut **einen** `IdentityGraph`. Wurzeln werden über `resolveObject()`
   aufgelöst, der Zusammenhang läuft über `jointEdges()` (statt `canonicalizeForMbD()`, das pro
   Aufruf einen neuen Graphen baut, und statt `traverseAndMarkConnectedParts()`). Verglichen wird
   über `materializeForObjectPartMap()`, also den Schlüssel, unter dem der Solver einen Körper
   führt.
2. **Instanzgenaue Auflösung bei Duplikaten:** Wird eine Unterbaugruppe mehrfach eingefügt, wird
   jede verschachtelte Wurzel über die Graph-Knoten mit gleicher Vorlage und dieser AssemblyLink in
   `duplicateInstancePath` aufgelöst. Ohne das blieb im Duplikat-Fixture das echte BoxD als
   isolierte Extra-Erdung stehen.
3. **Hotpath-Schutz:** Der Graph wird nur gebaut, wenn es überhaupt verschachtelte
   Erdungskandidaten gibt. Flache Baugruppen zahlen beim Drag nichts zusätzlich.

**Nebenbefund (eigenes Thema):** `resolveJointRef()` liefert für einen Joint **innerhalb** einer
duplizierten Instanz dasselbe Teil als anderes `IdentityHandle` (Template = Spiegel statt
Quellobjekt) als ein Joint der äußeren Ebene. Beide materialisieren zum selben Solver-Körper, deshalb
ist das für den Solver folgenlos, auf Handle-Ebene ist der Graph dort aber zerteilt (sichtbar mit
`exportIdentityGraphDot()` am Fixture `test_fixed_duplicate_instance_nested`).

**Verifikation:**

- Neuer Test `test_nested_grounding_context_isolation`: alter Stand FAIL in allen 11 Fixturen, neuer
  Stand PASS 11/11.
- Volle Matrix: 12 OK, genau die 6 bekannten Baseline-Fehler (identisch zum Stand ohne Fix).
- BG43/BG67/BG47: Unterbaugruppen bleiben geerdet und vollständig verbunden. BG43 ignoriert die
  Erdung von BG47 nur im eigenen Kontext.
- Zufälliger Drift von 1,33 mm in etwa 1 von 6 Läufen bei BG43: **auch ohne Fix** in gleicher Häufigkeit
  und gleicher Höhe, also vorbestehend und nicht durch diese Änderung verursacht.
- BG37: Die aktuelle Datei bindet BG43 nicht mehr ein, der Kreuz-Joint-Fall ist damit dort nicht
  mehr prüfbar. Sie selbst löst stabil.
- ReadOnly blockiert Python-Zuweisungen nicht (headless geprüft), `preSolve()` ist also nicht
  betroffen.
