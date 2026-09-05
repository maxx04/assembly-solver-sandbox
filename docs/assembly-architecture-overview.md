# Assembly-Modul: Architektur-Überblick (Vorbereitung für einen echten Fix von Issue #32171)

Reine Recherche-/Dokumentationsarbeit, keine Code-Änderung. Ziel: ein verlässlicher Überblick
über die Architektur von `src/Mod/Assembly`, bevor in einer zukünftigen Sitzung versucht wird,
die Root Cause des "Nested-Flex-Joint-Detach"-Bugs
(https://github.com/FreeCAD/FreeCAD/issues/32171, lokal gespiegelt in
`patches/bugreport-nested-flex-joint-detach/README.md`) tatsächlich zu reparieren.

Kurzfassung der bereits gefundenen Root Cause (siehe verlinkter Bugreport für die volle
Herleitung, hier nur als Gedächtnisstütze):

1. `AssemblyLink::synchronizeJoints()` spiegelt beim Verschachteln Joints einer eingebetteten
   Baugruppe in die Elternbaugruppe über `assembly->getJoints(false, false)` - der zweite
   Parameter (`subJoints`) ist hier hart auf `false` gesetzt, Joints aus Enkel-Baugruppen (zwei
   oder mehr Ebenen tief) werden dadurch nie mit hochgespiegelt.
2. Mit `subJoints=true` erscheint der tiefe Joint zwar eine Ebene höher, wird aber von
   `AssemblyObject::isMbDJointValid()` als "selbstreferenzierend" verworfen, weil
   `getMovingPartFromRef()` (`AssemblyUtils.cpp`) nur das Top-Level-Objekt einer
   `PropertyXLinkSub`-Referenz liefert, nie deren Sub-Element-Pfad - beide Enden des tiefen
   Joints landen beim `findLocalAncestor()`-Fallback auf demselben Zwischen-Wrapper-Objekt.
3. Die dahinterliegende `objectPartMap`/`getMbDData()`-Struktur in `AssemblyObject.cpp` ordnet
   Teile rein über `App::DocumentObject*`-Pointer einem starren MbD-Körper zu - ganz ohne
   Sub-Pfad-Bewusstsein. Ein echter Fix bräuchte diese Struktur sub-pfad-bewusst (Schlüssel
   `(Objekt, Sub-Pfad)` statt nur `Objekt`).

Alle drei Punkte wurden beim Schreiben dieses Überblicks im aktuellen Code (Stand des lokal
gebauten `freecad-source`, inkl. aller eigenen Patches) noch einmal gegengelesen und bestätigt
gefunden - Belegstellen unten.

## 1. Hauptakteure

### 1.1 `Assembly::AssemblyObject` (`App/AssemblyObject.{h,cpp}`)

Die eigentliche Baugruppe - eine `App::Part`-Unterklasse, die zusätzlich als Fassade zum
Ondsel/MbD-Solver dient. Eine Instanz pro `Assembly::AssemblyObject`-Dokumentobjekt, d.h. **auch
jede verschachtelte Unterbaugruppe hat ihre eigene, komplett unabhängige `AssemblyObject`-Instanz**
mit eigenem `mbdAssembly`, eigener `objectPartMap` usw. - das ist der Kern des gesamten
Nested-Flex-Problems: es gibt keine gemeinsame, geteilte Solver-Welt über Verschachtelungsebenen
hinweg.

Wichtigste Zuständigkeiten/Methoden:

- `solve(bool enableRedo)` (Zeile 211): der eigentliche Solve-Lauf - `ensureIdentityPlacements()`
  → `syncGroundedJoints()` → `makeMbdAssembly()` (frisches `mbdAssembly`, `objectPartMap.clear()`)
  → `rebuildRigidClusters()`/`syncActiveRigidGroupPlacements()` → `fixGroundedParts()` →
  `getJoints(false, true, true)` → `removeUnconnectedJoints()` → `jointParts()` →
  `mbdAssembly->runPreDrag()` → `setNewPlacements()` → `redrawJointPlacements()`.
- `preDrag()`/`doDragStep()`/`postDrag()` (Zeilen 487, 532, 646): der interaktive Zieh-Pfad -
  siehe Abschnitt 2.5, entscheidend für den Bug.
- `getJoints()` (Zeile 1083): sammelt alle aktiven Joints dieser Baugruppe; mit `subJoints=true`
  (Default) rekursiv auch aus direkten `AssemblyLink`-Kindern (`getSubAssemblies()`, Zeile 2620) -
  aber nur, weil jede Sub-Assembly ihrerseits wieder mit Default-Parametern aufgerufen wird, siehe
  Abschnitt 3.
- `getMbDPart()`/`getMbDData()`/`objectPartMap` (Zeilen 129, 139, 278 in der `.h`; Implementierung
  ab Zeile 2408 in der `.cpp`): die Zuordnung "FreeCAD-Objekt → starrer MbD-Solver-Körper", inkl.
  Rigid-Group-Bündelung und (nur während `preDrag`/`bundleFixed=true`) automatischer Bündelung über
  Fixed-Joints. **Das ist die Struktur, die laut Root-Cause-Analyse sub-pfad-bewusst gemacht werden
  müsste** - Details in Abschnitt 3.
- `isMbDJointValid()` (Zeile 2378): der Selbstreferenz-Wächter, der einen Joint verwirft, wenn
  beide Enden auf denselben MbD-Körper abbilden - aktuell der sichtbare Symptomträger des Bugs.
- `rebuildRigidClusters()`/`getRigidRepresentative()`/`getRigidMembers()`/
  `syncActiveRigidGroupPlacements()`/`updateRigidPlacementCache()` (Zeilen 721-921): das
  RigidGroup-Feature (Union-Find über `RigidGroupJoint.ObjectsToRigidGroup`) - **echter
  FreeCAD-Upstream-Code** (siehe Abschnitt 4), keine eigene Arbeit.
- `getGroundedParts()`/`fixGroundedParts()`/`syncGroundedJoints()` (Zeilen 1320, 1380, 2669):
  Erdungs-Logik - ein Teil gilt als geerdet, wenn seine `Placement`-Property `ReadOnly` ist (nicht
  nur wenn ein `GroundedJoint` existiert); `syncGroundedJoints()` legt/löscht `GroundedJoint`-Objekte
  automatisch passend zu diesem ReadOnly-Zustand.
- `getConnectedParts()`/`traverseAndMarkConnectedParts()`/`removeUnconnectedJoints()`/
  `isPartConnected()` (Zeilen 1560-1673): Graph-Erreichbarkeit ausgehend von geerdeten Teilen -
  filtert Joints heraus, die nicht über eine Kette zu einem geerdeten Teil führen.

### 1.2 `Assembly::AssemblyLink` (`App/AssemblyLink.{h,cpp}`)

Der "Container", über den eine Baugruppe eine andere (Unter-)Baugruppe einbindet - ebenfalls eine
`App::Part`-Unterklasse, aber **kein** `AssemblyObject`: sie hat keinen eigenen Solver, sondern
zeigt über `LinkedObject` (ein `App::PropertyXLink`) entweder direkt auf ein `AssemblyObject` oder
auf eine weitere `AssemblyLink` (`getLinkedObject2(recursive)`, Zeile 1132/1154).

Zwei Betriebsmodi über die Property `Rigid` (`App::PropertyBool`):

- **Rigid=true**: die Unterbaugruppe verhält sich wie ein einziges starres Teil - Kinder werden
  1:1 gespiegelt und in `Placement` synchron gehalten (`synchronizeComponents()`), es gibt keine
  eigene `JointGroup` (`ensureNoJointGroup()`).
- **Rigid=false ("flexibel")**: die Unterbaugruppe bleibt intern beweglich - zusätzlich zu den
  Komponenten werden auch die Joints der Unterbaugruppe gespiegelt (`synchronizeJoints()`), damit
  die äußere Baugruppe sie mit-lösen kann. **Das ist der Modus, in dem der Nested-Flex-Bug
  auftritt**, und zwar erst ab zwei Ebenen von Rigid=false-Verschachtelung.

Wichtigste Methoden:

- `updateContents()` (Zeile 321): der zentrale Sync-Einstiegspunkt, aufgerufen aus `execute()`,
  `onChanged(Group)`, `onChanged(Rigid)`, `onDocumentRestored()`. Siehe Abschnitt 2.3.
- `synchronizeComponents()` (Zeile 349): gleicht die Kind-Objekte der Quell-Baugruppe (`Group`
  des gelinkten `AssemblyObject`/`AssemblyLink`) mit denen dieser `AssemblyLink` ab - legt fehlende
  `App::Link`/`Assembly::AssemblyLink`-Spiegel an, entfernt überzählige, pflegt dabei `objLinkMap`
  (Quellobjekt → lokaler Spiegel).
- `synchronizeJoints()` (Zeile 581): spiegelt Joints **positionsbasiert** (Index `i` in beiden
  Joint-Listen muss übereinstimmen) statt über eine stabile ID - ruft für `Reference1`/`Reference2`
  jeweils `handleJointReference()` auf. Nutzt `assembly->getJoints(false, false)` - **hier liegt
  Root-Cause-Punkt 1**.
- `handleJointReference()` (Zeile 971): bildet die externe Referenz (`prop1->getValue()`, ein
  direktes Kind der Quell-Baugruppe) über `objLinkMap` auf den lokalen Spiegel ab; findet sich kein
  direkter Treffer (Enkelkind-Fall), Fallback auf `findLocalAncestor()`.
- `findLocalAncestor()` (Zeile 1059, **eigener Patch**, siehe Abschnitt 4): läuft die
  Struktur-Eltern-Kette hoch, bis ein in `objLinkMap` bekannter Vorfahre gefunden wird, liefert den
  durchlaufenen Pfad als `Sub`-Präfix zurück. Genau dieser Fallback ist es, der beim
  `subJoints=true`-Experiment beide Enden eines tiefen Joints auf denselben Wrapper zusammenfallen
  ließ (Root-Cause-Punkt 2, siehe Bugreport).
- `synchronizeGroundedAndRigidJoints()`/`mapToLocalComponent()` (Zeilen 660, 932, **eigener Patch**,
  aktuell mit auskommentiertem Aufruf) - siehe Abschnitt 4.
- `updateParentJoints()` (Zeile 248): das Gegenstück beim Umschalten Rigid↔Flexibel - schreibt
  bestehende Joint-Referenzen in der **Elternbaugruppe** um (Kind→`this`+Präfix bzw. umgekehrt).

### 1.3 Joint-Proxy-Klassen (`JointObject.py`)

Python-`Proxy`-Objekte auf `App::FeaturePython`-Dokumentobjekten (das eigentliche C++-Objekt ist
generisch, die Fachlogik steckt komplett in Python).

- **`Joint`** (Zeile 183): die "normale", bewegliche Verbindung (Fixed/Revolute/Slider/Ball/...).
  Zentrale Properties: `JointType` (Enum, muss mit dem C++-`enum class JointType` in
  `AssemblyUtils.h` synchron bleiben), `Reference1`/`Reference2` (je ein `App::PropertyXLinkSub`,
  siehe 1.5), `Placement1`/`Placement2` (JCS relativ zum referenzierten Objekt), `Offset1`/`Offset2`,
  `Distance`/`Distance2`/`Angle`, diverse Limit-Properties. `setJointConnectors()` (Zeile 847)
  schreibt `Reference1`/`Reference2` aus einer Auswahl; `matchJCS()` (Zeile 928) positioniert das
  bewegliche Teil beim Erstellen/Bearbeiten eines Joints per Snap auf das Gegenstück (siehe auch
  Fix 7 in `patches/README.md` für einen bekannten Bug in genau dieser Funktion).
- **`GroundedJoint`** (Zeile 1457): kein "richtiger" Joint im MbD-Sinn - eine Property
  `ObjectToGround` (`App::PropertyLinkGlobal`, **kein** Sub-Pfad) markiert ein Teil als geerdet
  und setzt dessen `Placement` auf `ReadOnly`. Wird von `AssemblyObject::syncGroundedJoints()`
  automatisch angelegt/gelöscht, siehe 2.4.
- **`RigidGroupJoint`** (Zeile 1299, Teil des **Upstream**-RigidGroup-Features, siehe Abschnitt 4):
  `ObjectsToRigidGroup` (`App::PropertyLinkList`, ebenfalls **kein** Sub-Pfad) plus
  `RigidPlacements` (`App::PropertyPlacementList`, die beim Erstellen/Suspendieren eingefrorenen
  relativen Placements der Mitglieder zueinander, für Restore beim Reaktivieren).
- Gemeinsam: `GroundedJoint` und `RigidGroupJoint` referenzieren Ziele nur als ganze
  `App::DocumentObject*`, nie mit Sub-Element-Pfad - das ist genau der Grund, warum
  `AssemblyLink::synchronizeJoints()` (positionsbasiert, für `Reference1`/`Reference2` gedacht) sie
  gar nicht mitnehmen kann und eine eigene, zusätzliche Spiegel-Logik
  (`synchronizeGroundedAndRigidJoints()`) nötig war (siehe `patches/README.md`, "GroundedJoint/
  RigidGroupJoint verschwindet bei verschachtelter flexibler Baugruppe").

### 1.4 `App::Link` / `App::LinkGroup` (FreeCAD-Kern, `src/App/Link.{h,cpp}`)

Der allgemeine, nicht-Assembly-spezifische Link-Mechanismus, auf dem `AssemblyLink`/
`synchronizeComponents()` aufbaut: ein `App::Link`-Objekt zeigt über `LinkedObject`
(`App::PropertyLink`) auf ein beliebiges Objekt und übernimmt/überschreibt dessen `Placement`
lokal, ohne das Original zu verändern - die Grundlage für "ein Teil mehrfach in verschiedenen
Baugruppen mit unterschiedlicher Position verwenden". `isLinkGroup()`/`ElementList`/`ElementCount`
sind das Pendant für Arrays gleicher Objekte (z.B. Pattern/Vervielfältigung) - `AssemblyLink`
behandelt diese explizit gesondert (Abgleich über `ElementList`-Reihenfolge statt 1:1-Objektidentität,
siehe `synchronizeComponents()` Zeilen 419-437). `getLinkedObject(bool recurse)` löst rekursiv bis
zum tatsächlichen Ziel auf - wird von `AssemblyLink::mapToLocalComponent()` (eigener Patch) genutzt,
um verschiedene Kopien desselben PDM-Teils über Verschachtelungsebenen hinweg wiederzuerkennen.

### 1.5 `App::PropertyXLinkSub` (FreeCAD-Kern, `src/App/PropertyLinks.h`, ab Zeile 1414)

Der Referenz-Typ von `Joint.Reference1`/`Reference2`: erbt von `PropertyXLink` (das bereits
dokumentübergreifende Links erlaubt), fügt eine Liste von Sub-Element-Pfaden hinzu (`getSubValues()`
/`setSubValues()`, z.B. `"Body.Face1"`). Konzeptionell also **bereits sub-pfad-fähig** - das
Problem ist nicht die Property selbst, sondern dass der Assembly-Code beim Auflösen einer solchen
Referenz für Solver-Zwecke (`getMovingPartFromRef()`, s.u.) den Sub-Pfad-Teil ignoriert und nur
`prop->getValue()` (das Top-Level-Objekt) verwendet.

### 1.6 Freie Funktionen als Klebstoff (`AssemblyUtils.{h,cpp}`)

Kein eigener Zustand, reine Übersetzungsfunktionen zwischen "Referenz/Selektion" und "Objekt":

- `getObjFromRef()` (Zeile 522/619): löst eine `PropertyXLinkSub`-Referenz bis zum **tatsächlichen
  Sub-Feature** auf (z.B. der `PartDesign::Body` hinter einem Edge/Face) - für die JCS-Placement-
  Berechnung gedacht, nicht für Solver-Körper-Identität.
- `getMovingPartFromRef()` (Zeile 717/726): liefert **nur** `prop->getValue()` - das rohe
  Top-Level-Ziel der Referenz, ohne jede Sub-Pfad-Betrachtung. Das ist die Funktion aus
  Root-Cause-Punkt 2.
- `getMovingPartFromSel()` (Zeile 660): das Gegenstück für eine **live Nutzerauswahl** (Objekt +
  Sub-String aus der 3D-Pick-Interaktion, nicht aus einer gespeicherten Property) - läuft den
  Sub-Pfad tatsächlich Schritt für Schritt ab und **überspringt dabei bewusst flexible
  (Rigid=false) `AssemblyLink`-Zwischenstufen** (Zeilen 704-710), um bei der eigentlichen
  beweglichen Teil-Ebene anzukommen. **Bemerkenswerte Asymmetrie, bisher nicht dokumentiert**: es
  gibt im Code bereits eine funktionierende, sub-pfad-bewusste "finde das wirklich bewegliche Teil"-
  Logik - sie wird nur für live Auswahl in der Gui (`Gui/Commands.cpp:342`,
  `Gui/ViewProviderAssembly.cpp:825,893`) verwendet, nie für das Auflösen einer bereits
  gespeicherten Joint-Referenz. Ein echter Fix an `getMovingPartFromRef()` könnte plausibel densel-
  ben Gedanken (Sub-Pfad ablaufen, flexible Zwischen-Assemblies überspringen) wiederverwenden statt
  ihn neu zu erfinden.
- `getAssemblyComponents()` (Zeile 800): rekursive Ermittlung aller "wirklich beweglichen" Teile
  einer Baugruppe - steigt in flexible `AssemblyLink`s hinein, behandelt rigide als atomar. Wird u.a.
  von `getGroundedParts()` genutzt.

### 1.7 `AssemblyGui::ViewProviderAssembly` (`Gui/ViewProviderAssembly.cpp`)

Der ViewProvider zu `AssemblyObject` - **eine Instanz pro `AssemblyObject`**, genau wie beim
`AssemblyObject` selbst (1.1). Zuständig für 3D-Interaktion: Drag-Erkennung
(`canDragObjectIn3d()`), Drag-Modus-Bestimmung (`findDragMode()`), und den eigentlichen
Zieh-Ablauf `initMove()`/`mouseMove()`/`endMove()` (siehe Abschnitt 2.5).

## 2. Zentrale Abläufe

### 2.1 Neue Baugruppe/Teil einfügen (`CommandInsertLink.py`)

`TaskAssemblyInsertLink.onItemClicked()` (Zeile 358):

1. Objekttyp bestimmen: ein `Assembly::AssemblyObject` als Quelle → neues
   `Assembly::AssemblyLink`-Objekt; alles andere → normales `App::Link`
   (`self.assembly.newObject(objType, ...)`, Zeile 407-412).
2. `LinkedObject` auf das Quellobjekt setzen, `Placement` auf die Bildschirmmitte (mit einfacher
   Kollisions-/Versatz-Heuristik für Mehrfach-Einfügungen, Zeilen 430-452).
3. **Erst danach** `Rigid` setzen (Zeile 456-457) - Kommentar im Code erklärt bewusst: dadurch
   greift die Positions-Korrektur-Logik aus `AssemblyLink::onChanged(Rigid)` (Zeile 118 in
   `AssemblyLink.cpp`), die beim Umschalten auf Rigid die aktuelle Kind-Position in eine sinnvolle
   `movePlc` für den Container übersetzt, statt alles auf den Ursprung zurückzuwerfen.
4. Bei der allerersten Einfügung in eine noch ungeerdete Baugruppe: `handleFirstInsertion()`
   (Zeile 468) fragt (bzw. nutzt eine gespeicherte Präferenz) ob automatisch geerdet werden soll -
   bei einer flexiblen `AssemblyLink` wird dabei versucht, das intern bereits geerdete Teil der
   Quell-Baugruppe wiederzufinden (`srcGrounded`, Zeilen 507-533), sonst das erste greifbare Kind.
5. `accept()` (Zeile 148) baut die eigentlichen `newObject("App::Link", ...)`-Befehle als
   Makro-Text und führt sie über `Gui.doCommandSkip()` aus (damit sie im Undo-Stack als
   nachvollziehbare Skript-Zeilen landen, nicht als Rohtransaktion).

Das eigentliche `Assembly::AssemblyLink`-Objekt selbst wird also **nicht** über den generischen
`Gui.doCommand`-Pfad in 5. erzeugt (das betrifft nur normale `App::Link`s), sondern direkt in 1.
über `self.assembly.newObject(...)`.

### 2.2 Joint-Erstellung, Aufbau von Reference1/2

(`CommandCreateJoint.py`/`TaskAssemblyCreateJoint`, hier nicht im Detail gelesen, aber aus
`JointObject.py` und den bekannten Patches rekonstruierbar - siehe `patches/README.md` Fix 1-9 für
sehr detaillierte, live verifizierte Beobachtungen zu genau diesem Ablauf.)

1. Nutzer wählt zwei Referenzelemente (Kante/Fläche/Ursprungsachse) in der 3D-Ansicht oder im Baum
   aus - jede Auswahl ist ein `(Objekt, Sub-String)`-Paar, wobei das Objekt bei PDM-typischen
   Cross-Document-Referenzen ein `App::Link` in einem anderen Dokument sein kann.
2. `Joint.setJointConnectors()` (`JointObject.py` Zeile 847) schreibt daraus `Reference1`/
   `Reference2` (`App::PropertyXLinkSub.setValue(obj, [sub])`) sowie `Placement1`/`Placement2`
   (`findPlacement()`, die lokale JCS-Placement relativ zum referenzierten Objekt).
3. `execute()` (Zeile 825) validiert die Referenzen (u.a. der "Broken link"-Check aus Fix 4 in
   `patches/README.md` für den Fall einer noch nicht auflösbaren Cross-Document-Referenz direkt
   nach dem ersten Recompute).
4. Für den Solver relevant ist ab hier ausschließlich `getMovingPartFromRef()` (Abschnitt 1.6) -
   der Sub-String selbst wird nur für die JCS-Placement-Berechnung (`getObjFromRef()`) gebraucht,
   nicht für die "welcher starre Körper ist das"-Frage.

### 2.3 Verschachteln: `AssemblyLink::updateContents()`

Ausgelöst durch `execute()`, `onChanged(&Group)` (auch fremdausgelöst über `getInList()`, siehe
2.5-relevanter Reentrancy-Kommentar), `onChanged(&Rigid)`, `onDocumentRestored()`. Geschützt durch
den (eigenen, siehe Abschnitt 4) statischen `updatingContents`-Reentrancy-Guard.

```
updateContents()
 └─ synchronizeComponents()      // Kinder abgleichen, objLinkMap aufbauen
 └─ if Rigid:  ensureNoJointGroup()
    else:      synchronizeJoints()
                └─ assembly->getJoints(false, false)     // NICHT rekursiv -> Root Cause 1
                └─ pro Joint: doc->copyObject() oder vorhandenen Spiegel wiederverwenden
                └─ Properties kopieren (Distance, Offset1/2, Limits, ...)
                └─ handleJointReference(Reference1) / handleJointReference(Reference2)
                    └─ objLinkMap-Lookup (direktes Kind)
                        └─ Fallback: findLocalAncestor()   // Enkelkind -> Wrapper + Sub-Präfix
    // (synchronizeGroundedAndRigidJoints() hier auskommentiert, s. Abschnitt 4)
```

`synchronizeJoints()` gleicht **positionsbasiert** ab (Joint `i` der Quelle ↔ Joint `i` des
Spiegels) - es gibt keine stabile Identität zwischen Quell-Joint und gespiegeltem Joint außer der
Listenposition. Das ist für sich genommen fragil (Neuordnung/Löschen mittendrin verschiebt alle
nachfolgenden Indizes), aber nicht die hier untersuchte Root Cause - wird nur der Vollständigkeit
halber erwähnt, falls ein künftiger Fix in dieser Funktion ohnehin etwas umbaut.

### 2.4 Normaler Recompute / `solve()`

`AssemblyObject::execute()` (Zeile 129) ruft nach `App::Part::execute()` `solve(false)` auf, sofern
die Preference `SolveOnRecompute` aktiv ist (Standard: ja). Ablauf von `solve()` (Zeile 211, siehe
auch 1.1):

```
solve()
 ├─ ensureIdentityPlacements()      // LinkGroups müssen Identity-Placement haben
 ├─ syncGroundedJoints()            // GroundedJoint an/abgleichen mit Placement.ReadOnly
 ├─ mbdAssembly = makeMbdAssembly() // frische leere MbD-Welt
 ├─ objectPartMap.clear()
 ├─ rebuildRigidClusters()          // Union-Find über RigidGroupJoint-Mitgliedschaften
 ├─ syncActiveRigidGroupPlacements()
 ├─ groundedObjs = fixGroundedParts()   // -> leer? Abbruch (-6), sonst je 1 MbD-Fixed-Joint zum Weltursprung
 ├─ joints = getJoints(false, true, true)   // rekursiv (subJoints=true) + Verbose-Logging
 ├─ removeUnconnectedJoints(joints, groundedObjs)   // Graph-Erreichbarkeit ab geerdeten Teilen
 ├─ jointParts(joints)              // pro Joint: makeMbdJoint() -> isMbDJointValid() + getMbDData()
 ├─ mbdAssembly->runPreDrag()       // eigentlicher Ondsel/MbD-Solve
 ├─ setNewPlacements()              // objectPartMap zurück in FreeCAD-Placement-Properties schreiben
 └─ redrawJointPlacements(joints) + updateSolveStatus()
```

Wichtig: **jede** `AssemblyObject`-Instanz im Dokument (also auch jede über eine flexible
`AssemblyLink` eingebettete Unterbaugruppe) hat ihre eigene, komplett unabhängige `execute()`/
`solve()`-Kaskade - ein normaler Dokument-Recompute löst sie alle der Reihe nach aus (bestätigt im
Bugreport-Log: bei einem vollen Recompute lösen Sub → Top → GrandTop brav nacheinander). Das ist
der Grund, warum ein kompletter Recompute den Bug nicht zeigt, obwohl der zugrundeliegende
`objectPartMap`-Konstruktionsfehler (Root-Cause-Punkt 3) auch dort prinzipiell zuschlagen würde,
sobald ein Joint tatsächlich über `subJoints=true` in eine Elternbaugruppe hochgespiegelt würde.

### 2.5 Interaktives Ziehen vs. Recompute - warum nur die äußerste Baugruppe löst

Aus `AssemblyGui::ViewProviderAssembly` (Abschnitt 1.7):

- `initMove()` → `tryInitMove()` (Zeile 1040): ermittelt `dragMode`, blendet alle Joints außer dem
  gerade aktiven aus, und ruft `assemblyPart->preDrag(dragParts)` auf (Zeile 1127) - wobei
  `assemblyPart = getObject<AssemblyObject>()` (Zeile 1047) **das `AssemblyObject`-Dokumentobjekt
  ist, das zu genau diesem `ViewProviderAssembly` gehört**.
- `mouseMove()` (Zeile 468 ff.) ruft bei jedem Mausereignis `assemblyPart->doDragStep()` (Zeile
  626, oder `solve()` direkt bei aktivem Rigid-Group-Zwang, Zeile 623) auf derselben Instanz auf.
- `endMove()` (Zeile 1134) ruft `assemblyPart->postDrag()` (Zeile 1164) auf, wieder dieselbe
  Instanz.

Da es pro `AssemblyObject` genau eine `ViewProviderAssembly`-Instanz gibt, und FreeCADs
Gui-Schicht 3D-Mausereignisse während einer Drag-Operation an **den gerade aktiven/editierten**
ViewProvider weiterleitet (der über `Gui::ViewProvider::startEditing()`/`setEdit()` bestimmt wird -
siehe auch den verwandten Absturz-Fund in `patches/README.md`,
"freecad-assembly-viewprovider-null-crash.patch", der genau an dieser `setEdit()`-Stelle ansetzt),
bekommt bei einem Zieh-Vorgang **ausschließlich die aktuell aktive (typischerweise die äußerste)
Baugruppe** ihre `preDrag()`/`doDragStep()`/`postDrag()`-Aufrufe. Innere, über eine flexible
`AssemblyLink` eingebettete `AssemblyObject`-Instanzen haben zwar ihre eigene `ViewProviderAssembly`
und ihren eigenen `mbdAssembly`/`objectPartMap`-Zustand - der wird während dieses Drags aber
schlicht nie berührt, weil ihre `preDrag()`/`doDragStep()` nicht aufgerufen werden.

Das deckt sich exakt mit dem live rekonstruierten Befund im Bugreport (Log zeigt bei jedem
Zieh-Frame nur `Solving 'MinimalReproGrandTop#Assembly'`, nie die inneren Baugruppen) und erklärt,
warum ein Joint, der **innerhalb** einer zwei Ebenen tief eingebetteten Sub-Baugruppe lebt, beim
Ziehen von niemandem gehalten wird - es sei denn, er wurde (über `subJoints=true`,
Root-Cause-Punkt 1) erfolgreich in die äußerste Baugruppe hochgespiegelt, wo er dann aber an
Root-Cause-Punkt 2/3 scheitert.

Bei einem kompletten Dokument-Recompute (nicht Drag) läuft dagegen jede `AssemblyObject`-Instanz
über ihre eigene `execute()`/`solve()`-Kaskade (2.4) - deshalb hält der Slider-Joint dort formal,
das vorher durch den unbewachten Drag geschriebene falsche `Placement` wird aber nicht rückgängig
gemacht, weil kein Mechanismus ein von außen (per Drag) gesetztes `Placement` als "ungültig"
erkennt (`validateNewPlacements()`, Zeile 613, prüft nur, ob sich ein **geerdetes** Teil bewegt
hat - nicht, ob ein von einem inneren, nie aufgerufenen Solver gehaltenes Teil plausibel steht).

## 3. `objectPartMap`/`getMbDPart`/`getMbDData`/`getMovingPartFromRef` - Fundstellen-Katalog

Alle Verwendungsstellen in `AssemblyObject.cpp` (Datei implizit, nur Zeilen angegeben) plus die
beiden Definitionen in `AssemblyUtils.cpp`. Einschätzung bezieht sich auf einen Fix, der
`objectPartMap` (und alles, was von ihr abhängt) von `unordered_map<DocumentObject*, MbDPartData>`
auf einen sub-pfad-bewussten Schlüssel umstellt (z.B. `pair<DocumentObject*, std::string>` oder
einen kanonisierten `(WurzelObjekt, aufgelöster Pfad)`-Typ):

| Zeile(n) | Funktion | Nutzung von `objectPartMap`/verwandtem | Einschätzung für sub-pfad-bewussten Fix |
|---|---|---|---|
| 227 | `solve()` | `objectPartMap.clear()` | unkritisch, reine Lifecycle-Stelle |
| 409 | `generateSimulation()` | `objectPartMap.clear()` | unkritisch |
| 514-519 | `preDrag()` | iteriert alle Einträge, um `offsetPlc` eines gedraggten Teils zu finden | müsste auf neuen Schlüsseltyp umgestellt werden; Logik selbst (linearer Scan) bleibt gleich |
| 543, 555-557 | `doDragStep()` | `getMbDPart(part)`, Lookup von `offsetPlc` für Positions-Korrektur | direkt abhängig von der Schlüssel-Auflösung für `part` - **muss** wissen, mit welchem Sub-Pfad `part` gemeint ist, sonst bleibt das ursprüngliche Symptom (falsches/kollabiertes Teil) bestehen |
| 622-623 | `validateNewPlacements()` | Lookup von `mbdPart`/`offsetPlc` für ein geerdetes Teil | gleiches Muster, unkritischer da nur für geerdete (Top-Level-)Teile gedacht |
| 656 | `savePlacementsForUndo()` | iteriert alle Einträge (`pair.first` als Placement-Schlüssel) | unkritisch, reine Kopie |
| 710 | `exportAsASMT()` | `objectPartMap.clear()` | unkritisch |
| 925-947 | `setNewPlacements()` | iteriert alle Einträge, schreibt `Placement` zurück | **zentral**: hier muss die neue Placement pro (Objekt, Sub-Pfad)-Eintrag korrekt auf das tatsächlich gemeinte Teil zurückgeschrieben werden - bei mehreren verschiedenen Teilen, die versehentlich denselben Schlüssel hätten, würde dies aktuell nur eines der beiden falsch/gar nicht aktualisieren |
| 2200-2237 | `handleOneSideOfJoint()` | `getMbDData(part)` für Marker-Erzeugung je Joint-Seite | **zentral** - hier entscheidet sich, an welchem MbD-Körper der Marker für eine Joint-Seite hängt; mit Sub-Pfad-Bewusstsein müsste `part` hier das tatsächliche Enkelkind sein, nicht der Wrapper |
| 2261-2330 | `getRackPinionMarkers()` | `getMbDData(part1)` analog zu `handleOneSideOfJoint` | gleiche Einschätzung, Spezialfall Rack&Pinion |
| 2333-2376 | `slidingPartIndex()` | vergleicht `getMovingPartFromRef()`-Ergebnisse zweier Joints auf Objekt-Gleichheit | müsste auf den neuen (Objekt, Sub-Pfad)-Vergleich umgestellt werden, sonst können zwei verschiedene Enkelteile fälschlich als "gleiches Teil" erkannt werden |
| **2378-2406** | **`isMbDJointValid()`** | `getMbDPart(part1) == getMbDPart(part2)` | **der unmittelbare Bug-Symptomträger** - Vergleich zweier `shared_ptr<ASMTPart>`; muss nach dem Fix tatsächlich unterschiedliche Ergebnisse für zwei verschiedene Enkelteile liefern, die zufällig über denselben Wrapper aufgelöst wurden |
| **2408-2486** | **`getMbDData()`** | Kernstück: `objectPartMap.find(part)`, RigidGroup-Bündelung (`getRigidRepresentative`/`getRigidMembers`), `bundleFixed`-Fixed-Joint-Bündelung (`addConnectedFixedParts`, rekursiv über `getJointsOfPart()`) | **die eigentliche Umbaustelle** - hier müsste der Schlüsseltyp geändert und alle Konstruktions-/Lookup-Pfade (Rigid-Cluster, Fixed-Bündelung) entsprechend nachgezogen werden; die rekursive `addConnectedFixedParts`-Lambda nutzt ebenfalls `getMovingPartFromRef()`/`getJointsOfPart()` und wäre mitbetroffen |
| 2488-2493 | `getMbDPart()` | dünner Wrapper um `getMbDData(part).part` | folgt automatisch aus obigem |

`getMovingPartFromRef()` selbst (`AssemblyUtils.cpp` Zeilen 717 und 726) ist keine
`objectPartMap`-Konsumentin, sondern deren **Eingabe** - jede der folgenden Stellen ruft sie auf,
um überhaupt erst das `part1`/`part2`-Objekt zu bekommen, das dann bei `objectPartMap`
nachgeschlagen wird. Ein Fix muss zwingend **hier** ansetzen (Rückgabetyp von `(DocumentObject*)`
auf etwas wie `(DocumentObject* wurzel, std::string subPfad)` erweitern), sonst bleibt
`objectPartMap` weiterhin blind gefüttert:

| Zeile(n) | Funktion | Rolle |
|---|---|---|
| 1027-1028 | `getJointOfPartConnectingToGround()` | Identitätsvergleich `part == part1`/`part2` gegen ein übergebenes Teil |
| 1127-1129 | `getJoints()` | Selbstreferenz-Vorfilter (`part1->getFullName() == part2->getFullName()`) - **zweite Stelle mit demselben Symptom wie `isMbDJointValid()`**, aktuell nur textuell durch Logging sichtbar gemacht (FCPROJECT-PATCH 16), nicht behoben |
| 1311-1312 | `getJointsOfPart()` | Identitätsvergleich gegen ein übergebenes Teil |
| 1430 | `isJointConnectingPartToGround()` | wie oben |
| 1536-1537, 1593-1594 | `removeUnconnectedJoints()`/`getConnectedParts()` | Graph-Knoten-Identität für die Erreichbarkeits-Traversierung - zwei verschiedene Enkelteile, die auf denselben Wrapper kollabieren, würden hier als **ein** Knoten behandelt, was Konnektivität vortäuschen oder verschleiern kann |
| 2200, 2261, 2340, 2348-2349 | `handleOneSideOfJoint()`/`getRackPinionMarkers()`/`slidingPartIndex()` | s.o. |
| 2383-2384 | `isMbDJointValid()` | s.o. |

**Fazit dieses Abschnitts:** es gibt keine einzelne, isolierte Stelle, die repariert werden kann -
`getMovingPartFromRef()` ist die gemeinsame Wurzel, aber praktisch jede Solver-nahe Funktion in
`AssemblyObject.cpp`, die "welches Teil ist das eigentlich" fragt, hängt direkt oder indirekt
(über `objectPartMap`) daran. Das bestätigt die bereits im Bugreport dokumentierte Einschätzung,
dass Teil (b) (nur `isMbDJointValid()` reparieren) allein nicht ausreicht.

## 4. Eigene Patches vs. Upstream-Code

Um zu vermeiden, dass ein künftiger Fix-Versuch eigene, bereits laufende Vorarbeit für
"unbekannten Upstream-Code" hält (oder umgekehrt): alle eigenen Ergänzungen sind im Quellcode mit
`FCPROJECT-PATCH`-Kommentaren markiert und lassen sich per `grep -rn "FCPROJECT-PATCH"
src/Mod/Assembly` im lokalen `freecad-source`-Checkout vollständig auflisten. Zusammengefasst:

- **`AssemblyLink.h`/`AssemblyLink.cpp`** (Patch: `freecad-assembly-grounded-joint-nested-flex.patch`,
  vormals zwei getrennte Patches, siehe `patches/README.md`):
  - `static bool updatingContents`-Reentrancy-Guard um `updateContents()` (Header-Kommentar Zeilen
    128-146, Anwendung Zeilen 321-328) - **eigen**, behebt den "loeschfehler"-Hang.
  - `findLocalAncestor()` (Header Zeilen 78-90, Implementierung Zeilen 1059-1105) plus der
    Fallback-Aufruf in `handleJointReference()` (Zeilen 996-1002) - **eigen**, behebt die
    Enkelkind-Referenz-Regression bei Rigid=False-Verschachtelung (siehe
    `bugreport-rigid-nested-joint-reference`).
  - `synchronizeGroundedAndRigidJoints()`/`mapToLocalComponent()` (Header Zeilen 91-108,
    Implementierung Zeilen 660-968) - **eigen**, **aber der Aufruf in `updateContents()` ist
    auskommentiert** (Zeile 344: `// synchronizeGroundedAndRigidJoints();`) - toter Code ohne
    Laufzeitwirkung, siehe Warnhinweis in Abschnitt "Nächste Schritte" unten. Nicht mit
    aktivem Code verwechseln.
- **`AssemblyObject.h`/`AssemblyObject.cpp`** (Patch: `freecad-assembly-jointobject.patch`, Fixes
  10/11/13/14/15/16 laut `patches/README.md`):
  - `getJointContextName()`/`joinContextNames()` (anonymer Namespace, Zeilen 156-208) - **eigen**,
    reine Diagnose-Namensauflösung.
  - Die drei `Base::Console()`-Meldungen in `solve()` (Zeilen 220, 235-239, 279-325) - **eigen**,
    reine Logging-Ergänzung, keine Verhaltensänderung.
  - `verboseLog`-Parameter an `getJoints()` (Header Zeile 169, Implementierung Zeile 1083 sowie die
    `if (verboseLog)`-Blöcke Zeilen 1114-1173) und das Logging in `removeUnconnectedJoints()`
    (Zeilen 1518-1557) - **eigen**, reine Diagnose. **Wichtig für einen künftigen Fix**: dieses
    Logging ist die einzige aktuell vorhandene "sichtbare Beweisführung", dass ein Joint durch
    Selbstreferenz herausfällt - beim Testen eines echten Fixes lohnt es sich, `solve()` einmal mit
    aktivem Logging laufen zu lassen, um zu sehen, ob der vorher verworfene Joint jetzt tatsächlich
    "UEBERNOMMEN" wird.
  - `Origin.getValue()`-Nullprüfung in `getGroundedParts()` (Zeile 1357) - **eigen**, kleiner
    Cold-Load-Fix, unabhängig vom Nested-Flex-Thema.
  - `getJointContextName()`-Verwendung in `isMbDJointValid()`s Warnmeldung (Zeile 2401) - **eigen**
    (nur die Namensauflösung in der Meldung, **nicht** die Vergleichslogik selbst - `getMbDPart(part1)
    == getMbDPart(part2)` in Zeile 2390 ist unverändertes Upstream-Verhalten).
  - **Nicht eigen, reines Upstream** (Merge "Assembly: Add RigidGroup #29605", 11. Aug 2026):
    `rebuildRigidClusters()`, `getRigidRepresentative()`, `getRigidMembers()`,
    `syncActiveRigidGroupPlacements()`, `updateRigidPlacementCache()`, `getRigidGroups()`,
    `requiresRigidSolveForMove()`, die `rigidRepByPart`/`rigidMembersByRep`/`rigidPlacementCache`-
    Member sowie `RigidGroupJoint` in `JointObject.py` - **komplett fremder Code**, trotz
    thematischer Nähe (auch hier gibt es eine Objekt-zu-Objekt-Zuordnung ohne Sub-Pfad) nicht mit
    eigener Arbeit verwechseln.
- **`JointObject.py`** (Patch: `freecad-assembly-jointobject.patch`, Fixes 1-9): 17 kleinere, in
  `patches/README.md` einzeln sehr detailliert beschriebene Fixes (Cross-Document-Crash,
  `QuantitySpinBox`-Signal-Problem, Slider-`matchJCS()`-Kollaps-Bug, `getContext()`-Namensauflösung
  etc.) - **alle eigen**, aber **thematisch nicht mit dem Nested-Flex-Bug verwandt**. Für einen
  Fix an `objectPartMap`/`getMovingPartFromRef` vermutlich irrelevant, außer evtl. `matchJCS()`
  falls ein Fix auch das Positions-Snapping beim Joint-Erstellen innerhalb verschachtelter
  Baugruppen berührt.
- **`freecad-assembly-viewprovider-null-crash.patch`**: reine Absturz-Guards
  (`UpdateSolverInformation()`, `setEdit()`, `updateTaskPanel()` in `ViewProviderAssembly.cpp`) -
  **eigen**, aber komplett orthogonal zum Nested-Flex-Thema (Nullpointer-Checks nach
  Dokument-Reload).

**Zur Vorsicht bei einer Reaktivierung von `synchronizeGroundedAndRigidJoints()`**: dieser Code hat
live einen FreeCAD-Absturz verursacht (Reentrancy-Kaskade über `Base::Interpreter().runString()`-
Aufrufe beim Löschen einer verschachtelten `AssemblyLink`, siehe `patches/README.md`). Ein Fix am
`objectPartMap`-Kern (Abschnitt 3) ist davon **unabhängig** - beide Baustellen sollten nicht in
einem Rutsch angegangen werden, um die Fehlerquelle im Fall eines neuen Absturzes eindeutig
eingrenzen zu können.

## 5. Eigene Einschätzung: Aufwand und möglicher erster Schritt

**Größenordnung eines vollständigen Fixes: groß, mehrere Tage reine Entwicklungszeit plus
mindestens ebenso viel Testzeit.** Begründung:

- Es ist kein lokaler Bugfix, sondern eine Änderung an einer Kern-Datenstruktur
  (`objectPartMap`), die von praktisch jeder Solver-nahen Funktion in `AssemblyObject.cpp`
  gelesen/geschrieben wird (siehe die Fundstellen-Tabelle in Abschnitt 3 - mindestens 10 direkt
  betroffene Funktionen, dazu alle Aufrufer von `getMovingPartFromRef()`).
- Der neue Schlüsseltyp muss mit **mehreren bestehenden Konzepten gleichzeitig** kompatibel
  bleiben, die bereits heute über einfache `DocumentObject*`-Identität arbeiten und nicht ohne
  Kollateralschaden geändert werden dürfen: RigidGroup-Bündelung (Upstream-Feature, Abschnitt 4),
  die `bundleFixed`-Fixed-Joint-Bündelung beim Dragging (`getMbDData()`s
  `addConnectedFixedParts`-Lambda), und die Graph-Erreichbarkeit
  (`getConnectedParts()`/`traverseAndMarkConnectedParts()`).
- Zusätzlich zum reinen App-seitigen Fix müsste **auch Root-Cause-Punkt 1**
  (`AssemblyLink::synchronizeJoints()`s `getJoints(false, false)`) behoben werden, sonst erreicht
  ein tiefer Joint die äußere Baugruppe gar nicht erst - vermutlich `subJoints=true` plus eine
  entsprechende Anpassung an `handleJointReference()`/`findLocalAncestor()`, damit der
  Sub-Pfad-Präfix beim Spiegeln korrekt mitgeführt wird (aktuell wird er nur für die textuelle
  `Sub`-Liste der `PropertyXLinkSub` gepflegt, nicht für eine spätere Solver-Identität).
- Der interaktive Zieh-Pfad (Abschnitt 2.5: nur die äußerste Baugruppe löst) ist eine **dritte,
  im Prinzip unabhängige** Baustelle - selbst mit sub-pfad-bewusster `objectPartMap` und
  korrekt gespiegelten tiefen Joints würde ein Zug an einem Teil in der tiefsten Ebene weiterhin
  nur die äußerste `AssemblyObject`-Instanz lösen lassen. Ob das nach den ersten beiden Fixes
  überhaupt noch ein Problem ist (weil der tiefe Joint dann ja Teil der äußeren, tatsächlich
  lösenden Baugruppe wird), oder ob es weiterhin eine separate Lücke bleibt (z.B. für
  Simulationen/`generateSimulation()`, die pro `AssemblyObject`-Instanz einzeln laufen), ist noch
  nicht untersucht.
- Jede Änderung an dieser Codezone hat in der Vergangenheit bereits zweimal reale Abstürze
  produziert (`synchronizeGroundedAndRigidJoints()`-Reentrancy, siehe Abschnitt 4) - das spricht
  für einen vorsichtigen, in kleinen Schritten mit viel Live-Testing gegen echte verschachtelte
  Baugruppen (nicht nur das synthetische Minimal-Repro) abgesicherten Vorgehen, nicht für einen
  großen Umbau in einem Rutsch.
- Als reiner FreeCAD-Upstream-Bug (im unveränderten Weekly-AppImage bestätigt, siehe Bugreport)
  betrifft ein Fix zudem eine Codezone, die aktives Upstream-Entwicklungsziel ist (RigidGroup kam
  erst am 11. Aug 2026 dazu) - ein lokaler Fix müsste bei jedem `update-and-rebuild-freecad.sh`-Lauf
  gegen etwaige neue Upstream-Änderungen an denselben Funktionen erneut angepasst werden, ähnlich
  wie es beim RigidGroup-Merge bereits mit `freecad-assembly-jointobject.patch` passiert ist.

**Möglicher kleinerer, inkrementeller erster Schritt, der schon für sich genommen Wert liefert:**

Statt sofort `objectPartMap` selbst umzubauen, zunächst **nur** Root-Cause-Punkt 1 + eine reine
**Erkennungs**-Verbesserung an Root-Cause-Punkt 2 angehen, ohne den Joint tatsächlich lösbar zu
machen:

1. `synchronizeJoints()` auf `subJoints=true` umstellen (kleiner, lokal begrenzter Eingriff in
   `AssemblyLink.cpp`), damit tiefe Joints wenigstens strukturell in der äußeren Baugruppe
   ankommen.
2. `isMbDJointValid()` (und den bereits vorhandenen, bisher rein diagnostischen Vorfilter in
   `getJoints()`, Zeilen 1127-1148) so erweitern, dass bei einer erkannten Selbstreferenz **auch
   der jeweils andere, tatsächlich beteiligte Sub-Pfad** mit ausgegeben wird (aus
   `prop->getSubValues()` der beiden `Reference`-Properties) - macht den bereits bestehenden
   Warnhinweis "conflicting or redundant constraint" für Nutzer und Entwickler erkennbar als "das
   ist eigentlich kein echter Konflikt, nur eine bekannte Solver-Einschränkung", ohne den Joint
   scharf zu schalten.

Das liefert **kein** funktionierendes Verhalten (der Joint bleibt weiterhin unwirksam), aber:

- es macht das aktuell stillschweigende Verwerfen für jeden Nutzer/Entwickler sofort sichtbar und
  eindeutig einem bekannten, dokumentierten Sonderfall zuordenbar (statt einer generischen
  "redundant constraint"-Meldung, die nach einem Modellierungsfehler aussieht),
- es ist mit dem heutigen `objectPartMap`-Schema (reine `DocumentObject*`-Schlüssel) vollständig
  kompatibel - kein Risiko für die bereits stabilen RigidGroup-/Fixed-Bündelungs-Pfade,
  entsprechend deutlich risikoärmer zu testen,
  und liefert nebenbei bereits die Diagnose-Infrastruktur (Sub-Pfad-Extraktion aus
  `PropertyXLinkSub`), die ein späterer vollständiger `objectPartMap`-Umbau ohnehin bräuchte.

Der eigentliche, funktionale Fix (sub-pfad-bewusste `objectPartMap` + entsprechend erweiterte
`getMovingPartFromRef()`) bliebe damit als klar abgegrenzter, separater zweiter Schritt bestehen -
mit dann bereits vorhandener, in der Praxis erprobter Sub-Pfad-Extraktion aus Schritt 2 als
Baustein.

## Fix-Konzept: Adressieren statt Kopieren

Nutzer-Vorgabe wörtlich: *"AssemblyLink (flexibel) versucht, interne Joints eine Ebene nach oben zu
kopieren (`synchronizeJoints()`) - genau da sitzt der ganze Bug-Komplex. Bessere und bessere
Abbildung von verschachtelten Joints: ADRESSIEREN statt KOPIEREN!"*

Reine Konzept-/Recherchearbeit, keine Code-Änderung. Alle unten zitierten Zeilennummern wurden beim
Schreiben dieses Abschnitts im lokal gebauten `freecad-source` gegengelesen.

### 0. Vorab: was genau heute kopiert wird

`AssemblyLink::synchronizeJoints()` (`AssemblyLink.cpp`, Zeile 581-645) tut wörtlich das:

```cpp
std::vector<App::DocumentObject*> assemblyJoints = assembly->getJoints(false, false);  // Zeile 591
...
auto ret = doc->copyObject({joint});   // Zeile 607 - ECHTE neue Joint-DocumentObject-Instanz
...
handleJointReference(joint, lJoint, "Reference1");   // Zeile 636 - Referenz umschreiben
handleJointReference(joint, lJoint, "Reference2");   // Zeile 637
```

Jede Verschachtelungsebene legt also ein **physisch eigenständiges** `Joint`-Dokumentobjekt an
(`doc->copyObject()`), das eigene `Reference1`/`Reference2`-Properties trägt, die (über
`handleJointReference()`/`findLocalAncestor()`) auf den jeweils *lokalen* Spiegel-Wrapper
umgeschrieben werden. Der Abgleich ist zudem **positionsbasiert** (Index `i` in
`assemblyJoints`/`assemblyLinkJoints` muss übereinstimmen, Zeile 600) statt über eine stabile
Identität. Genau diese Kopie-Kette ist es, die laut Root-Cause-Punkt 1-3 (Kurzfassung oben)
irgendwann kollidiert oder Information (den echten Sub-Pfad) verliert.

Wichtig für das Folgende: `AssemblyObject::getJoints()` hat mit `subJoints=true` (Default seit dem
bereits vorgeschlagenen kleinen ersten Schritt, s.o.) bereits einen Rekursionspfad in
`getSubAssemblies()` (Zeile 1178-1189) - aber der ruft aktuell `assembly->getJoints()` auf, wobei
`assembly` vom Typ `AssemblyLink*` ist. Das ist **nicht** die 3-Parameter-Methode von
`AssemblyObject` (Zeile 1083), sondern die separate, parameterlose `AssemblyLink::getJoints()`
(`AssemblyLink.cpp` Zeile 1181-1188), die schlicht die **bereits kopierten** Joints aus der
*eigenen* `JointGroup` der `AssemblyLink` zurückgibt. Der Rekursionspfad liest also heute
ausschließlich Kopien, nie das Original tiefer unten. Das ist die konkrete Stelle, an der
"adressieren statt kopieren" ansetzen müsste.

### 1. Konkretes Adressierungsformat

Kein neues Format nötig - FreeCAD hat für genau dieses Problem (mehrstufige Objekt-Verschachtelung)
bereits eine Konvention, und der Assembly-Code nutzt sie an einer Stelle schon korrekt:

- **`Base::Tools::splitSubName()`** (`src/Base/Tools.cpp` Zeile 345-364) zerlegt einen
  Punkt-getrennten String wie `"Part.Part001.Body.Pad.Edge1"` in eine Liste von
  Namensegmenten - dieselbe Konvention, mit der `App::PropertyXLinkSub`/`PropertyXLink` ihre
  `getSubValues()`/`setSubValues()` (`PropertyLinks.h` Zeile 946, 1360, 4738-4745 in
  `PropertyLinks.cpp`) bereits arbeiten: **ein Wurzelobjekt (`prop->getValue()`) + ein
  Punkt-getrennter Sub-String, der beliebig viele Zwischenobjekte durchläuft**, nicht nur ein
  einzelnes Geometrie-Element.
- **`getMovingPartFromSel()`** (`AssemblyUtils.cpp` Zeile 660-715) nutzt exakt dieses Format bereits
  für Verschachtelung über mehrere `AssemblyLink`-Ebenen hinweg - und zwar nicht nur lesend: der
  Aufrufer baut den String beim Abstieg selbst zusammen. `ViewProviderAssembly::collectMovableObjects()`
  (`Gui/ViewProviderAssembly.cpp` Zeile 851-893) hängt bei jeder rekursiv durchquerten
  `AssemblyLink`-Ebene ein weiteres Namenssegment an (Zeile 867:
  `std::string newSubNamePrefix = subNamePrefix + child->getNameInDocument() + "."`) - bei zwei
  verschachtelten flexiblen `AssemblyLink`s entsteht so z.B. `"AssemblyLink1.AssemblyLink2.Box2."`,
  aufgelöst relativ zu einem `selRoot`, der meist das äußerste `AssemblyObject` selbst ist.

**Vorschlag:** Eine "Joint-Adresse" ist exakt dieses bereits existierende Paar
`(App::DocumentObject* wurzel, std::string subPfad)` - **keine neue Datenstruktur**, sondern die
konsequente Fortsetzung dessen, was `Reference1`/`Reference2` als `PropertyXLinkSub` schon heute
lokal (innerhalb der Baugruppe, in der der Joint tatsächlich lebt) speichern. Der einzige
Unterschied zu heute: der Sub-Pfad-String, den eine *äußere* Baugruppe zur Auflösung braucht, ist
nicht der in `Reference1.getSubValues()[0]` gespeicherte lokale String selbst, sondern dieser lokale
String **mit einem vorangestellten Präfix aus den Namen der durchquerten `AssemblyLink`-Objekte**
(`"AssemblyLink1.AssemblyLink2." + localSub`) - dieses Präfix entsteht rein aus der
Container-Struktur (wer enthält wen), nicht aus einer neu zu erfindenden Syntax. Eine strukturierte
Liste von `(AssemblyLink, lokaler Name)`-Paaren (die in der Aufgabenstellung als Alternative genannt
wird) wäre computational äquivalent, aber unnötig - der Punkt-String ist bereits das, was
`PropertyXLinkSub` nativ speichert/exportiert/im XML persistiert (`exportSubName()`/`importSubName()`,
`PropertyLinks.cpp` Zeile 1662, 1764), eine eigene Listenstruktur müsste zusätzlich eine eigene
Serialisierung bekommen, ohne einen erkennbaren Vorteil zu bieten.

### 2. Wiederverwendung von `getMovingPartFromSel()`

Der Kern der Funktion (Zeile 672-714), Segment für Segment abgelaufen:

```cpp
auto names = Base::Tools::splitSubName(sub);
names.insert(names.begin(), obj->getNameInDocument());
bool assemblyPassed = false;
for (const auto& objName : names) {
    obj = doc->getObject(objName.c_str());
    if (!obj) continue;
    if (obj->isLink()) doc = obj->getLinkedObject()->getDocument();       // Zeile 683-685
    if (obj == assemblyObject) { assemblyPassed = true; continue; }        // Zeile 687-691
    if (!assemblyPassed) continue;
    if (obj->isDerivedFrom<App::DocumentObjectGroup>()) continue;          // Zeile 696-698
    if (obj->isLinkGroup()) continue;                                      // Zeile 700-702
    if (obj->isDerivedFrom<Assembly::AssemblyLink>()) {                    // Zeile 705-710
        const auto* pRigid = obj->getPropertyByName<App::PropertyBool>("Rigid");
        if (pRigid && !pRigid->getValue()) continue;   // <-- GENAU DAS wird gebraucht
    }
    return obj;
}
```

Die für "adressieren statt kopieren" entscheidende Zeile ist **705-710**: eine flexible
(`Rigid=false`) `AssemblyLink`-Zwischenstufe wird beim Ablaufen des Pfads einfach **übersprungen**
(`continue`) statt zurückgegeben - der Pfad läuft transparent durch sie hindurch bis zum tatsächlich
beweglichen Teil dahinter. Genau dieses Verhalten fehlt `getMovingPartFromRef()` (Zeile 717-734)
komplett: die Funktion kennt gar keinen Pfad, nur `prop->getValue()` (das Wurzelobjekt), und bricht
dort sofort ab.

**Übertragbarkeit auf gespeicherte Referenzen - drei relevante Unterschiede, keiner davon
grundsätzlich:**

1. `getMovingPartFromSel()` bekommt `obj`+`sub` als **fertigen, vollständigen Pfad ab Dokumentwurzel**
   übergeben (vom Aufrufer in der Gui zusammengebaut, s.o.). Eine gespeicherte `Reference1` kennt nur
   ihren **lokalen** Sub-String relativ zur Baugruppe, in der der Joint tatsächlich als Dokumentobjekt
   lebt - nicht das Präfix der äußeren `AssemblyLink`-Kette, die ihn ggf. gerade einbindet (ein und
   derselbe Joint könnte im Prinzip von mehreren verschiedenen äußeren Ebenen aus adressiert werden,
   je nachdem, wie tief er verschachtelt gerade ist). Das Präfix muss also **von außen mitgegeben**
   werden, nicht aus der `Reference1`-Property selbst rekonstruiert werden.
2. `getMovingPartFromSel()` nimmt `assemblyObject` (die aktuell lösende `AssemblyObject`-Instanz) als
   expliziten Parameter, um zu wissen, ab wo der Pfad überhaupt "zählt" (`assemblyPassed`-Gate,
   Zeile 675, 687-691). `getMovingPartFromRef(App::DocumentObject* joint, const char* pName)`
   (Zeile 726-734) hat aktuell **keinen** `AssemblyObject*`-Parameter - das müsste ergänzt werden
   (mechanisch unproblematisch: jeder heutige Aufrufer ist bereits eine Member-Funktion von
   `AssemblyObject`, `this` ist überall verfügbar, siehe Fundstellen-Katalog Abschnitt 3).
3. `getMovingPartFromSel()` gibt nur das **Zielobjekt** zurück, keinen aufgelösten Sub-Pfad - für die
   reine "wohin ziehen" Frage reicht das. Ein adressierungsbasierter Ersatz für
   `getMovingPartFromRef()` müsste zusätzlich den **verbleibenden, tatsächlich aufgelösten Sub-Pfad**
   mit zurückgeben (nicht nur das Objekt), weil genau dieser fehlende Sub-Pfad die Wurzel von
   Root-Cause-Punkt 2/3 ist (`objectPartMap` kennt aktuell nur Objekt-Identität). Rückgabetyp müsste
   also von `App::DocumentObject*` auf ein Paar/Struct `{DocumentObject* obj; std::string subPath;}`
   erweitert werden.

**Konkreter Ansatzpunkt für eine neue Auflösungsfunktion:** eine neue Funktion in
`AssemblyUtils.{h,cpp}`, z.B. `resolveJointReference(const AssemblyObject* solvingAssembly,
App::DocumentObject* joint, const char* pName, const std::string& nestingPrefix)`, die

- `prop = joint->getPropertyByName<PropertyXLinkSub>(pName)` liest (wie bisher),
- den vollen Adress-String `nestingPrefix + prop->getValue()->getNameInDocument() + "." +
  prop->getSubValues()[0]` bildet (Wurzel = `solvingAssembly` statt `obj` aus der Selektion),
- und dann **denselben Segment-für-Segment-Walk** wie oben (Zeile 672-714) durchläuft - idealerweise
  durch Extraktion dieser Schleife in eine gemeinsame private Hilfsfunktion, die sowohl von
  `getMovingPartFromSel()` (interaktive Selektion) als auch von der neuen
  `resolveJointReference()` (gespeicherte Referenz + Verschachtelungs-Präfix) genutzt wird, statt die
  Logik ein zweites Mal zu duplizieren.

`nestingPrefix` selbst entsteht genau dort, wo aktuell die Kopier-Rekursion sitzt: in
`AssemblyObject::getJoints()`s `subJoints`-Zweig (Zeile 1178-1189). Statt (wie heute)
`assembly->getJoints()` auf der `AssemblyLink` (= gespiegelte Kopien lesen) aufzurufen, müsste dort
**in die tatsächliche verlinkte `AssemblyObject`-Instanz hinabgestiegen** werden (`getLinkedAssembly()`,
bereits vorhanden in `AssemblyLink.cpp` Zeile 584, dort schon für den heutigen Kopiervorgang genutzt)
und bei jedem Abstieg ein weiteres `AssemblyLink`-Namenssegment an `nestingPrefix` angehängt werden -
strukturell identisch zu dem, was `collectMovableObjects()` in der Gui bereits tut (s.o.), nur einmal
zentral im App-Layer statt bei jeder interaktiven Auswahl neu.

### 3. Auswirkung auf `getJoints()`, `isMbDJointValid()`, `objectPartMap`/`getMbDData()`

**`getJoints()`** (Zeile 1083-1192): der `subJoints`-Zweig (1178-1189) ändert seine Datenquelle
grundlegend - von "lies kopierte Joint-Objekte aus der `AssemblyLink`-eigenen `JointGroup`" zu "lies
die Original-Joint-Objekte direkt aus der verlinkten `AssemblyObject`-Instanz, rekursiv, mit
mitgeführtem `nestingPrefix`". Der Rückgabetyp `std::vector<App::DocumentObject*>` reicht dafür nicht
mehr aus, weil ein und dasselbe Original-Joint-Objekt (Pointer-Identität) je nachdem, über welchen
`AssemblyLink`-Pfad man es erreicht, ein *anderes* Präfix bräuchte (relevant z.B. wenn dieselbe
Sub-Baugruppe über zwei verschiedene `AssemblyLink`-Instanzen eingebunden ist - ein durchaus
bestehender, heute schon unterstützter Fall). Realistisch müsste `getJoints()` auf einen Rückgabetyp
wie `std::vector<std::pair<App::DocumentObject*, std::string /*nestingPrefix*/>>` (oder ein kleines
`JointRef`-Struct) umgestellt werden - **eine Signaturänderung, die alle Aufrufer betrifft** (`solve()`
Zeile 284, `removeUnconnectedJoints()`, `jointParts()`, die rekursiven `getJointsOfPart()`-Aufrufe
u.a., siehe Fundstellen-Katalog Abschnitt 3).

**`isMbDJointValid()`** (Zeile 2378-2406): ruft aktuell zweimal `getMovingPartFromRef(joint, ...)`
ohne Kontext auf (Zeile 2383-2384) und vergleicht `getMbDPart(part1) == getMbDPart(part2)`
(Zeile 2390). Müsste zu `resolveJointReference(this, joint, "Reference1", nestingPrefix)` bzw.
`"Reference2"` wechseln (das `nestingPrefix` muss also bis hierhin durchgereicht werden - entweder
als zusätzlicher Parameter an `isMbDJointValid(joint, nestingPrefix)`, oder implizit über den oben
vorgeschlagenen `JointRef`-Rückgabetyp von `getJoints()`, den `jointParts()` dann Schritt für Schritt
weiterreicht) und danach **`getMbDPart()` mit dem vollen `(obj, subPath)`-Paar** aufrufen statt nur
mit `obj` - die reine Vergleichslogik selbst (`==`) bleibt unverändert, nur der verglichene
Schlüsseltyp wird reichhaltiger.

**`objectPartMap`/`getMbDData()`** (`AssemblyObject.h` Zeile 134-139, 278; `.cpp` Zeile 2408-2486):
der Schlüsseltyp der `unordered_map` müsste von `App::DocumentObject*` auf ein
`std::pair<App::DocumentObject*, std::string>` (Wurzelobjekt + aufgelöster Sub-Pfad) oder einen
eigenen kanonisierten Typ mit passendem `std::hash`-Spezialisierung wechseln - das ist exakt die
schon in Abschnitt 3/5 (oben) beschriebene, als "groß" eingeschätzte Umbaustelle. Das Adressierungs-
Konzept **ersetzt diese Umbaustelle nicht**, es liefert ihr nur einen **korrekten, verlustfreien
Eingabewert** (den vollen Sub-Pfad statt nur eines Top-Level-Objekts) - vorher lief diese Umbaustelle
ins Leere, weil `getMovingPartFromRef()` den Pfad gar nicht erst zur Verfügung hatte. Alle in der
Fundstellen-Tabelle (Abschnitt 3) als "zentral" markierten Zeilen (`handleOneSideOfJoint()`,
`getRackPinionMarkers()`, `slidingPartIndex()`, `setNewPlacements()`) bleiben in ihrer Umbaugröße
unverändert - der Unterschied ist nur, **woher** ihr `part`-Argument jetzt kommt.

**Wann eine Adresse aufgelöst werden sollte:** analog zum bestehenden Lebenszyklus von
`objectPartMap` selbst (das an denselben drei Stellen geleert wird: `solve()` Zeile 227,
`generateSimulation()` Zeile 409, `exportAsASMT()` Zeile 710 laut Fundstellen-Tabelle) - **nicht** bei
jedem einzelnen Zugriff neu (das würde bei jedem `doDragStep()`-Mausereignis erneut den kompletten
Pfad-Walk wiederholen, mit demselben CPU-Kostenproblem, das schon beim Verbose-Logging in Fix 16
beobachtet wurde, siehe Header-Kommentar Zeile 162-167 in `AssemblyObject.h`), sondern:

1. **Einmalig beim Aufbau von `getJoints()`** innerhalb von `solve()`/`preDrag()`/
   `generateSimulation()` - das ist ohnehin der einzige Ort, an dem das `nestingPrefix` durch den
   rekursiven Abstieg in `getSubAssemblies()`/`getLinkedAssembly()` überhaupt natürlich anfällt.
2. Das Ergebnis (der aufgelöste `(obj, subPath)`-Schlüssel pro Joint-Seite) wird zusammen mit dem
   Joint durch die restliche `solve()`-Kette (`jointParts()` → `makeMbdJoint()` →
   `handleOneSideOfJoint()`/`isMbDJointValid()`/`getMbDData()`) **mitgeführt statt neu aufgelöst** -
   entweder über den oben vorgeschlagenen erweiterten `getJoints()`-Rückgabetyp, oder (weniger
   invasiv, aber mit doppelter Buchführung) über einen zusätzlichen Cache
   (`std::unordered_map<std::pair<DocumentObject*, std::string /*pName*/>, ResolvedRef>`, analog
   zu `objectPartMap` als weiteres Member) - beide Varianten laufen strukturell auf dasselbe hinaus,
   der Rückgabetyp-Weg ist sauberer, der Cache-Weg ist ein kleinerer Diff.
3. **Invalidierung:** an genau denselben drei Stellen, an denen heute schon `objectPartMap.clear()`
   steht (s.o.) - eine Struktur-Änderung (Objekt hinzugefügt/gelöscht/verschoben, `AssemblyLink`
   umgeschaltet Rigid↔Flexibel) löst ohnehin einen Recompute/neuen `solve()`-Lauf aus, der diese
   Stellen durchläuft. Ein zusätzlicher, eigener Invalidierungsmechanismus wäre nur nötig, falls der
   Cache-Ansatz (Punkt 2, zweite Variante) über eine einzelne `solve()`-Ausführung hinaus leben soll -
   davon wird abgeraten, da eine kurzlebige, pro-Solve-Instanz gültige Auflösung deutlich weniger
   Fehlerpotential hat als ein langlebiger Cache mit eigener Invalidierungslogik (siehe die bereits
   dokumentierte Vorsicht bei `synchronizeGroundedAndRigidJoints()`, Abschnitt 4 - jede zusätzliche
   zustandsbehaftete Struktur in dieser Codezone hat sich bisher als Absturzrisiko erwiesen, sobald sie
   über Reentrancy-Grenzen hinweg gültig bleiben soll).

### 4. Migrationspfad

**Was sich am Speicherformat eines einzelnen Joints NICHT ändert:** `Reference1`/`Reference2`
bleiben ganz normale `App::PropertyXLinkSub`, mit demselben lokalen `(Wurzelobjekt, Sub-String)`-Wert
wie heute - ein Joint speichert weiterhin nur seine *lokale* Referenz, relativ zu der Baugruppe, in
der er als Dokumentobjekt lebt. Es gibt also **kein neues XML-Property-Format zu migrieren** und
keine Änderung an `JointObject.py`s `setJointConnectors()`.

**Was komplett entfällt:** die gesamte Kopier-Maschinerie in
`AssemblyLink::synchronizeJoints()`/`handleJointReference()`/`findLocalAncestor()`
(`AssemblyLink.cpp` Zeile 581-645, 971-1105) - eine flexible `AssemblyLink` bräuchte unter
"adressieren statt kopieren" **gar keine eigene `JointGroup`-Population mehr**, genau wie eine
*rigide* `AssemblyLink` heute schon keine eigene `JointGroup` führt (`ensureNoJointGroup()`,
`AssemblyLink.cpp` Zeile 1107ff., aufgerufen aus `updateContents()` Zeile 333 im Rigid-Zweig). Der
naheliegende Migrationsschritt ist deshalb wörtlich: **denselben `ensureNoJointGroup()`-Aufruf, der
heute nur im Rigid-Zweig von `updateContents()` steht, auch im Flexibel-Zweig aufrufen**, sobald
`synchronizeJoints()` durch die adressierungsbasierte Auflösung ersetzt ist.

**Bereits gespeicherte, kopierte Joint-Objekte in existierenden Projektdateien:** das ist der
eigentlich heikle Teil. Eine gespiegelte Kopie ist im Dokument von einem "echten" Joint aktuell
**nicht unterscheidbar** - `doc->copyObject()` (Zeile 607) erzeugt ein vollwertiges Joint-Objekt
gleichen Typs, ohne Markierung "ich bin eine Kopie von X". Drei Optionen:

1. **Nichtstun / Toleranz:** alte Kopien bleiben einfach als normale, eigenständige Joint-Objekte in
   der `AssemblyLink`-eigenen `JointGroup` liegen. Sobald `ensureNoJointGroup()` (s.o.) beim nächsten
   `updateContents()`-Lauf für diese `AssemblyLink` greift, würden sie beim Aufräumen ohnehin gelöscht
   (`ensureNoJointGroup()` entfernt per Definition alle Inhalte der `JointGroup`) - **das passiert
   also praktisch automatisch beim ersten Laden/Recompute unter neuem Code**, ohne eigenen
   Migrationscode. Voraussetzung: `ensureNoJointGroup()` muss tatsächlich robust mit "JointGroup
   enthält Joints, die gerade noch von `getJoints()`/`solve()` gebraucht werden" umgehen - dafür
   reicht die bestehende Reihenfolge (`synchronizeComponents()` → Rigid/Flexibel-Zweig → nächster
   `solve()`-Lauf danach) vermutlich aus, wäre aber ein konkreter Testfall für die Umsetzung.
2. **Aktive Erkennung einer "verwaisten" Kopie:** wäre nur nötig, falls Nutzer bereits manuell
   Properties an einer kopierten Instanz verändert haben (z.B. `Suppressed` umgeschaltet) - dann
   würde Option 1 diese Änderung stillschweigend verwerfen. Da Joint-Properties laut
   `synchronizeJoints()` (Zeile 616-633) ohnehin bei jedem Sync von der Quelle überschrieben werden
   (Kopien sind heute schon nie eigenständig editierbar, jede manuelle Änderung an einer Kopie würde
   beim nächsten `updateContents()`-Lauf zurücküberschrieben), ist dieses Risiko in der Praxis gering
   - Option 1 dürfte ausreichen.
3. Kein Bedarf für einen expliziten Versions-/Formatwechsel-Marker im Dokument, da das alte Verhalten
   (Kopien in der `AssemblyLink`-`JointGroup`) und das neue Verhalten (keine Kopien, nur Adressierung)
   sich nicht gleichzeitig um dieselben Daten "streiten" - die alte `JointGroup` wird einfach geleert,
   nicht umgeschrieben.

**Schrittweise statt Alles-oder-Nichts - mit einer wichtigen Einschränkung:** global inkrementell
(z.B. "nur neu erstellte Joints nutzen Adressierung, alte weiter kopieren") ist **nicht sinnvoll
möglich**, weil `getJoints()`s `subJoints`-Zweig (Abschnitt 3) pro `AssemblyLink`-Kind entscheiden
muss, ob er dessen `JointGroup` liest (alter Weg) oder in die verlinkte `AssemblyObject`-Instanz
hinabsteigt (neuer Weg) - **beide gleichzeitig für dieselbe `AssemblyLink` anzuwenden würde jeden
Joint doppelt liefern** (einmal als Kopie, einmal als adressiertes Original) und damit einen neuen,
selbstgemachten "redundant constraint"-Fehler erzeugen. Der Umbau ist also **pro einzelner
`AssemblyLink`-Instanz atomar** (entweder ganz alt oder ganz neu für genau dieses eine
Verschachtelungs-Objekt), aber **zwischen verschiedenen `AssemblyLink`-Instanzen im selben Dokument
unabhängig** - eine Datei mit drei verschachtelten Unterbaugruppen könnte während der Umstellung
z.B. zwei bereits migrierte und eine noch alte `AssemblyLink` enthalten, ohne dass sich das
gegenseitig stört (jede löst unabhängig über ihre eigene `AssemblyObject`-Instanz, siehe
Abschnitt 1.1). Praktisch heißt das: die Umstellung lässt sich gut hinter einem Feldkennzeichen an
der `AssemblyLink` selbst gestaffelt einführen (z.B. vorübergehend geprüft anhand des
FreeCAD-Dateiversions-Attributs beim Laden), ist aber **kein** Feature, das man Joint für Joint
einzeln umschalten könnte - die kleinste sinnvolle Migrationseinheit ist eine `AssemblyLink`.

### 5. Aufwands-/Risiko-Vergleich zum bereits dokumentierten kleinen ersten Schritt

**Größenordnung:** mindestens so groß wie der bereits in Abschnitt 5 (oben) grob geschätzte "große"
`objectPartMap`-Umbau (mehrere Tage Entwicklung + mindestens ebenso viel Testzeit) - "adressieren
statt kopieren" **ist keine dritte, zusätzliche Option neben "kleiner Schritt" und "großer
`objectPartMap`-Umbau"**, sondern **der große Umbau selbst, nur mit einem saubereren Ausgangspunkt**:
er liefert dem ohnehin nötigen `objectPartMap`-Schlüsselwechsel (Abschnitt 3, oben) den fehlenden,
korrekten Eingabewert (vollständiger Sub-Pfad statt bloßem Top-Level-Objekt) und macht zusätzlich
Root-Cause-Punkt 1 (`synchronizeJoints()`s fehlende Rekursionstiefe) durch **Streichen** der
betroffenen Kopier-Logik gegenstandslos, statt sie separat zu reparieren (`subJoints=true` allein,
wie im kleinen ersten Schritt vorgeschlagen, wäre bei "adressieren statt kopieren" gar nicht mehr
nötig - es gibt dann keine `AssemblyLink`-eigene `JointGroup` mehr, deren Rekursionstiefe man
anpassen müsste).

**Warum es trotzdem grundsätzlich sauberer/dauerhafter ist statt nur "genauso groß, anders
verteilt":**

- Es beseitigt eine **ganze Fehlerklasse ursächlich** statt sie nachträglich abzufangen: die
  positionsbasierte Index-Kopplung in `synchronizeJoints()` (Zeile 600, oben in Abschnitt 2.3 als
  "für sich genommen fragil" vermerkt), der `findLocalAncestor()`-Kollisionsfall
  (Root-Cause-Punkt 2), und - laut `MEMORY.md` bereits als eigener, vom Nutzer als "echter Bug, nicht
  nur kosmetisch" eingestufter offener Punkt dokumentiert - die **`GroundedJoint`-Vervielfachung**
  (`syncGroundedJoints()` legt für dasselbe Teil wiederholt neue `GroundedJoint`-Objekte an) sowie das
  bereits **live abgestürzte** `synchronizeGroundedAndRigidJoints()` (Abschnitt 4, oben) sind alle drei
  **Symptome derselben Grundursache "Kopieren statt Adressieren"** - auch `GroundedJoint`/
  `RigidGroupJoint` referenzieren ihre Ziele nur über nackte `App::DocumentObject*`
  (`App::PropertyLinkGlobal`/`PropertyLinkList`, Abschnitt 1.3) und bräuchten für eine korrekte
  Abbildung über `AssemblyLink`-Grenzen hinweg im Kern dieselbe Pfad-Auflösung wie `Reference1`/
  `Reference2` - ein Adressierungs-Fix an der einen Stelle würde also plausibel **alle drei
  Baustellen gemeinsam** entschärfen, statt sie einzeln und wiederholt zu flicken.
- Es reduziert langfristig **Code**, statt ihn zu vermehren: die komplette Kopier-/
  Property-Sync-Maschinerie in `synchronizeJoints()`/`handleJointReference()`/`findLocalAncestor()`
  (rund 165 Zeilen, `AssemblyLink.cpp` Zeile 581-645 + 971-1105) entfällt ersatzlos, während der
  "kleine erste Schritt" (`subJoints=true` + Diagnose) diese Maschinerie unverändert weiterleben
  lässt und der spätere `objectPartMap`-Umbau zusätzlichen Code obendrauf legen müsste.
- Es ist **weniger** anfällig für Kollisionen mit Upstream-Änderungen an genau dieser Codezone
  (RigidGroup kam laut Abschnitt 4 erst am 11. Aug 2026 dazu, ist also aktives Entwicklungsziel) -
  eine Streichung von Code braucht bei einem Merge-Konflikt typischerweise weniger Handarbeit als das
  Nachziehen einer parallel gewachsenen Kopier-Logik.

**Warum es trotzdem NICHT der empfohlene nächste Schritt ist, sondern der bereits dokumentierte
kleine erste Schritt zuerst kommen sollte:**

- Der kleine erste Schritt ist mit dem **heutigen** `objectPartMap`-Schema vollständig kompatibel
  (Abschnitt 5, oben: "kein Risiko für die bereits stabilen RigidGroup-/Fixed-Bündelungs-Pfade") -
  "adressieren statt kopieren" ist es nicht: es ändert den `objectPartMap`-Schlüsseltyp, den
  `getJoints()`-Rückgabetyp und entfernt eine seit Langem laufende, mehrfach live gehärtete
  Kopier-Pipeline in einem Zug. Jede dieser drei Änderungen einzeln ist schon nicht trivial risikofrei
  (siehe die zweifache Absturzhistorie in Abschnitt 4) - alle drei gemeinsam in einer Sitzung zu
  verifizieren ist unrealistisch.
- Der kleine erste Schritt liefert **sofort** nutzbaren diagnostischen Mehrwert (sichtbare, korrekt
  zugeordnete Warnmeldung) mit sehr geringem Testaufwand. Ein Adressierungs-Umbau liefert erst am
  Ende der gesamten Kette (`getJoints()` + `resolveJointReference()` + `objectPartMap`-Schlüsselwechsel
  + Migration) überhaupt ein sichtbares, testbares Ergebnis - es gibt keinen sinnvollen
  Zwischenzustand, den man isoliert gegen echte Baugruppen testen könnte, ohne mindestens Abschnitt 2
  und 3 gemeinsam umzusetzen.

**Sinnvoller Zwischenweg:** genau der in Abschnitt 5 (oben) bereits vorgeschlagene - **zuerst** der
kleine erste Schritt (subJoints=true + Diagnose-Erweiterung um den jeweils anderen Sub-Pfad aus
`getSubValues()`), **weil** dessen zweiter Teilschritt ("Sub-Pfad-Extraktion aus `PropertyXLinkSub`")
exakt die Bausteine liefert, die `resolveJointReference()` (Abschnitt 2, oben) ohnehin braucht -
danach, mit dieser bereits in der Praxis erprobten Extraktion als Fundament, **schrittweise** in
Richtung Adressierung: zuerst `resolveJointReference()` + gemeinsame Walk-Hilfsfunktion (Abschnitt 2)
isoliert am 3-Boxen-Minimal-Repro bauen und verifizieren, **danach erst** den
`objectPartMap`-Schlüsselwechsel (Abschnitt 3) und zuallerletzt das Entfernen der Kopier-Pipeline
samt Migration (Abschnitt 4) - in dieser Reihenfolge, weil jeder Zwischenschritt für sich weiterhin
mit der bestehenden Kopier-Pipeline koexistieren kann (die neue Auflösungsfunktion kann parallel
zur alten `getMovingPartFromRef()` existieren und an einer einzelnen, klar abgegrenzten Stelle wie
`isMbDJointValid()` zuerst nur *zusätzlich* zur Diagnose genutzt werden, bevor sie den bisherigen
Aufruf tatsächlich ersetzt) - und erst der letzte Schritt (Kopier-Pipeline entfernen) den in
Abschnitt 4 beschriebenen "atomar pro `AssemblyLink`"-Charakter hat, der keinen weiteren
Zwischenzustand erlaubt.

## Schritt 2 - Ergebnis

(2026-08-28, isoliert und rein diagnostisch gebaut/getestet, siehe Auftragsbeschreibung oben.
Patch-Datei: `patches/step2-resolve-joint-reference.patch` - **noch nicht** in die konsolidierten
Patches gemergt, bewusst separat gehalten, siehe Auftrag.)

### Was gebaut wurde

**1. `resolveJointReference()`** (`AssemblyUtils.h`/`.cpp`), Signatur wie im Konzeptdokument
(Abschnitt 2) vorgeschlagen:

```cpp
struct ResolvedJointRef {
    App::DocumentObject* obj = nullptr;
    std::string subPath;
};
AssemblyExport ResolvedJointRef resolveJointReference(
    const AssemblyObject* solvingAssembly,
    App::DocumentObject* joint,
    const char* pName,
    const std::string& nestingPrefix
);
```

Liest `Reference1`/`Reference2` (wie `getMovingPartFromRef()`), baut `nestingPrefix +
refObj->getNameInDocument() + "." + localSub`, und läuft dann - **eigenständig implementiert, ohne
`getMovingPartFromSel()` selbst anzufassen** (siehe Risiko-Abwägung unten) - Segment für Segment
über `Base::Tools::splitSubName()` durch, mit denselben drei Sprungregeln wie
`getMovingPartFromSel()` (Zeile 696-710 der Originalfunktion): Gruppen überspringen, LinkGroups
überspringen, flexible (`Rigid=false`) `AssemblyLink`-Zwischenstufen transparent überspringen.
Sobald ein Segment auf ein "echtes" Zielobjekt trifft, werden `result.obj` und - **das ist der
Mehrwert gegenüber `getMovingPartFromSel()`** - die noch nicht konsumierten Segmente als
`result.subPath` zurückgegeben. Bei jedem Fehler (nullptr, leere Referenz, ein Segment lässt sich
nicht in `solvingAssembly->getDocument()` finden) wird sauber `ResolvedJointRef{}` zurückgegeben,
niemals eine Exception/ein Crash.

**Das `assemblyPassed`-Gate wurde bewusst NICHT übernommen** - die Analyse in Abschnitt 2 (oben)
wurde beim Bauen noch einmal kritisch gegengelesen und bestätigt: das Gate existiert in
`getMovingPartFromSel()` nur, weil deren Eingabe-Pfad ab Dokumentwurzel beginnt und `assemblyObject`
selbst als Segment durchlaufen werden muss, bevor nachfolgende Segmente zählen. `nestingPrefix` wird
dagegen an der Aufrufstelle (`diagnoseAddressableJointsRecursive()`, s.u.) so konstruiert, dass der
resultierende Adress-String bereits relativ "innerhalb" von `solvingAssembly` beginnt (das erste
Segment ist bereits der Name der ersten durchquerten `AssemblyLink`) - ein zusätzliches Gate hätte
hier nichts zu tun, an dem es "passieren" könnte, und hätte im schlimmsten Fall den Walk fälschlich
komplett blockiert, wenn `solvingAssembly` selbst nie als Segment auftaucht.

**2. `diagnoseAddressableJoints(AssemblyObject* root)`** (`AssemblyUtils.h`/`.cpp`) - **bewusst als
komplett separate, freie Funktion gebaut, NICHT in `getJoints()` eingebaut** (die im Auftrag
genannte Alternative). Begründung für diese Entscheidung:

- `getJoints()` wird nicht nur von `solve()`, sondern auch von `isPartConnected()`/
  `getJointsOfPart()` aufgerufen, die während eines interaktiven Drags potenziell auf jedem
  Mausereignis laufen (siehe Fix 16/`verboseLog`-Kommentar im Header) - jede zusätzliche
  Traversierung dort, und sei sie noch so lesend, vergrößert die Angriffsfläche für ein erneutes
  CPU-Problem wie beim ersten (unbedingten) Fix-16-Logging.
- Genau diese Codezone (`AssemblyLink`-Rekursion, `getLinkedAssembly()`,
  `AssemblyObject`-Instanzgrenzen) hat bereits zweimal reale FreeCAD-Abstürze verursacht
  (`synchronizeGroundedAndRigidJoints()`, siehe `patches/README.md`) - eine komplett separate,
  niemals von `getJoints()`/`solve()` aus erreichbare Funktion hat per Konstruktion **keine**
  Möglichkeit, den bestehenden Solve-/Drag-Pfad zu beeinflussen, selbst wenn sich in ihr später ein
  Fehler fände.
- Für den in dieser Sitzung verlangten Nachweis ("beweist, dass `resolveJointReference()` mit einem
  echten, rekursiv aufgebauten `nestingPrefix` tatsächlich funktioniert") reicht eine separate
  Funktion vollständig aus - eine Integration in `getJoints()` selbst liefert keinen zusätzlichen
  Erkenntnisgewinn für Schritt 2, nur zusätzliches Risiko.

Traversierung: steigt rekursiv über `AssemblyLink::getLinkedAssembly()` in jede flexible
(`Rigid=false`) Unter-`AssemblyLink` hinab (rigide werden übersprungen - dort gibt es keine eigene
Solver-Welt), führt dabei `nestingPrefix` mit (`+= subLink->getNameInDocument() + "."`), und ruft
für jeden ORIGINAL-Joint (direkt aus `assembly->getJointGroup()->getObjects()` der jeweils
**verlinkten Instanz**, nicht aus einer `AssemblyLink`-eigenen Kopie-`JointGroup`)
`resolveJointReference()` für `Reference1`/`Reference2` auf und protokolliert das Ergebnis über
`Base::Console().message()`. Rein lesend - erzeugt/löscht keine Objekte, ändert keine Properties.
Ein Reentrancy-Guard gegen `linked == assembly` ist eingebaut (defensiv, für den Fall einer
fehlerhaften/zyklischen Verlinkung).

**3. Dünner Test-Zugang von Python aus** (`AssemblyObject::diagnoseAddressableJoints()`,
`AssemblyObject.pyi`, `AssemblyObjectPyImp.cpp`): da `diagnoseAddressableJoints()` eine reine
C++-Funktion ohne Python-Bindung ist und die Aufgabe explizites Testen über FreeCADCmd/Python
verlangt, wurde ein no-arg `AssemblyObject`-Methodenwrapper ergänzt (`this->getAssemblyObjectPtr()
->diagnoseAddressableJoints();`, welcher intern nur `Assembly::diagnoseAddressableJoints(this)`
aufruft). Gleiches Muster wie die bestehenden No-Arg-Methoden (`ensureIdentityPlacements()` etc.) in
derselben Datei. Rein additiv, keine bestehende Methode/Bindung verändert.

### Testergebnisse

Getestet ausschließlich gegen Kopien/frische synthetische Dokumente (nie gegen echte
Projektdateien), headless via `FreeCADCmd`, mit `VIRTUAL_ENV`/`PYTHONPATH` explizit deaktiviert
(`env -u VIRTUAL_ENV -u PYTHONPATH -u PYTHONHOME ...`) - das umgeht den bekannten
PySide6/`Qt_6`-ImportError beim Laden von `JointObject.py`-Proxys vollständig (er trat mit
aktiviertem `.venv` weiterhin auf, störte aber wie erwartet nur die Proxy-Neuverbindung, nicht die
hier getesteten C++-Funktionen).

**Build/Stabilität:** `Assembly`-Target baut ohne Warnungen; über gut zwei Dutzend
`recompute()`/`solve()`-Läufe (mehrere Kopien der 3-Boxen-Dateien, ein frisches synthetisches
2-Ebenen-Repro, mehrfaches `Rigid`-Umschalten, mehrfache erzwungene Recomputes) - kein einziger
Absturz, keine Exception, `solve()` verhält sich in allen Fällen exakt wie vor diesem Patch
(gleiche "computed (N joint(s), ...)"/"finished successfully"-Meldungen, gleiche
`isMbDJointValid()`-Warnungen an denselben Stellen). Das bestätigt die Kernanforderung: **keine
Verhaltensänderung**.

**1. `resolveJointReference()` korrekt für einen einfachen (nicht verschachtelten) Joint:**
Getestet an `MinimalReproTop.FCStd`s eigenem Top-Level-Joint (`Joint`, Gleitverbindung BoxC-BoxD,
`nestingPrefix=""`):

```
Assembly: diagnoseAddressableJoints - Original-Joint 'Joint' -> Reference1: MinimalReproTop#BoxC
(subPath='Edge10'), Reference2: MinimalReproTop#BoxD (subPath='Edge10')
```

Exakt das erwartete Ergebnis: beide Enden korrekt aufgelöst, jeweils mit dem passenden
Rest-Sub-Pfad (`Edge10`) - genau die Information, die `getMovingPartFromRef()` bisher wegwirft.

**2. `resolveJointReference()` korrekt defensiv bei nicht auflösbaren Referenzen:** für
`MinimalReproTop.FCStd`s `Joint002` (StarrerVerbund) lieferte die Original-Property `Reference1`
bereits selbst `None` (ein vorbestehender, von diesem Patch unabhängiger Defekt in dieser
Alt-Testdatei - siehe unten) - `resolveJointReference()` gab dafür sauber `ResolvedJointRef{}`
zurück, keine Exception:

```
Assembly: diagnoseAddressableJoints - Original-Joint 'Joint002' -> Reference1: <nicht aufgeloest>
(subPath=''), Reference2: MinimalReproTop#BoxD (subPath='Face6')
```

**3. `diagnoseAddressableJointsRecursive()`s Traversierungsmechanik selbst (der Kern von "adressieren
statt kopieren") funktioniert nachweislich über zwei Verschachtelungsebenen hinweg:** an einer
frisch und ausschließlich für diesen Test programmatisch erzeugten, synthetischen 2-Ebenen-Baugruppe
(`SynGrandTop` → `Assembly001`[flexibel] → `SynTop` → `unterAssembly`[flexibel] → `SynSub`,
gespeichert unter `/tmp/synth_scratch*`, NICHT Teil des Repos) fand die Traversierung erfolgreich
das **Original**-Joint-Objekt in `SynSub`s eigenem Dokument - nicht irgendeine Kopie - mit korrekt
über zwei Ebenen aufgebautem `nestingPrefix`:

```
Assembly: diagnoseAddressableJoints - Original-Joint 'Assembly001.unterAssembly.Joint' ->
Reference1: <nicht aufgeloest> (subPath=''), Reference2: <nicht aufgeloest> (subPath='')
```

Das beweist: `getLinkedAssembly()`-Abstieg + `nestingPrefix`-Aufbau + "lies aus der verlinkten
Instanz, nicht aus der `AssemblyLink`-eigenen Kopie-`JointGroup`" funktionieren strukturell genau
wie im Konzeptdokument beschrieben. Die Reference1/2-Auflösung selbst schlug hier NICHT wegen eines
Fehlers in `resolveJointReference()` fehl, sondern weil das Zielobjekt (`BoxA`/`BoxB`-Spiegel
innerhalb von `unterAssembly`) in diesem Testlauf gar nicht erst existierte - siehe nächster
Abschnitt, ein **separater, neu entdeckter Befund**, keine Schwäche des neuen Codes.

### Neuer Befund (nicht behoben, außerhalb des Schritt-2-Scopes): `AssemblyLink::synchronizeComponents()` erzeugt unter headless FreeCADCmd keine Spiegel-Objekte für einfache (Nicht-AssemblyLink-)Teile

Beim Versuch, Testfall 3 mit einer echten `BoxA`/`BoxB`-Auflösung zu verifizieren, wiederholt
reproduziert - sowohl an den originalen `bugreport-nested-flex-joint-detach`-Dateien (unverändert,
nur lesend geöffnet) als auch an einer komplett frischen, ausschließlich für diesen Test per Skript
erzeugten und gespeicherten 2-Ebenen-Baugruppe:

- Eine flexible `AssemblyLink` (`unterAssembly`/`unterAssambly`) zeigt nach `openDocument()` +
  `recompute()` (auch nach mehrfachem erzwungenem `recompute()`, auch nach `Rigid`-Toggle
  True→False, was `updateContents()` nachweislich erneut auslöst - das `Group`-Property ändert sich
  sichtbar) **niemals** die erwarteten `App::Link`-Spiegelobjekte für einfache Teile (`BoxA`,
  `BoxB`, ganz normale `Part::Box`) in ihrer eigenen `Group`.
- **Eingegrenzt auf genau einen von zwei Zweigen in `synchronizeComponents()`**: der Zweig, der eine
  verschachtelte `AssemblyLink` als weitere `AssemblyLink` spiegelt (`unterAssembly` selbst,
  gespiegelt als Kind von `Assembly001` in `GrandTop`), funktioniert headless einwandfrei - nur der
  Zweig, der ein einfaches `Part::Feature`/`Part::Box` als `App::Link` spiegelt, bleibt leer, ohne
  jede Fehlermeldung.
- Ein manuell (nur für den Test) nachgebautes `App::Link`-Spiegelobjekt wird beim nächsten
  `recompute()` von `synchronizeComponents()`s eigener Aufräum-Logik wieder gelöscht (`"pending
  remove of BoxA after recomputing document ..."`), weil es nicht in `objLinkMap` auftaucht - das
  bestätigt, dass `topLevelComponents`/`assemblyGroup` (`assembly->Group.getValues()` der
  **verlinkten**, fremden `AssemblyObject`-Instanz) beim headless-Ausführungszeitpunkt von
  `updateContents()` leer bzw. unvollständig ist, obwohl dieselbe `Group`-Abfrage direkt im
  Quelldokument selbst (`SynSub` allein geöffnet) korrekt `['BoxA', 'BoxB', 'Joints', 'Joint']`
  liefert.
- An den alten Bugreport-Dateien traten dabei zusätzlich (nur dort, nicht am frischen
  synthetischen Testfall) unabhängige `"Cannot create object 'Body'/'Box'"`-Meldungen auf sowie
  mehrere Joints mit bereits selbst `None`/unset `Reference1`/`Reference2` - beides vermutlich
  Alt-Zustand dieser mehrfach im Laufe der ursprünglichen Bug-Untersuchung gespeicherten
  Testdateien, nicht ursächlich mit dem hier beschriebenen Befund verwandt (der frische synthetische
  Testfall zeigte keine dieser Meldungen, aber denselben Kern-Befund: leere `Group`).

**Vermutung:** sehr wahrscheinlich derselbe oder ein eng verwandter Root Cause wie der bereits
dokumentierte `project_fcproject_freecadcmd_zero_joints_cold_load`-Befund ("headless `solve()`
meldet nach frischem `openDocument()` zuverlässig '0 joint(s)'") - beide Symptome passen zu
"irgendetwas, das während eines von `execute()` neu erzeugten Objekts/einer neu gelesenen
`Group`-Liste in einer FreeCADCmd-Rekompute-Kaskade nicht denselben Abschluss-/Nachbearbeitungs-Pass
bekommt wie in der GUI (dort vermutlich durch zusätzliche, event-loop-getriebene
Recompute-Anstöße kaschiert)". Nicht tiefer verfolgt - das wäre eine Untersuchung von
`AssemblyLink::synchronizeComponents()`/`App::Document`-Rekompute-Reihenfolge, komplett
unabhängig vom hier bearbeiteten Auftrag (`resolveJointReference()`/Diagnose), und explizit
außerhalb des für diese Sitzung vorgegebenen, bewusst engen Scopes. **Nicht behoben, nur
dokumentiert** - eigener Bugbericht/eigene Untersuchung empfohlen, falls relevant (z.B. falls
künftige headless-Testskripte für Schritt 3 wieder auf dasselbe Symptom stoßen).

### Verbleibende offene Fragen/Risiken für Schritt 3 (`objectPartMap`-Umbau)

- ~~Voller End-to-End-Beweis... steht noch aus~~ **ERLEDIGT (2026-08-28, live in der GUI
  nachgeholt):** `asm.diagnoseAddressableJoints()` auf `MinimalReproGrandTop` (echtes, per GUI
  geöffnetes/gespeichertes 2-Ebenen-Dokument, kein Headless-Workaround nötig) liefert:
  ```
  Original-Joint 'Assembly001.unterAssambly.Joint' -> Reference1: MinimalReproGrandTop#BoxA
  (subPath='Edge10'), Reference2: MinimalReproGrandTop#BoxB (subPath='Edge10')
  ```
  Beweist zweifelsfrei: der zwei Ebenen tief verschachtelte Original-Joint löst zu `BoxA`/`BoxB`
  als tatsächlich unterschiedlichen, korrekten Objekten auf - nicht mehr zur kollabierten
  `unterAssambly`-Hülle wie bei der bisherigen Kopier-Logik. Bonus: derselbe Lauf löste auch den
  Cross-Boundary-Joint `Assembly001.Joint002` (BoxA↔BoxD über die Top-Level-Grenze) korrekt zu
  zwei unterschiedlichen Objekten auf, GroundedJoints (kein Reference1/2) liefern erwartungsgemäß
  "nicht aufgelöst" statt eines Absturzes. Kein Crash, kein Verhaltensunterschied am Solve. Der
  headless-Mirror-Bug (oben dokumentiert) bleibt bestehen, betrifft aber nur `FreeCADCmd`-Skripte,
  nicht den echten Nutzungsweg über die GUI - für Schritt 3 kein Blocker mehr.
- Die in Abschnitt 2 (oben) vorgeschlagene Extraktion einer **gemeinsamen** privaten Walk-Hilfsfunktion
  (von `getMovingPartFromSel()` UND `resolveJointReference()` genutzt) wurde bewusst NICHT umgesetzt
  - `resolveJointReference()` dupliziert den Walk aktuell eigenständig. Grund: Risikominimierung
  (`getMovingPartFromSel()` ist aktiver Solve-/Drag-Code, siehe `Gui/Commands.cpp:342`,
  `Gui/ViewProviderAssembly.cpp:825,893` - ihn anzufassen, auch nur um eine Hilfsfunktion
  herauszuziehen, hätte das gleiche Chirurgie-Risiko wie eine funktionale Änderung, ohne dass Schritt
  2 das bräuchte). Für Schritt 3 (wo `getMovingPartFromRef()` selbst ersetzt werden soll) wird diese
  Konsolidierung ohnehin fällig - dann in einem Zug mit dem eigentlichen Umbau sinnvoller als jetzt
  isoliert.
- `getJoints()`s `subJoints`-Zweig (Zeile ~1178-1189) wurde **nicht** angefasst - `getJoints()`
  liest weiterhin ausschließlich über `AssemblyLink::getJoints()` (die Kopien). Für Schritt 3 muss
  diese Entscheidung (separate Diagnosefunktion vs. Integration in `getJoints()`) erneut getroffen
  werden, diesmal aber nicht mehr vermeidbar, weil Schritt 3 den tatsächlichen Rückgabewert von
  `getJoints()` ändern muss (siehe Abschnitt 3, oben: neuer `JointRef`-artiger Rückgabetyp) - dort
  reicht eine rein parallele, folgenlose Diagnosefunktion nicht mehr aus.
- Namenskollisionsrisiko der Adressierung wurde nicht getestet: `resolveJointReference()`s
  Adress-String verlässt sich auf eindeutige Objektnamen im flachen Dokument-Namensraum
  (`doc->getObject(name)`); zwei verschiedene `AssemblyLink`-Instanzen, die zufällig gleich
  benannte Quell-Objekte spiegeln (z.B. zwei verschiedene Kaufteil-Kopien, beide mit einem Kind
  namens `Body`), könnten kollidieren, wenn FreeCADs automatische Namens-Deduplizierung
  (`BoxA`, `BoxA001`, ...) beim Spiegeln zuschlägt - dann würde `nestingPrefix + "BoxA"` u.U. auf
  das FALSCHE `BoxA001` verweisen. `getMovingPartFromSel()` hat exakt dasselbe Risiko (nutzt
  dieselbe Namens-Resolution), ist aber seit Langem im Produktiveinsatz ohne gemeldete
  Kollisionsfälle - vermutlich, weil FreeCADs Auto-Rename beim Spiegeln (`doc->addObject(type,
  obj->getNameInDocument())`) bereits dafür sorgt, dass Kollisionsfälle selten/kontrolliert
  auftreten. Für Schritt 3 lohnt sich trotzdem ein bewusster Test mit zwei gleich benannten
  Quellteilen.

## Schritt 3 - Ergebnis (Teilfortschritt, NICHT abgeschlossen)

(2026-08-28, im ca. 40-Minuten-Zeitfenster der Nutzerabwesenheit begonnen. Patch-Datei:
`patches/step3-objectpartmap-addressing.patch` - **noch nicht** in die konsolidierten Patches
gemergt, bewusst separat gehalten, wie bei Schritt 2. Der Patch ist so gebaut, dass er sauber auf
den bereits angewendeten `step2-resolve-joint-reference.patch` aufsetzt - siehe "Wie geprüft" unten
für den Beweis.)

**Ehrliche Kurzfassung: von den drei im Auftrag verlangten Teilschritten wurde NUR Teilschritt 1
(`objectPartMap`-Schlüsseltyp) umgesetzt und verifiziert. Teilschritt 2
(`getMovingPartFromRef()` durch eine adressierungsbewusste Version ersetzen, inkl. `getJoints()`s
Rückgabetyp-Änderung) und Teilschritt 3 (`isMbDJointValid()` auf den vollen Schlüssel umstellen)
wurden NICHT begonnen** - das ist bewusst so entschieden, siehe Begründung unten, nicht ein
Zeitüberschreitungs-Abbruch mitten in der Arbeit.

### Was gebaut und verifiziert wurde (Teilschritt 1: `objectPartMap`-Schlüsseltyp)

**`AssemblyObject.h`:**

- Neuer Typ `PartKey = std::pair<App::DocumentObject*, std::string>` plus `PartKeyHash`
  (Standard-`hash_combine`-Formel) als eigenständiger, dokumentierter Baustein (nicht in
  `objectPartMap` selbst versteckt) - für spätere Wiederverwendung, falls Schritt 3.2/3.3 einen
  gleichartigen Schlüssel brauchen.
- `objectPartMap` von `std::unordered_map<App::DocumentObject*, MbDPartData>` auf
  `std::unordered_map<PartKey, MbDPartData, PartKeyHash>` umgestellt.
- `getMbDData()`/`getMbDPart()` um einen `subPath`-Parameter erweitert, **mit Default
  `std::string()`** - das ist der entscheidende Kompatibilitäts-Kniff: kein einziger der ca. 10
  bestehenden Aufrufer (`preDrag()`, `doDragStep()`, `validateNewPlacements()`,
  `handleOneSideOfJoint()`, `getRackPinionMarkers()`, `isMbDJointValid()`, ...) musste angepasst
  werden, weil sie alle weiterhin implizit den kanonischen leeren Sub-Pfad verwenden - exakt die im
  Auftrag verlangte Eigenschaft ("Teile ohne eigenen individuellen Sub-Pfad-Bedarf bekommen einfach
  einen leeren/kanonischen Sub-Pfad, damit sich für sie am Verhalten NICHTS ändert").

**`AssemblyObject.cpp`:** alle 13 Fundstellen aus dem Fundstellen-Katalog (Abschnitt 3, oben), die
direkt auf `objectPartMap` zugreifen, wurden nachgezogen:

- `preDrag()` (Iteration über alle Einträge, `pair.first` → `pair.first.first`),
- `doDragStep()` (`objectPartMap.find(part)` → `find(PartKey{part, ""})`),
- `validateNewPlacements()` (gleiches Muster),
- `savePlacementsForUndo()` (Iteration, `pair.first` → `pair.first.first`),
- `setNewPlacements()` (Iteration, gleiches Muster - das ist laut Fundstellen-Katalog die
  "zentrale" Stelle, die den Placement-Rückschreib-Pfad betrifft),
- `getMbDData()` selbst (die eigentliche Umbaustelle): RigidGroup-Repräsentant/-Mitglieder werden
  bewusst **immer** mit dem kanonischen leeren Sub-Pfad-Schlüssel adressiert (`PartKey{rep, ""}`,
  `PartKey{member, ""}`), unabhängig vom `subPath`, mit dem `getMbDData()` selbst aufgerufen wurde -
  das erhält RigidGroup-Bündelung 1:1, weil `ObjectsToRigidGroup` (`PropertyLinkList`) ohnehin nie
  einen Sub-Pfad kennt (siehe Abschnitt 1.3). Gleiches für `addConnectedFixedParts`
  (`bundleFixed`-Lambda) - `partToAdd` kommt aktuell noch aus `getMovingPartFromRef()` (liefert
  nach wie vor keinen Sub-Pfad), wird also ebenfalls mit `""` adressiert. Ein Sonderfall wurde
  zusätzlich behandelt, der im Architekturdokument noch nicht auftauchte: falls `part` selbst
  rigide-geclustert ist, aber mit einem nicht-kanonischen `subPath` angefragt wird (heute nie der
  Fall, da niemand `getMbDData()` mit nicht-leerem `subPath` aufruft - Vorbereitung für Schritt 3.2),
  fällt die Funktion sauber auf den kanonischen `(part, "")`-Eintrag zurück statt einen neuen,
  redundanten MbD-Teil für dasselbe Objekt anzulegen.
- `getMbDPart()` (dünner Wrapper, folgt automatisch).

**Nicht angefasst** (bewusst, siehe Fundstellen-Katalog-Einschätzung "unkritisch"): die drei
`objectPartMap.clear()`-Aufrufe in `solve()`/`generateSimulation()`/`exportAsASMT()` - reine
Lifecycle-Stellen, schlüsseltyp-unabhängig.

### Wie geprüft

1. **Build:** `Assembly`-Target baut nach der Änderung ohne einen einzigen Compiler-Fehler oder
   -Warning (`cmake --build build --target Assembly -- -j$(nproc)`), sowohl isoliert getestet
   (siehe Punkt 3) als auch im finalen Zustand.
2. **Vollständigkeits-Grep:** `grep -rn "getMbDPart(\|getMbDData(" src/Mod/Assembly` bestätigt: es
   gibt **keinen einzigen Aufrufer** dieser beiden Funktionen außerhalb von `AssemblyObject.h`/
   `.cpp` selbst (weder in `Gui/`, noch in `JointObject.py`, noch sonstwo) - die Signaturänderung
   (mit Default-Parameter) ist dadurch nachweislich vollständig rückwärtskompatibel, nichts wurde
   übersehen.
3. **Vergleichs-Build gegen die exakte Vor-Schritt-3-Baseline:** über einen temporären
   `git worktree` (`HEAD` + `step2-resolve-joint-reference.patch` angewendet, **ohne** die
   Schritt-3-Änderungen) wurde dieselbe headless-Testdatei (`MinimalReproTop.FCStd`, Kopie im
   Scratch-Verzeichnis) mit dem alten Binary UND danach erneut mit dem neuen (Schritt-3-)Binary
   durchlaufen - **die Konsolen-Ausgabe ist in beiden Läufen Zeile für Zeile identisch** (gleiche
   `getJoints()`/`removeUnconnectedJoints()`-Diagnosezeilen aus Schritt "kleiner erster Schritt",
   gleiches `"computed (0 joint(s), 1 grounded part(s))"`, gleiches
   `"Solve of '...' finished successfully"`). Das ist der konkrete Beleg für "keine
   Verhaltensänderung" - kein bloßes "hat nicht abgestürzt", sondern byte-identische Diagnose-Logs
   vorher/nachher an derselben Testdatei.
   - Der ohnehin bekannte, von diesem Patch unabhängige Cold-Load-Mirror-Bug (`getJoints()` meldet
     headless zuverlässig "0 joint(s)", siehe `project_fcproject_freecadcmd_zero_joints_cold_load`
     und Schritt-2-Befund oben) trat in **beiden** Läufen identisch auf - bestätigt zusätzlich, dass
     er nichts mit dieser Änderung zu tun hat.
   - Der Vergleichs-Build erforderte einen temporären `git worktree add --detach` (vermeidet ein
     riskantes Stash/Pop im eigentlichen Arbeitsverzeichnis) - wurde nach Gebrauch wieder mit
     `git worktree remove` entfernt, keine Altlasten.
4. **Patch-Konsistenz:** `git apply --check patches/step3-objectpartmap-addressing.patch` gegen den
   exakten Post-Schritt-2-Baumzustand (derselbe temporäre Worktree) läuft **fehlerfrei** durch -
   der Patch ist nachweislich in sich konsistent und baut sauber auf `step2-resolve-joint-
   reference.patch` auf, nicht auf einem zufälligen Zwischenzustand.
5. **RigidGroup-Regression:** **NICHT** end-to-end getestet - es gibt aktuell keine dedizierte,
   kleine RigidGroup-Testdatei im Repo (`bugreport-rigid-nested-joint-reference/` behandelt einen
   anderen, verwandten aber nicht identischen Bug - Joint-Referenz-Spiegelung, nicht
   `RigidGroupJoint`/`ObjectsToRigidGroup`). Die Absicherung für RigidGroup beschränkt sich in
   dieser Sitzung auf **Code-Inspektion** (siehe "getMbDData() selbst" oben: Repräsentant/Mitglieder
   werden konsequent mit kanonischem Schlüssel adressiert, computational äquivalent zum
   Vorher-Zustand) - **kein** echter Solve-Lauf mit einer aktiven Rigid Group wurde in dieser
   Sitzung durchgeführt. Das ist eine offene Lücke, kein "erledigt".
6. **GUI-Live-Test (Slider-Joint-Endbeweis):** **NICHT durchgeführt** - der Nutzer war während der
   gesamten Sitzung abwesend (das war die Auftragsvoraussetzung), ein interaktiver Zieh-Test in der
   GUI ist ohne Nutzer bzw. ohne GUI-Automatisierungs-Werkzeug in dieser Umgebung nicht möglich.
   Das ist ohnehin unabhängig von diesem Teilschritt: Teilschritt 1 allein ändert am eigentlichen
   Solver-*Verhalten* nichts (siehe Punkt 3, identische Logs) - der Slider-Joint-Endbeweis wird
   erst nach Teilschritt 2 (echte Sub-Pfad-Auflösung über `resolveJointReference()`) überhaupt
   sinnvoll versuchbar.

### Warum hier bewusst gestoppt wurde, statt Teilschritt 2/3 zu beginnen

- Teilschritt 2 (`getMovingPartFromRef()` ersetzen, `getJoints()`s Rückgabetyp ändern) ist laut
  Fundstellen-Katalog (Abschnitt 3, oben) selbst schon die größte Einzeländerung des gesamten
  Vorhabens: **jeder** Aufrufer von `getJoints()` (`solve()`, `isPartConnected()`,
  `getJointsOfPart()`, `slidingPartIndex()` - das ruft `getJoints()` selbst rekursiv innerhalb einer
  laufenden Solve-Traversierung auf) müsste im selben Zug umgestellt werden, weil ein gemischter
  Zwischenzustand (manche Aufrufer erwarten `vector<DocumentObject*>`, manche den neuen
  `JointRef`-artigen Typ) nicht kompiliert, geschweige denn korrekt liefe.
- Der einzige Weg, diese Änderung seriös zu verifizieren, ist der im Auftrag selbst geforderte
  GUI-Live-Test mit dem Slider-Joint (nur ein kompletter Solve-Durchlauf mit tatsächlich
  aufgelöstem Sub-Pfad zeigt, ob der Joint endlich funktioniert oder ob eine der zahlreichen
  Kaskaden-Stellen - `jointParts()`, `handleOneSideOfJoint()`, `removeUnconnectedJoints()` -
  während der Umstellung kaputtgegangen ist) - und dieser Test ist in einer Sitzung ohne
  anwesenden Nutzer nicht durchführbar (siehe Punkt 6 oben).
- Diese Codezone hat bereits zweimal echte FreeCAD-Abstürze verursacht (siehe Abschnitt 4).
  Teilschritt 2 in einer Sitzung ohne die Möglichkeit, das Endergebnis tatsächlich in der GUI zu
  verifizieren, würde bedeuten: entweder ungetesteten, potenziell absturzträchtigen Code
  einchecken, oder die Verifikation auf "hat sich kompiliert und ist beim headless-Recompute nicht
  abgestürzt" reduzieren - Letzteres wäre nach dem eigenen, in dieser Sitzung wiederholt bestätigten
  Maßstab ("Sorgfalt vor Geschwindigkeit", "ein unsicherer, unter Zeitdruck erzwungener Erfolg ist
  es nicht") nicht ausreichend, um es als "verifiziert" auszugeben.
- Teilschritt 1 dagegen ist per Konstruktion (Default-Parameter, byte-identische Logs) mit
  **Sicherheit** verhaltensneutral - ein seriöser, in sich abgeschlossener Fortschritt, der als
  Fundament für Teilschritt 2 gebraucht wird, aber für sich genommen kein Risiko trägt.

### Genauer Ansatzpunkt für die nächste Sitzung (Teilschritt 2)

1. **Datei:** `AssemblyUtils.h`/`.cpp` - `getMovingPartFromRef()` (Zeile 717/726 vor jeder eigenen
   Änderung, siehe Fundstellen-Katalog) bleibt als Legacy-Funktion bestehen (wird noch von
   `getJointOfPartConnectingToGround()`, `getJointsOfPart()`, `isJointConnectingPartToGround()`
   gebraucht, die NICHT Teil des `getJoints()`-Rückgabetyp-Umbaus sind, siehe Fundstellen-Katalog
   zweite Tabelle). **Nicht löschen**, nur `handleOneSideOfJoint()`, `getRackPinionMarkers()`,
   `slidingPartIndex()`, `isMbDJointValid()`, `removeUnconnectedJoints()` schrittweise auf
   `resolveJointReference()` umstellen (die bereits fertige, in Schritt 2 gebaute Funktion - siehe
   oben, Signatur `resolveJointReference(const AssemblyObject* solvingAssembly,
   App::DocumentObject* joint, const char* pName, const std::string& nestingPrefix)`).
2. **Datei:** `AssemblyObject.h`, Zeile ~168-169 (`getJoints()`-Deklaration) - Rückgabetyp von
   `std::vector<App::DocumentObject*>` auf einen neuen Typ ändern, der pro Joint zusätzlich das
   `nestingPrefix` mitführt, mit dem er gefunden wurde (Vorschlag aus Abschnitt 3, oben:
   `std::vector<std::pair<App::DocumentObject*, std::string>>` oder ein kleines `JointRef`-Struct
   analog zu `ResolvedJointRef`). **Zuerst** an einer Kopie/Branch isoliert durchspielen, welche
   Aufrufer wirklich alle betroffen sind (Grep-Bestätigung wie in dieser Sitzung Punkt 2 oben, für
   `getJoints(` statt `getMbDData(`/`getMbDPart(`) - die Liste ist länger und stärker verzweigt.
3. **Datei:** `AssemblyObject.cpp`, `getJoints()`s `subJoints`-Zweig (aktuell Zeile ~1178-1189,
   exakte Zeile nach dem Schritt-3.1-Diff leicht verschoben - neu suchen mit
   `grep -n "add sub assemblies joints" AssemblyObject.cpp`): von `assembly->getJoints()`
   (`AssemblyLink`-Kopien lesen) auf `subLink->getLinkedAssembly()->getJoints(...)` (echte
   Original-Instanz, rekursiv) umstellen, `nestingPrefix` dabei aufbauen - strukturell identisch zu
   `diagnoseAddressableJointsRecursive()` aus Schritt 2 (`AssemblyUtils.cpp`), die als Vorlage/
   Referenzimplementierung dienen kann (dort bereits gebaut und verifiziert, nur eben nicht in
   `getJoints()` selbst integriert).
4. **Nach jedem einzelnen Aufrufer-Umbau** (nicht erst am Ende): neu bauen, gegen die
   Minimal-Repro-Kopien headless testen (Basis-Regressionscheck wie in dieser Sitzung Punkt 3),
   und **sobald der Nutzer wieder anwesend ist**, den eigentlichen GUI-Live-Test mit dem
   tief verschachtelten Slider-Joint durchführen (BoxA-BoxB in `MinimalReproSub`, sichtbar von
   `MinimalReproGrandTop` aus ziehen) - das ist der einzige Test, der zeigt, ob der Umbau
   tatsächlich funktioniert, nicht nur kompiliert.
5. Erst wenn Teilschritt 2 live verifiziert ist: Teilschritt 3 (`isMbDJointValid()` auf
   `getMbDPart(part1, subPath1) == getMbDPart(part2, subPath2)` umstellen, wobei `subPath1`/
   `subPath2` aus dem `resolveJointReference()`-Ergebnis kommen) - das ist dann nur noch eine
   kleine, lokale Änderung an einer einzigen Funktion, weil Teilschritt 1 (`objectPartMap`) und
   Teilschritt 2 (Sub-Pfad-Fluss) beide schon stehen.

## Teilschritt 2 - Konkreter Detailplan (2026-08-28, live erarbeitet, NICHT umgesetzt)

Beim Versuch, Teilschritt 2 live umzusetzen, wurde der Plan aus Abschnitt 3 (oben) verfeinert
und deutlich verkleinert - festgehalten hier, damit die naechste Sitzung nicht wieder von vorne
anfangen muss.

### Wichtige Klarstellung des Mechanismus (warum der Fix ueberhaupt noetig ist)

Jede Verschachtelungsebene (`AssemblyLink`) haelt ihre EIGENE, komplett unabhaengige Kopie jedes
Teils (`synchronizeComponents()`, `AssemblyLink.cpp` Zeile 349ff.) - z.B. hat `GrandTop` eine
eigene "BoxB", physisch lebend in GrandTops eigenem Dokument, komplett getrennt von Subs echter
"BoxB". Diese Kopien werden NICHT automatisch bei jedem Recompute mit dem Original
synchronisiert (kein generischer Placement-Sync-Mechanismus gefunden, nur punktuelle
`Base::Placement()`-Resets an einer Stelle, `AssemblyLink.cpp` Zeile 188) - ihre Placement wird
nur dann korrekt gesetzt, wenn ein Joint AUF GENAU DIESER EBENE (in dieser AssemblyLink eigenem
lokalem Solve) sie tatsaechlich referenziert. Bei BoxB gibt es aber gar keinen lokalen Joint auf
GrandTop-Ebene - ihr einziger Constraint (der Slider zu BoxA) lebt ausschliesslich in Subs
eigenem Dokument. Deshalb "haengt BoxB einfach nur rum", frei beweglich, von GrandTops Solve
komplett ignoriert. Der Fix muss also dafuer sorgen, dass GrandTops EIGENER Solve diesen tiefen
Joint kennt UND ihn auf die RICHTIGEN, LOKALEN Kopien (GrandTops eigene BoxA/BoxB, nicht Subs
Originale, nicht die kollabierte Zwischenhuelle) anwendet - genau das, was
`resolveJointReference()` bereits korrekt liefert (siehe Schritt-2-Ergebnis oben).

### Verkleinerter Umbauplan (deutlich kleiner als der urspruengliche ~20-Stellen-Umbau)

1. **`getJoints()` bekommt einen zusaetzlichen Parameter** `const std::string& nestingPrefix =
   std::string()` - KEIN Rueckgabetyp-Umbau noetig (bleibt `std::vector<App::DocumentObject*>`).
   Alle bestehenden Aufrufer (die den neuen Parameter nicht angeben) sind dadurch komplett
   unveraendert.
2. **`subJoints`-Rekursionszweig** (Zeile ~1185-1195): statt `assembly->getJoints()` (liest
   Kopien aus der `AssemblyLink`-eigenen `JointGroup`) muss `assembly->getLinkedAssembly()`
   (liefert die echte, tiefer liegende `AssemblyObject`-Instanz) rekursiv mit
   `nestedAssembly->getJoints(delBadJoints, subJoints, verboseLog, nestingPrefix +
   assembly->getNameInDocument() + ".")` aufgerufen werden. Das liefert die echten
   Original-Joint-Objekte (nicht Kopien) samt korrektem, rekursiv aufgebautem Praefix.
3. **Neue Zwischenspeicherung** noetig, um das pro Joint aufgeloeste `nestingPrefix` an
   `isMbDJointValid()`/`jointParts()` weiterzureichen, OHNE die Rueckgabetyp-Signatur zu
   aendern: eine neue Member-Map in `AssemblyObject.h`, z.B.
   `std::unordered_map<App::DocumentObject*, std::string> jointNestingPrefixMap;` - befuellt
   waehrend des rekursiven Abstiegs in Schritt 2 oben.

### Der heikle, noch ungeloeste Punkt: Lebenszyklus der neuen Zwischenspeicherung

`getJoints()` wird nicht nur von `solve()` aufgerufen, sondern staendig auch von
`isPartConnected()`/`getJointsOfPart()` waehrend des interaktiven Ziehens (siehe Fix-16-Kommentar
in `AssemblyObject.h` Zeile 193-198 - das ist derselbe CPU-Vorfall, der `verboseLog` erst noetig
gemacht hat). Wuerde `jointNestingPrefixMap` bei JEDEM `getJoints()`-Aufruf neu befuellt (statt
nur beim eigentlichen `solve()`-Durchlauf), koennte ein waehrend des Ziehens ausgeloester Aufruf
die Map veraendern, WAEHREND `solve()` gerade dabei ist, sie fuer `isMbDJointValid()`/
`jointParts()` auszuwerten - ein potenzielles Race/Inkonsistenz-Problem, auch wenn FreeCAD
single-threaded ist (das eigentliche Risiko ist logische Verschachtelung/Reentrancy, nicht
Threading - siehe der bereits dokumentierte `synchronizeGroundedAndRigidJoints()`-Absturz fuer
ein Beispiel, wie sowas schiefgehen kann).

**Empfehlung fuer die naechste Sitzung:** `jointNestingPrefixMap.clear()` an EXAKT denselben
Stellen wie `objectPartMap.clear()` platzieren (`solve()` Zeile ~227, `generateSimulation()`
Zeile ~409, `exportAsASMT()` Zeile ~710 - siehe Fundstellen-Katalog Abschnitt 3, oben) - NICHT
am Anfang von `getJoints()` selbst. Das bedeutet aber: `getJoints()`s `subJoints`-Rekursion
(Schritt 2 oben) baut die Map bei JEDEM Aufruf weiter auf (nie geleert dazwischen), was fuer
den reinen `solve()`-Anwendungsfall korrekt ist, aber bedeutet, dass waehrend des Ziehens
aufgerufene `getJoints()`-Instanzen die Map ebenfalls befuellen (unschaedlich, wenn nachfolgende
Lookups nur fuer Joints erfolgen, die tatsaechlich im JEWEILS AKTUELLEN `joints`-Rueckgabewert
enthalten sind - der Map-Eintrag fuer einen Joint aus einem FRUEHEREN, andersartigen Aufruf
wuerde einfach nie nachgefragt). Muss vor der Umsetzung nochmal explizit durchdacht/getestet
werden, statt live unter Zeitdruck entschieden zu werden - deshalb hier bewusst nicht mehr am
selben Abend umgesetzt.

### Naechste konkrete Schritte (fuer die naechste Sitzung, in dieser Reihenfolge)

1. `nestingPrefix`-Parameter an `getJoints()` ergaenzen (Header + .cpp), Rekursionszweig auf
   `getLinkedAssembly()` umstellen wie oben beschrieben. Bauen, mit `diagnoseAddressableJoints()`
   als Referenz gegentesten (sollte dieselben Original-Joints/Praefixe liefern wie die bereits
   verifizierte Diagnosefunktion aus Schritt 2).
2. `jointNestingPrefixMap` ergaenzen, Clear-Lebenszyklus wie oben beschrieben umsetzen und
   EXPLIZIT testen (z.B. waehrend eines Ziehvorgangs mehrfach `solve()` ausloesen, pruefen dass
   nichts durcheinanderkommt).
3. `isMbDJointValid()` auf die neue Map + `resolveJointReference()` umstellen (kleinste, lokalste
   Aenderung von allen - siehe Abschnitt 3 oben).
4. `jointParts()`/`makeMbdJoint()` ebenso umstellen, DAS ist die Stelle, an der der Joint
   tatsaechlich in eine MbD-Randbedingung uebersetzt wird - erst danach kann der End-to-End-Beweis
   (BoxB laesst sich nur noch entlang der Slider-Achse ziehen) ueberhaupt gelingen.
5. Alle anderen `getMovingPartFromRef()`-Aufrufstellen (Redundanz-Vorpruefung, Undo, etc.) bleiben
   bewusst UNVERAENDERT fuer diesen ersten funktionalen Fix - sie betreffen nicht den kritischen
   Pfad "Joint tatsaechlich anwenden", nur Diagnose/Sekundaerfunktionen. Koennen in einem
   spaeteren, separaten Schritt nachgezogen werden.

## Teilschritt 2 - Umsetzung (2026-08-28)

Setzt den obigen "Konkreter Detailplan" um. Patch-Datei: `patches/step2b-getjoints-nesting-prefix.patch`
(git diff im `freecad-source`-Repo gegen den Stand nach `step2-resolve-joint-reference.patch` +
`step3-objectpartmap-addressing.patch`, **noch nicht** in die konsolidierten Patches gemergt, wie bei
Schritt 2/3). Betrifft `AssemblyObject.h`, `AssemblyObject.cpp`, `AssemblyLink.cpp`.

### Was fertig UND verifiziert ist

**1. `getJoints()` bekommt `nestingPrefix`-Parameter, `subJoints`-Zweig liest Originale statt Kopien.**
Genau wie im Detailplan: vierter Parameter `const std::string& nestingPrefix = std::string()` (kein
Rueckgabetyp-Umbau). Der `subJoints`-Zweig steigt jetzt über `AssemblyLink::getLinkedAssembly()`
rekursiv in die ECHTE verlinkte `AssemblyObject`-Instanz ab (statt `AssemblyLink::getJoints()`, die
KOPIEN aus der `AssemblyLink`-eigenen `JointGroup` zu lesen), mit `nestedPrefix = nestingPrefix +
assembly->getNameInDocument() + "."` - strukturell identisch zu
`diagnoseAddressableJointsRecursive()` aus Schritt 2, wie geplant als Vorlage genutzt.

**Wichtige, beim Umsetzen selbst gefundene Ergaenzung zum Detailplan:** rigide (`Rigid=true`)
Unter-`AssemblyLink`s werden jetzt EXPLIZIT uebersprungen (`if (assembly->isRigid()) continue;`).
Im alten Code (Kopien lesen) passierte das nur zufaellig, weil `ensureNoJointGroup()` die Kopie-
`JointGroup` einer rigiden `AssemblyLink` leer haelt. Mit "Originale lesen" waere dieser Zufalls-
schutz weg gewesen - die verlinkte `AssemblyObject`-Instanz hat ja unabhaengig von ihrem
Einbau-Modus ihre eigenen Joints. Ohne den expliziten Check haette der blosse Wechsel der
Datenquelle (Kopien -> Original) ungewollt auch die INTERNEN Joints einer rigide eingebundenen
Unterbaugruppe in den Solve der aeusseren Baugruppe gezogen - ein neuer, selbstgemachter Fehler.
Gegen `diagnoseAddressableJoints()` gegengeprueft (siehe Log unten) - beide Mechanismen finden
strukturell dieselben Original-Joints mit denselben Praefixen.

**2. `jointNestingPrefixMap` - Lebenszyklus wie geplant, Merge-Mechanismus zusaetzlich abgesichert.**
Neue Member-Map `std::unordered_map<App::DocumentObject*, std::string> jointNestingPrefixMap;` in
`AssemblyObject.h`. `clear()` an denselben drei Stellen wie `objectPartMap.clear()`
(`solve()`/`generateSimulation()`/`exportAsASMT()`, jeweils VOR dem nachfolgenden `getJoints()`-
Aufruf), nicht bei jedem `getJoints()`-Aufruf selbst - exakt wie im Detailplan empfohlen.

**Beim Umsetzen selbst kritisch nachgedacht und einen konkreten Fallstrick am geplanten Ansatz
gefunden, der im Detailplan noch nicht auftauchte:** `getJoints()` wird rekursiv auf einer ANDEREN
`AssemblyObject`-Instanz aufgerufen (`nestedAssembly->getJoints(...)`), nicht nur auf `this`. Wuerde
jede Rekursionsebene naiv direkt in `this->jointNestingPrefixMap` schreiben, wuerde das JEWEILS
FALSCHE Objekt beschrieben (der Eintrag fuer einen tief verschachtelten Joint wuerde in der Map der
TIEFEN, nicht der WURZEL-Instanz landen - genau die Instanz, die spaeter `isMbDJointValid()`/
`jointParts()` tatsaechlich aufruft, saehe diesen Eintrag nie). Geloest ueber denselben
"Ebene-fuer-Ebene-hochreichen"-Mechanismus, den `joints` selbst schon nutzt (`joints.insert(...)`):
jede Rekursionsebene schreibt AUSSCHLIESSLICH in ihre EIGENE Map (fuer eigene Joints direkt, fuer
geerbte Joints per gezieltem Schluessel-Kopieren aus der Kind-Instanz-Map NACH deren Rueckkehr) -
beim Zurueckbubbeln bis zur Wurzel landen so alle Eintraege korrekt bei der tatsaechlich loesenden
Instanz. Sicher, weil private Member derselben Klasse ueber Instanzgrenzen hinweg lesbar sind und
weil nur fuer Schluessel gelesen wird, die im SELBEN Aufruf soeben frisch geschrieben wurden (siehe
Code-Kommentar in `AssemblyObject.cpp` fuer die volle Herleitung). Eine Alternative (ein einzelner,
per Zeiger durchgereichter "root"-Parameter analog zu `diagnoseAddressableJointsRecursive(root,
assembly, ...)`) waere ebenfalls sicher gewesen, wurde aber verworfen, weil sie eine zusaetzliche
Signaturaenderung (root-Parameter) an einer bereits ueberladenen Funktion gebraucht haette - der
gewaehlte Ansatz kommt ohne jede weitere Signaturaenderung aus.

**3. `isMbDJointValid()` auf `resolvePartForMbD()` umgestellt.** Neue private Hilfsfunktion
`App::DocumentObject* resolvePartForMbD(App::DocumentObject* joint, const char* propRefName)`:
liest `nestingPrefix` aus `jointNestingPrefixMap` (Default `""`, falls kein Eintrag - kann bei
Aufrufen ausserhalb der `getJoints()`-Kaskade passieren, dann identisch zum Altverhalten), ruft
`resolveJointReference(this, joint, propRefName, nestingPrefix)` auf, faellt bei einem leeren
Ergebnis defensiv auf die alte `getMovingPartFromRef()` zurueck. `isMbDJointValid()` nutzt das jetzt
fuer `part1`/`part2` statt der alten Funktion direkt.

**4. `jointParts()`/`makeMbdJoint()` ebenso umgestellt - die eigentliche Wirkstelle.**
`handleOneSideOfJoint()` (liefert den MbD-Marker-Namen fuer `Reference1`/`Reference2`, aufgerufen
aus `makeMbdJoint()`) und `getRackPinionMarkers()`s `part1` nutzen jetzt ebenfalls
`resolvePartForMbD()` fuer das an `getMbDData()` uebergebene `part`-Argument - **das** ist die
Stelle, die einen Joint tatsaechlich einem MbD-Koerper zuordnet, ohne diese Aenderung waere der
Rest reine Diagnose geblieben.

**Bewusste, beim Umsetzen selbst getroffene Abweichung vom urspruenglichen Konzept (Abschnitt "3.
Auswirkung auf getJoints(), isMbDJointValid(), objectPartMap/getMbDData()", weiter oben in diesem
Dokument):** dort war vorgeschlagen, `getMbDData()`/`getMbDPart()` mit dem VOLLEN `(obj, subPath)`-
Paar aus `resolveJointReference()` aufzurufen. Das wurde bewusst NICHT so gemacht -
`resolveJointReference()`s `subPath` ist in JEDEM bisher beobachteten Fall (siehe Schritt-2-
Testergebnisse: `subPath='Edge10'` sowohl fuer den flachen ALS AUCH den korrekt aufgeloesten tief
verschachtelten Fall) nur noch der reine GEOMETRIE-Sub-Pfad auf dem bereits gefundenen Zielobjekt,
niemals ein weiterer, eigenstaendiger Teile-Identitaets-Unterschied - jede zwischenliegende
`AssemblyLink`-Ebene wird beim Walk bereits konsumiert/uebersprungen, bevor `result.obj` gesetzt
wird; es gibt in der heutigen Datenmodellierung von `Reference1`/`Reference2` keinen Fall, in dem
eine EINZELNE Referenz durch mehrere verschiedene, eigenstaendig bewegliche Assembly-Teile
hindurchlaeuft. Wuerde man `result.subPath` trotzdem in den `PartKey` einfliessen lassen, wuerde
JEDER Joint (auch jeder heute schon funktionierende, nicht verschachtelte) ploetzlich einen
nicht-leeren, individuellen Sub-Pfad bekommen - zwei verschiedene Joints auf demselben Teil, aber an
unterschiedlichen Kanten, wuerden dann faelschlich ZWEI VERSCHIEDENE MbD-Koerper fuer dasselbe
physische Teil erzeugen (eine Regression fuer so gut wie jede bestehende Baugruppe mit mehr als
einem Joint pro Teil). Deshalb wird `getMbDData()`/`getMbDPart()` weiterhin IMMER mit dem
kanonischen leeren Sub-Pfad aufgerufen - `result.obj` allein reicht zur Unterscheidung, genau das
leistet die Verschachtelungs-Aufloesung in `resolveJointReference()` bereits: zwei verschiedene
Enkelteile werden zu zwei verschiedenen `result.obj`-Werten aufgeloest, nicht mehr zum selben
kollabierten Wrapper. Die `PartKey`/`objectPartMap`-Umstellung aus Schritt 3 bleibt dadurch
computational aequivalent zu "Schluessel = Objekt-Pointer" (der Sub-Pfad-Teil des Schluessels ist
in der Praxis immer `""`) - nicht verschwendet, aber (noch) nicht der Ort, an dem die eigentliche
Disambiguierung passiert.

**5. Alle anderen `getMovingPartFromRef()`-Aufrufstellen bewusst unveraendert gelassen** - wie
geplant (Redundanz-Vorpruefung in `getJoints()`s Top-Level-Schleife, `getJointOfPartConnectingToGround()`,
`getJointsOfPart()`, `isJointConnectingPartToGround()`, `removeUnconnectedJoints()`/
`getConnectedParts()`, `slidingPartIndex()`s Vergleichslogik, die obj/ref-basierte Placement-
Berechnung in `handleOneSideOfJoint()`/`getRackPinionMarkers()`).

**Zusaetzlicher, beim Umsetzen selbst gefundener und behobener Kollateral-Risiko-Fund (nicht im
Detailplan vorgesehen):** `AssemblyLink::synchronizeJoints()` (die ALTE Kopier-Pipeline) ruft
selbst `assembly->getJoints(false, true)` auf (`subJoints=true`, seit dem "kleinen ersten Schritt"),
um eine Ebene bereits kopierter Enkel-Joints zu bekommen. Mit der neuen `getJoints()`-Rekursion
haette DIESER Aufruf jetzt beliebig tief in `assembly`s eigene Unter-Baugruppen hinabgestiegen und
deren ORIGINAL-Joints zurueckgeliefert - deren `Reference1`/`Reference2` zeigen auf Objekte in einem
KOMPLETT ANDEREN Dokument (der jeweils tiefsten Unter-Baugruppe), fuer die `findLocalAncestor()`
(laeuft die Struktur-Eltern-Kette NUR innerhalb desselben Dokuments hoch) nicht ausgelegt ist - ein
neues, ungetestetes Fehlerbild in genau der Codezone mit der zweifachen Absturzhistorie, fuer
keinen erkennbaren Nutzen (der urspruengliche Zweck von `subJoints=true` an dieser Stelle - tiefe
Joints fuer die `isMbDJointValid()`-Diagnose sichtbar machen - ist seit dieser Aenderung
gegenstandslos, weil `solve()` tiefe Joints jetzt ueber einen komplett EIGENSTAENDIGEN Pfad
erreicht). Deshalb an dieser einen Stelle bewusst auf `assembly->getJoints(false, false)`
zurueckgesetzt - der Wert, der dort schon jahrelang (vor dem "kleinen ersten Schritt") stand. Die
Kopier-Pipeline selbst (`synchronizeJoints()`/`handleJointReference()`/`findLocalAncestor()`) bleibt
davon unberuehrt und unangetastet - ihre geplante Entfernung ist weiterhin ein spaeterer, separater
Schritt (siehe Abschnitt "4. Migrationspfad" oben).

### Wie geprueft (alles headless, `FreeCADCmd`, Kopien in einem Scratch-Verzeichnis, NIE die
Originale)

1. **Build:** `Assembly`-Target baut nach jeder Teilaenderung ohne Fehler/Warnungen.
2. **Flacher Fall (`MinimalReproTop.FCStd`, 1 Ebene), Regressionscheck:** `solve()`-Konsolen-
   Ausgabe (Joint-Filter-Meldungen, `"computed (0 joint(s), 1 grounded part(s))"`,
   `"finished successfully"`) ist Zeile fuer Zeile identisch vor/nach diesem Patch - der bekannte
   `0 joint(s)`-Cold-Load-Befund (siehe `project_fcproject_freecadcmd_zero_joints_cold_load`) tritt
   unveraendert in beiden Faellen auf (Joint-Python-Proxy laedt headless nicht, `hasAttr(
   "setJointConnectors")` liefert `False`, daher werden hier - wie schon vor Schritt 2/3 -
   headless NIE echte Joints an `jointParts()`/`isMbDJointValid()`/`handleOneSideOfJoint()`
   weitergereicht). **Wichtige Einschraenkung:** das bedeutet, `resolvePartForMbD()` selbst wird
   durch diesen Test nicht durchlaufen - nur strukturell/durch Code-Inspektion, nicht per echtem
   Solve-Lauf verifiziert.
3. **Verschachtelter Fall (`MinimalReproGrandTop.FCStd`, 2 Ebenen), strukturelle Traversierungs-
   Verifikation:** `asm.solve()` (GrandTop) durchlaeuft in einem EINZIGEN Solve-Aufruf jetzt
   nachweislich `getJoints()`-Meldungen fuer alle drei Ebenen (`getJoints('MinimalReproGrandTop#
   Assembly')`, `getJoints('MinimalReproTop#Assembly')`, `getJoints('MinimalReproSub#Assembly')`) -
   vor diesem Patch waere dort nie `'MinimalReproSub#Assembly'` aufgetaucht (die alte Rekursion las
   nur `Assembly001`s eigene, flache Kopie-`JointGroup`, stieg nie in `Sub`s eigene Instanz ab).
   Gegen `diagnoseAddressableJoints()` gegengeprueft: liefert fuer dieselbe Datei
   `Assembly001.Joint`, `Assembly001.Joint002`, `Assembly001.unterAssambly.GroundedJoint`,
   `Assembly001.unterAssambly.Joint` - exakt dieselben Original-Joints/Praefixe, die die neue
   `getJoints()`-Rekursion strukturell erreicht (der `GroundedJoint`-Eintrag wird von `getJoints()`
   bewusst separat gefiltert, kein Widerspruch). **Der bekannte, dokumentierte
   `synchronizeComponents()`-Mirror-Bug unter `FreeCADCmd`** (leere `Group` fuer einfache
   Part-Mirrors, siehe "Schritt 2 - Ergebnis" oben) bedeutet: die FINALE Identitaetsaufloesung in
   `resolveJointReference()` faellt fuer den tiefen Joint headless auf `ResolvedJointRef{}` zurueck
   (der lokale Mirror "BoxA" existiert in `GrandTop`s Dokument nicht), `resolvePartForMbD()` faellt
   dadurch defensiv auf die alte `getMovingPartFromRef()` zurueck (liefert `Sub`s eigenes,
   cross-document Original) - **das ist nur strukturell/Kompilier-verifiziert, kein End-to-End-
   Beweis, der End-to-End-Beweis braucht die GUI** (dort funktioniert der Mirror-Mechanismus
   nachweislich, siehe der bereits zitierte GUI-Live-Test in "Schritt 3 - Ergebnis" oben).
4. **Stresstest (Crash-/Reentrancy-Freiheit):** 3x frisches `openDocument()` der 2-Ebenen-Datei,
   je 5x `solve()` in Folge, dann `Assembly001.Rigid` True->False->True mit Recompute+Solve bei
   jedem Schritt (uebt den `isRigid()`-Skip UND `synchronizeJoints()`s korrigierten Aufruf aktiv
   aus), abschliessend nochmal Solve - **kein Crash, keine Exception ausser den bekannten,
   unabhaengigen PySide6-Import-Fehlermeldungen**, ueber alle 3 Iterationen identisch.
5. **RigidGroup-Regression:** wie schon bei Schritt 3 NICHT end-to-end mit einer echten Rigid Group
   getestet (keine dedizierte Testdatei im Repo) - Absicherung bleibt auf Code-Inspektion
   beschraenkt (RigidGroup-Bündelung in `getMbDData()` ist von diesem Patch nicht beruehrt, da
   `resolvePartForMbD()` nur VOR `getMbDData()` ansetzt, nicht innerhalb).
6. **GUI-Live-Test (Slider-Joint-Endbeweis): NICHT durchgefuehrt** - wie in den vorigen Schritten,
   braucht eine anwesende Person mit echter Maus-Interaktion. Siehe naechster Abschnitt fuer die
   genaue Anleitung.

### Naechster, konkreter manueller Testschritt (braucht die GUI, kann nicht automatisiert werden)

1. FreeCAD (die selbst gebaute Version, per `resources/run-freecad-26.3.sh` oder Desktop-Starter)
   oeffnen.
2. **Eine FRISCHE KOPIE** von `patches/bugreport-nested-flex-joint-detach/MinimalReproGrandTop.FCStd`
   oeffnen (NIE das Original direkt bearbeiten) - stellt sicher, dass auch `MinimalReproTop.FCStd`
   und `MinimalReproSub.FCStd` im selben (kopierten) Verzeichnis liegen, da `GrandTop` sie per
   Cross-Document-Link referenziert.
3. Im Baum: `GrandTop` -> `Assembly001` (die eingebettete `Top`-Baugruppe) -> `unterAssambly` (die
   darin eingebettete `Sub`-Baugruppe) aufklappen, bis `BoxA`/`BoxB` sichtbar sind (die zwei ueber
   den Slider-Joint verbundenen, zwei Ebenen tief verschachtelten Teile).
4. `BoxB` (oder `BoxA`, je nachdem welches Teil beim Erstellen des Joints als beweglich markiert
   wurde) mit der Maus im 3D-Fenster anklicken und ziehen.
5. **Erwartetes Ergebnis bei erfolgreichem Fix:** `BoxB` laesst sich NUR NOCH entlang der
   Slider-Achse bewegen (der Joint haelt), nicht mehr frei in alle Richtungen. Das Report-View
   sollte beim Ziehen `Assembly: Solving 'MinimalReproGrandTop#Assembly'...` zeigen (GrandTops
   eigener Solve laeuft) - bei aktiviertem Fehler-Logging (`verboseLog`, nur in `solve()` aktiv,
   nicht in `doDragStep()`) waere der tiefe Joint dort als "UEBERNOMMEN" sichtbar, sofern ein
   voller Solve (nicht nur ein Drag-Schritt) ausgeloest wird.
6. **Falls es NICHT haelt:** vor dem Vermuten eines neuen Fehlers zuerst pruefen, ob das erwartete
   Symptom aus Abschnitt 2.5 (oben) noch zusaetzlich zuschlaegt - "nur die aktuell aktive/aeusserste
   Baugruppe bekommt waehrend eines Drags `preDrag()`/`doDragStep()`-Aufrufe" ist eine SEPARATE,
   durch Teilschritt 2 NICHT behobene Baustelle (siehe Abschnitt 5, "Der interaktive Zieh-Pfad ist
   eine dritte, im Prinzip unabhaengige Baustelle"). Ein vollstaendiger Dokument-Recompute (Menü
   Bearbeiten -> Neuberechnen bzw. `Strg+R`) nach dem Ziehen loest dagegen JEDE `AssemblyObject`-
   Instanz einzeln und sollte unabhaengig von der Drag-Frage zeigen, ob der Joint beim SOLVEN
   (nicht beim interaktiven Ziehen) tatsaechlich haelt/den korrekten DoF durchsetzt - das waere
   bereits ein Teilerfolg, selbst wenn das interaktive Ziehen noch die Drag-Baustelle zeigt.

### Betriebs-Lektion aus dieser Sitzung (fuer kuenftige Sessions wichtig)

Beim Testen dieses Patches wurde versehentlich `/home/maxx/freecad/install/lib/AssemblyApp.so` (die
gemeinsame, von der echten GUI des Nutzers genutzte Installation) neu gebaut/deployt, OHNE das
ABI-abhaengige `AssemblyGui.so` im selben Zug neu zu bauen - `AssemblyGui.so` ruft `getJoints()`
auf, dessen mangled Symbolname sich durch den neuen `nestingPrefix`-Parameter aendert, das fuehrte
zu "undefined symbol" beim Laden JEDES Assembly-Objekts in der echten Projektdatei des Nutzers
(vermutlich auch Ursache eines vorherigen, faelschlich einem LCS-Problem zugeschriebenen Crashs).
Sofort behoben (`AssemblyGui`-Target mitgebaut, beide `.so` gemeinsam deployt, per `nm`/`ldd -r`
und einem echten Offscreen-GUI-Start verifiziert). **Ab sofort gilt fuer alle Sessions, die gegen
den selbst gebauten FreeCAD testen:** niemals mehr nach `/home/maxx/freecad/install` deployen -
stattdessen einen isolierten Sandbox-Install-Prefix nutzen (in dieser Sitzung angelegt unter
`/home/maxx/freecad/install-claude-sandbox`, eine ECHTE `cp -a`-Kopie, bewusst KEINE Hardlinks, da
`cp` beim spaeteren Ueberschreiben sonst via Hardlink-Aliasing die echte Installation mitveraendert
haette) - und, falls doch einmal ein App-seitiges Modul neu gebaut wird, IMMER auch das
zugehoerige Gui-Modul mitbauen/mitdeployen, nie nur eines von beiden.

## Teilschritt 2 - GUI-Testergebnis (2026-08-29) und Teilschritt 2c

### Der Live-Test (durchgefuehrt vom Nutzer)

Echter Drag-Versuch an `BoxB` in einer per GUI geoeffneten Kopie von `MinimalReproGrandTop.FCStd`.
**Ergebnis: der Slider-Joint haelt noch nicht** - `BoxB` laesst sich weiterhin frei ziehen. **Aber**
die Ursache hat sich nachweislich verschoben - das ist ein echter Teilerfolg, kein Rueckschritt.
Log-Auszug (`~/freecad/logs/freecad-compiled-20260829-064550.log`, Zeile ~426-443):

```
Assembly: Solving 'MinimalReproGrandTop#Assembly'...
Assembly: getJoints('MinimalReproGrandTop#Assembly') - Joint 'GroundedJoint' uebersprungen (isError=0, Suppressed=0).
Assembly: removeUnconnectedJoints('MinimalReproGrandTop#Assembly') - 2 geerdete(s) Teil(e), 2 erreichbare(s) Teil(e), pruefe 3 Joint(s).
Assembly: removeUnconnectedJoints('MinimalReproGrandTop#Assembly') - Joint 'Joint' entfernt: part1='MinimalReproTop#BoxC' (erreichbar=0), part2='MinimalReproTop#BoxD' (erreichbar=0).
Assembly: removeUnconnectedJoints('MinimalReproGrandTop#Assembly') - Joint 'Joint002' entfernt: part1='MinimalReproTop#BoxA' (erreichbar=0), part2='MinimalReproTop#BoxD' (erreichbar=0).
Assembly: removeUnconnectedJoints('MinimalReproGrandTop#Assembly') - Joint 'Joint' entfernt: part1='MinimalReproSub#BoxA' (erreichbar=0), part2='MinimalReproSub#BoxB' (erreichbar=0).
Assembly: 'MinimalReproGrandTop#Assembly' computed (0 joint(s), 2 grounded part(s)):
Assembly:   grounded part: MinimalReproGrandTop#Assembly.Origin
Assembly:   grounded part: MinimalReproGrandTop#Assembly.Assembly001.BoxC
Assembly: Solve of 'MinimalReproGrandTop#Assembly' finished successfully.
```

**Der entscheidende Beleg dafuer, dass der Teilschritt-2-Fix strukturell wirkt:**
`removeUnconnectedJoints()` **prueft jetzt 3 Joints** - vor diesem Patch waere der zwei Ebenen tief
liegende `MinimalReproSub`-Joint 'Joint' (BoxA-BoxB-Slider) an dieser Stelle NIE aufgetaucht, weil
`getJoints()`s alte, kopie-basierte Rekursion nie bis in `Sub`s eigene `JointGroup` vorgedrungen
ist. Er kommt jetzt tatsaechlich bis zu `removeUnconnectedJoints()` durch - **aber genau dort wird
er sofort wieder verworfen**, weil `part1='MinimalReproSub#BoxA'`/`part2='MinimalReproSub#BoxB'`
als `erreichbar=0` eingestuft werden.

**Zusatzbefund, beim Auswerten des Logs entdeckt:** auch `Joint`/`Joint002` (Top's EIGENE,
NICHT tief verschachtelte Joints, nur EINE Ebene unter `GrandTop`) werden als `erreichbar=0`
verworfen! Das war schon VOR dem `resolveJointReference()`-Ansatz und unabhaengig von jeder
Verschachtelungstiefe kaputt - der `getConnectedParts()`/`removeUnconnectedJoints()`-Bug betrifft
also nicht nur den 2-Ebenen-Fall, sondern JEDEN Joint, der innerhalb einer verschachtelten
`AssemblyLink` lebt, sobald `getJoints()` ihn ueberhaupt erst findet.

### Root-Cause-Analyse (Bestaetigung der Vermutung des Nutzers)

Vermutung bestaetigt, mit konkretem Code-Beleg: `getConnectedParts()` (Zeile ~1660) und
`removeUnconnectedJoints()`s eigene Filter-Lambda (Zeile ~1607) rufen beide weiterhin die alte
`getMovingPartFromRef()` auf, um `obj1`/`obj2` fuer einen Joint zu bestimmen - GENAU die Funktion,
die Teilschritt 2 fuer `isMbDJointValid()`/`handleOneSideOfJoint()`/`getRackPinionMarkers()` bereits
durch `resolvePartForMbD()` ersetzt hat, hier aber (wie im Detailplan Schritt 5 bewusst vorgesehen:
"Alle anderen getMovingPartFromRef()-Aufrufstellen ... bleiben bewusst UNVERAENDERT") noch nicht.

Der eigentliche Mechanismus des Mismatches: `getGroundedParts()` (ueber
`AssemblyUtils::getAssemblyComponents()`, `collectComponentsRecursively()`) steigt bereits SEIT
LANGEM (kein Teilschritt-2-Code) korrekt rekursiv durch die LOKALEN `Group`-Listen jeder
verschachtelten flexiblen `AssemblyLink` ab (`asmLink->Group.getValues()`, Zeile ~939 in
`AssemblyUtils.cpp`) - die zurueckgelieferten "geerdeten Teile" sind deshalb IMMER lokale, im
Dokument der loesenden `AssemblyObject`-Instanz lebende Spiegel-Identitaeten (belegt durch das
Log: `MinimalReproGrandTop#Assembly.Assembly001.BoxC` - eindeutig ein `GrandTop`-lokales Objekt,
kein cross-document-Original). `getConnectedParts()`s `obj1`/`obj2` (ueber die alte
`getMovingPartFromRef()`) liefern dagegen fuer einen Joint, der innerhalb einer verschachtelten
`AssemblyLink` LEBT, das ROHE, JOINT-LOKALE (und bei zwei+ Ebenen: cross-document) Original -
belegt durch `MinimalReproTop#BoxC`/`MinimalReproSub#BoxA` in derselben Log-Ausgabe. Zwei
UNTERSCHIEDLICHE Identitaetsraeume treffen in `isObjInSetOfObjRefs()` (reiner Pointer-Vergleich)
aufeinander - der Vergleich schlaegt deshalb IMMER fehl, selbst wenn die Teile physisch
tatsaechlich ueber eine Kette von Joints mit einem geerdeten Teil verbunden sind.

### Teilschritt 2c: Fix umgesetzt

Patch-Datei: `patches/step2c-removeunconnectedjoints-addressing.patch` (baut auf
`step2b-getjoints-nesting-prefix.patch` auf, ebenfalls noch nicht konsolidiert). Betrifft nur
`AssemblyObject.cpp`, zwei Stellen:

1. **`getConnectedParts()`**: `obj1`/`obj2` (die Kanten des Erreichbarkeits-Graphen) werden jetzt
   ueber `resolvePartForMbD()` statt `getMovingPartFromRef()` bestimmt.
2. **`removeUnconnectedJoints()`**s Filter-Lambda: dieselbe Umstellung fuer die abschliessende
   `isObjInSetOfObjRefs()`-Pruefung, aus demselben Grund - beide Seiten des Vergleichs muessen im
   selben (lokalen) Identitaetsraum liegen wie `getGroundedParts()`s Startknoten.

**Warum das sicher ist (dieselbe Absicherung wie bei den bereits umgestellten Funktionen):**
`resolvePartForMbD()` faellt bei einer nicht aufloesbaren Referenz defensiv auf die alte
`getMovingPartFromRef()` zurueck - fuer jeden nicht verschachtelten Fall (bei dem beide ohnehin
dasselbe liefern, siehe Teilschritt-2-Begruendung oben) aendert sich NICHTS. Alle Aufrufer von
`getConnectedParts()`/`traverseAndMarkConnectedParts()` (`removeUnconnectedJoints()`,
`isPartConnected()`, `getDownstreamParts()`) rufen nachweislich VORHER selbst `getJoints()` auf
(Grep-bestaetigt: `isPartConnected()` Zeile ~1725, `getDownstreamParts()` Zeile ~2739) - die dafuer
noetige `jointNestingPrefixMap` ist also in JEDEM Aufrufkontext bereits frisch befuellt, genau wie
bei den in Teilschritt 2 bereits umgestellten Funktionen. Das `.ref`-Feld von `ObjRef` (bleibt
unangetastet die rohe, joint-lokale `PropertyXLinkSub*`-Referenz) wird nirgends im gesamten Modul
gelesen (per `grep` bestaetigt, auch nicht im Python-Binding `getDownstreamParts()` in
`AssemblyObjectPyImp.cpp`) - die Aenderung an `.obj` allein kann diesen ungenutzten Wert also nicht
inkonsistent machen.

**Bewusst NICHT mit angefasst** (gleiche Begruendung wie beim urspruenglichen Teilschritt-2-Scope,
Punkt 5): `getJointOfPartConnectingToGround()`, `isJointConnectingPartToGround()` - beide nutzen
ebenfalls `getMovingPartFromRef()` fuer einen Identitaetsvergleich und koennten prinzipiell demselben
Mismatch unterliegen, sind aber in diesem konkreten Testfall nicht als Ursache aufgetreten und
liegen ausserhalb des durch den GUI-Test konkret belegten Symptoms - falls ein kuenftiger Test zeigt,
dass sie ebenfalls betroffen sind, waeren sie ein eigener, ebenso kleiner naechster Schritt nach
demselben Muster.

### Wie geprueft (headless, gleiche Einschraenkung wie zuvor)

Build sauber (`Assembly` + `AssemblyGui`, gemeinsam deployt in die isolierte Sandbox
`/home/maxx/freecad/install-claude-sandbox`, NIE `/home/maxx/freecad/install`). Regressionscheck am
flachen Fall (`MinimalReproTop.FCStd`): `removeUnconnectedJoints()`-Meldung
(`"1 geerdete(s) Teil(e), 1 erreichbare(s) Teil(e), pruefe 0 Joint(s)"`) byte-identisch vor/nach
diesem Patch. Stresstest (3x frisches Oeffnen, je 5x `solve()`, `Rigid`-Toggle) erneut ohne Crash.
**Wie schon bei Teilschritt 2 selbst kann der eigentliche Wirkmechanismus dieses Fixes headless
NICHT beobachtet werden** - derselbe, bereits dokumentierte Proxy-Ladefehler
(`ModuleNotFoundError: No module named 'PySide6'`) sorgt dafuer, dass `getJoints()` headless
weiterhin "0 joint(s)" liefert, `removeUnconnectedJoints()` bekommt also headless nie echte Joints
zum Pruefen - die Aenderung ist nur strukturell/durch Code-Inspektion abgesichert, siehe
Root-Cause-Analyse oben fuer die vollstaendige Herleitung.

### Naechster konkreter Testschritt (unveraendert derselbe wie zuvor, jetzt mit neuer Erwartung)

Denselben Live-Test wiederholen (frische Kopie von `MinimalReproGrandTop.FCStd`, `BoxB` ziehen,
siehe Anleitung oben). **Neue Erwartung nach Teilschritt 2c:** `removeUnconnectedJoints()` sollte
jetzt melden, dass der tiefe `MinimalReproSub`-Joint 'Joint' (und die beiden Top-Joints) tatsaechlich
`erreichbar=1` sind und NICHT mehr entfernt werden - das waere der Beweis, dass die
Erreichbarkeits-Pruefung jetzt denselben Identitaetsraum wie `isMbDJointValid()` sieht. Haelt der
Slider-Joint beim Ziehen danach immer noch nicht, waere der naechste Verdaechtige
`isMbDJointValid()`/`jointParts()` selbst (ob der Joint dort tatsaechlich ankommt und korrekt in
eine MbD-Randbedingung uebersetzt wird) oder die in Abschnitt 2.5 dokumentierte, separate
Drag-Pfad-Baustelle ("nur die aktive/aeusserste Baugruppe bekommt `preDrag()`/`doDragStep()`") -
ein voller Recompute (`Strg+R`) statt eines interaktiven Drags wuerde diese beiden Faelle
unterscheiden helfen (siehe Anleitung oben, Punkt 6).

## Neuer, vermutlich UNABHAENGIGER Befund (2026-08-29): `BoxB` komplett eingefroren in Subs eigener, flacher Baugruppe

Live-Test (Nutzer): alle drei Dateien gleichzeitig offen, aber NICHT ueber `GrandTop` navigiert -
direkt per Doppelklick in `MinimalReproSub.FCStd`s EIGENE, NICHT verschachtelte Baugruppe
("unterAssambly" ist hier nur das Label des Objekts `Assembly`, keine weitere
Verschachtelungsebene). Solve() VOR jedem Drag-Versuch laeuft sauber (`removeUnconnectedJoints()`
findet den Joint erreichbar, `computed (1 joint(s), 2 grounded part(s))`) - aber `BoxB` reagiert im
3D-Fenster GAR NICHT auf Drag (nicht "frei beweglich" wie beim GrandTop-Befund, sondern komplett
eingefroren), Log bleibt beim Drag-Versuch selbst komplett still (kein Err:/Exception).

**ReadOnly-Hypothese widerlegt** (Nutzer, live per Python-Konsole): `BoxB.getPropertyStatus(
"Placement")` liefert `[23]` - das ist nur `Prop_NoRecompute` (Standard-Typ-Flag jeder
`Placement`-Property), NICHT `ReadOnly`. Placement ist also nicht gesperrt. Mein eigener
Versuch, dasselbe headless nachzustellen, war UNBRAUCHBAR: `FreeCADCmd` zeigt in BEIDEN
Szenarien (Sub allein / alle drei offen) denselben Wert, aber schon der allererste Solve zeigt
"skipped - no grounded part found" - der PySide6-Proxy-Ladefehler verhindert offenbar auch, dass
`GroundedJoint`s eigene Python-Logik (die `Placement.ReadOnly` ueberhaupt erst setzt) headless
jemals laeuft. Der Vergleich war deshalb von vornherein blind - **nicht** verwendbar als Beleg
fuer "kein Unterschied", nur konsistent mit der ohnehin schon bekannten Proxy-Einschraenkung.

**Code-Analyse `ViewProviderAssembly.cpp` (Gui-Layer, komplett ausserhalb von Teilschritt 2/2c -
diese Aenderungen betreffen ausschliesslich `AssemblyObject::solve()`-nahen Code in der App-Schicht,
nicht die Drag-Erkennung):**

- `isInEditMode()` ist schlicht `asmDragger != nullptr`, pro `ViewProviderAssembly`-INSTANZ. Der
  verbundene `signalActivatedViewProvider` (der bei Fremd-Aktivierung `resetEdit()` ausloesen
  koennte) wird ueber `getDocument()->signalActivatedViewProvider` verbunden - `getDocument()` ist
  hier das EIGENE `Gui::Document` (Sub), NICHT applikationsweit. Cross-Document-Kontamination durch
  GrandTop/Top ueber dieses Signal **ausgeschlossen** - jedes Dokument hat sein eigenes Signal.
- `tryMouseMove()` (Zeile 486-492): `if (canStartDragging) { ... if (enableMovement &&
  getSelectedObjectsWithinAssembly()) { initMove(...); } }` - ist `enableMovement` (Property
  `ViewObject.EnableMovement`, Default `true`) `false`, wird `initMove()` NIE aufgerufen -
  **exakt** das beobachtete Symptom: komplett stumm, keine Log-Zeile, keine Exception, kein
  Drag-Start.
- `EnableMovement` wird im GESAMTEN `freecad-source`-Baum nur an EINER Stelle umgeschaltet:
  `CommandCreateView.py` (das "Bauteil auseinanderziehen"/Exploded-View-Task-Panel),
  `TaskAssemblyCreateView.__init__()` Zeile 751 (`EnableMovement = False` beim Oeffnen) und
  `deactivate()` Zeile 851 (`EnableMovement = True`, aufgerufen aus SOWOHL `accept()` als auch
  `reject()`). Wurde dieses Task-Panel fuer GENAU DIESE Baugruppe irgendwann in der Sitzung
  geoeffnet und NICHT ueber `accept()`/`reject()` sauber verlassen (Absturz, erzwungenes
  Dialog-Schliessen, Dokumentwechsel waehrend offenem Panel), bliebe `EnableMovement` fuer den
  Rest der Sitzung dauerhaft `false` haengen - unabhaengig von Teilschritt 2/2c, ein reiner
  Python-Task-Panel-Bug (falls ueberhaupt die Ursache - nicht verifiziert, nur strukturell
  hergeleitet).
- Auch strukturell geprueft: `findDragMode()`s einziger `DragMode::None`-Rueckgabepunkt braucht
  `jointType == JointType::Fixed` (Zeile 963-965) - der hier verwendete Joint ist ein SLIDER,
  dieser Pfad greift also nicht. Ein "kompletter Stillstand" passt eher zu `enableMovement=false`
  oder zu `docsToMove` bleibt leer (heisst: `canDragObjectIn3d(BoxB)` liefert `false` oder die
  Selektion findet BoxB gar nicht) als zu einem erreichten, aber falsch ausgewerteten `DragMode`.

**Konkreter, sofortiger (risikofreier) Test-Vorschlag fuer die naechste Live-Sitzung, OHNE
Neustart, direkt im noch offenen, eingefrorenen Zustand:**

1. Python-Konsole: `Gui.ActiveDocument.getObject("Assembly").EnableMovement` (Name des
   `Assembly`-Objekts ggf. anpassen, siehe Log: `MinimalReproSub#Assembly`) - zeigt das `False`?
2. Falls ja: `Gui.ActiveDocument.getObject("Assembly").EnableMovement = True` setzen und SOFORT
   erneut versuchen, `BoxB` zu ziehen (kein Reload noetig) - haelt/bewegt es sich jetzt?
3. Unabhaengig vom Ergebnis: der bereits vorgeschlagene Isolationstest (NUR
   `MinimalReproSub.FCStd` oeffnen, ohne GrandTop/Top) bleibt wertvoll, um zu klaeren, ob das
   Verhalten wirklich am gleichzeitigen Offenhalten der drei Dokumente haengt oder an einem
   Sitzungs-Zustand (wie einem haengengebliebenen Task-Panel), der zufaellig in dieser Sitzung
   vorher entstanden ist.

**Einschaetzung:** falls Punkt 1/2 den Fehler bestaetigt/behebt, ist das ein voellig eigenstaendiger,
von Teilschritt 2/2c unabhaengiger Python-Bug (Task-Panel-Cleanup) - kein Rueckschlag fuer die
Solver-Root-Cause-Arbeit, nur ein separater, paralleler Fund. Falls NICHT, bleibt der urspruengliche
Verdacht (Multi-Dokument-Interaktion) bestehen und muesste tiefer im Drag-Code (`getSelectedObjectsWithinAssembly()`/`canDragObjectIn3d()`) verfolgt werden - dafuer waere ein
Live-Test mit gezielten Zwischenschritt-Ausgaben (temporaeres zusaetzliches Logging in
`tryMouseMove()`) der naechste sinnvolle Schritt.

## EnableMovement-Hypothese widerlegt, jointNestingPrefixMap aktiv geprueft, temporaeres Drag-Diagnose-Logging (2026-08-29)

**`EnableMovement` live gegengeprueft (Nutzer): `True`** - die Exploded-View-Panel-Hypothese ist
damit widerlegt, genau wie zuvor schon ReadOnly. Zwei plausible Kandidaten weniger.

### Aktive Ueberpruefung: kann `jointNestingPrefixMap` (Teilschritt 2) selbst einen fruehzeitigen Abbruch im Drag-Pfad verursachen?

Explizit angefordert vom Nutzer - Ergebnis: **strukturell ausgeschlossen**, mit zwei voneinander
unabhaengigen Begruendungen:

1. **Jeder Aufrufer von `resolvePartForMbD()`/`getConnectedParts()` im Drag-Pfad ruft nachweislich
   selbst vorher `getJoints()` bzw. `solve()` auf, im SELBEN Funktionsrumpf, ohne Zwischenaufruf,
   der die Map leeren koennte:**
   - `preDrag()` (Zeile 495-499): `bundleFixed = true; solve(); bundleFixed = false;` **ALS
     ALLERERSTES**, noch VOR dem `isPartConnected(part)`-Aufruf in derselben Funktion (Zeile 511) -
     `solve()` durchlaeuft `getJoints()` und befuellt `jointNestingPrefixMap` also garantiert
     frisch, bevor irgendetwas in `preDrag()` selbst davon liest.
   - `getDownstreamParts()` (aufgerufen aus `findDragMode()`) ruft selbst `getJoints()` (Zeile
     ~2739) unmittelbar vor `traverseAndMarkConnectedParts()`/`getConnectedParts()` auf.
   - `isPartConnected()` ruft ebenfalls selbst `getJoints()` (Zeile ~1725) unmittelbar vorher auf.
   - `getJointOfPartConnectingToGround()` (ebenfalls von `findDragMode()` genutzt) nutzt
     `resolvePartForMbD()`/`jointNestingPrefixMap` **gar nicht** - unveraendert die alte
     `getMovingPartFromRef()`, komplett unberuehrt von Teilschritt 2/2c.
2. **Selbst FALLS die Map an irgendeiner Stelle doch leer/veraltet waere, waere das Ergebnis fuer
   genau diesen Testfall (flache, NICHT verschachtelte Baugruppe) trotzdem identisch:**
   `resolvePartForMbD()` faellt bei einem Map-Miss auf `nestingPrefix = ""` zurueck (Default-Wert
   der lokalen `std::string`) - und `""` ist fuer einen Joint, der DIREKT in der gerade
   editierten, nicht verschachtelten `unterAssambly`/`Assembly`-Instanz lebt, ohnehin der
   OBJEKTIV KORREKTE Praefix (kein Verschachtelungs-Segment vorhanden). `resolveJointReference(
   this, joint, ref, "")` liefert fuer einen solchen Joint exakt dasselbe Objekt wie die alte
   `getMovingPartFromRef()` (beide loesen `refObj = prop->getValue()` auf, siehe Herleitung in
   Abschnitt "Teilschritt 2 - Umsetzung" oben). Und selbst wenn `resolveJointReference()` aus
   irgendeinem unvorhergesehenen Grund fehlschluege, faengt der eingebaute defensive Ruecksfall in
   `resolvePartForMbD()` das exakt mit der alten Funktion ab. Es gibt also **keinen** Codepfad, auf
   dem `resolvePartForMbD()` fuer einen flachen Joint etwas ANDERES liefern koennte als die alte,
   unveraenderte `getMovingPartFromRef()` - unabhaengig vom Zustand der Map.

**"Vorher/Nachher"-Vergleich per Diff statt per Neu-Checkout** (aussagekraeftiger als ein erneuter
Build-Vergleich, weil er nicht von Sitzungszustand/Zufaelligkeiten abhaengt): `cat
patches/step2b-getjoints-nesting-prefix.patch patches/step2c-removeunconnectedjoints-addressing.patch
| grep "^+++ b/"` zeigt, dass Teilschritt 2/2c **ausschliesslich** `AssemblyLink.cpp`,
`AssemblyObject.cpp` und `AssemblyObject.h` beruehrt - **null** Treffer fuer
`ViewProviderAssembly.cpp` (grep-verifiziert, `grep -c "ViewProviderAssembly"` liefert `0`). Die
GESAMTE Drag-Erkennung/-Initiierung (`getSelectedObjectsWithinAssembly()`, `canDragObjectIn3d()`,
`findDragMode()`, `tryInitMove()`, `tryMouseMove()`, `mouseButtonPressed()`) ist also
BUCHSTAEBLICH keine einzige Zeile durch Teilschritt 2/2c veraendert worden. Ein Vergleich mit dem
Commit vor Teilschritt 2 wuerde in genau diesem Code zwangslaeufig IDENTISCHEN Text zeigen - ein
tatsaechlicher Neu-Build+Test waere also keine zusaetzliche Erkenntnis gegenueber diesem
Diff-Beleg, nur derselbe Beweis auf umstaendlicherem Weg.

**Fazit:** Teilschritt 2/2c kann fuer DIESEN (flachen, nicht verschachtelten) Testfall die
beobachtete Drag-Blockade nachweislich nicht verursacht haben - weder ueber
`jointNestingPrefixMap`-Zustand noch ueber irgendeine andere Aenderung, weil der komplette
Drag-Erkennungscode unangetastet ist.

### Weitere strukturell ueberprufte Kandidaten (Nutzer-Vorschlag)

- **`findDragMode()`s einziger `DragMode::None`-Rueckgabepunkt** (Zeile 963-965 in der aktuellen
  Fassung) ist an `jointType == JointType::Fixed` mit fehlendem Upstream-Teil gebunden - der Joint
  hier ist ein SLIDER, dieser Pfad kann also nicht getroffen werden. Alle anderen Codepfade der
  Funktion enden entweder in einem konkreten `DragMode` (Revolute/Slider/Cylindrical/Ball/...) oder
  im finalen Fallback `return DragMode::Translation;` (NICHT `None`) - `DragMode::None` hat in der
  gesamten Funktion nachweislich nur den einen Rueckgabepunkt.
- **`docsToMove` bleibt leer**: dafuer muesste `getSelectedObjectsWithinAssembly()` (genauer: der
  `addPreselection`-Zweig, da beim reinen Klick-und-Ziehen typischerweise noch keine echte
  Selektion, nur eine Preselection vorliegt) entweder keine Preselection finden, oder
  `getMovingPartFromSel()` liefert `nullptr`, oder `canDragObjectIn3d()` lehnt das Ergebnis ab.
  Genau das macht das neue temporaere Logging unten sichtbar.

### Temporaeres Diagnose-Logging ergaenzt (nur fuer den naechsten Live-Test, danach wieder entfernen)

Patch: `patches/step2d-TEMP-drag-diagnose-logging.patch` (betrifft ausschliesslich
`Gui/ViewProviderAssembly.cpp` - **bewusst als eigene, klar als TEMPORAER markierte Patch-Datei**,
nicht mit step2b/step2c vermischt, damit sie nach der Diagnose isoliert wieder entfernt werden
kann). Reines `Base::Console().message()`-Logging, KEINE Kontrollfluss-/Verhaltensaenderung:

- `canDragObjectIn3d()`: loggt an JEDEM der vier fruehen Rueckgabe-Punkte den genauen Grund
  ("keine eigene Kind-Beziehung", "keine Placement-Property", "ist selbst ein GroundedJoint",
  "isPartGrounded() sagt geerdet") sowie den Erfolgsfall.
- `getSelectedObjectsWithinAssembly()`: loggt, ob ueberhaupt eine Preselection vorliegt, und falls
  ja, was `getMovingPartFromSel()` daraus aufloest (bevor `canDragObjectIn3d()` darueber
  entscheidet).
- `tryMouseMove()`: loggt beim Drag-Start-Versuch (einmal pro Klick) `enableMovement` und das
  Ergebnis von `getSelectedObjectsWithinAssembly()`.

**Performance-Sicherheit gepruft**: alle drei Stellen feuern nur EINMAL PRO MAUSKLICK (gated durch
`tryMouseMove()`s `canStartDragging`-Flag, das direkt danach auf `false` gesetzt wird) - NICHT bei
jedem Mausbewegungsereignis. Kein CPU-Risiko wie beim urspruenglichen, unbedingten Fix-16-Logging.
Headless verifiziert: ein normaler `solve()`-Lauf (`FreeCADCmd`, kein Gui-Mausereignis moeglich)
erzeugt `grep -c "Assembly-Diagnose"` = `0` Treffer - die neue Logging-Instrumentierung ist beim
reinen Solve-Pfad vollstaendig inaktiv, genau wie beabsichtigt. Build sauber (`AssemblyGui`-Target),
in der isolierten Sandbox deployt und verifiziert (keine undefined symbols, `/home/maxx/freecad/
install` unberuehrt), Stresstest ohne Crash.

**Naechster Live-Test:** denselben Drag-Versuch in `MinimalReproSub.FCStd`s eigener Baugruppe
wiederholen (sobald die Sandbox-Version genutzt wird) - das Report-View sollte jetzt beim
Drag-Start-Versuch mehrere `Assembly-Diagnose:`-Zeilen zeigen, die genau erklaeren, an welcher
Stelle (falls ueberhaupt) BoxB verworfen wird, oder ob der Prozess ueberhaupt bis
`canDragObjectIn3d()` vordringt. Nach der Diagnose: `patches/step2d-TEMP-drag-diagnose-logging.patch`
wieder aus dem Arbeitsbaum entfernen (`git apply -R` bzw. die drei Stellen manuell zuruecksetzen),
es ist NICHT fuer eine dauerhafte Aufnahme in die konsolidierten Patches gedacht.

## Live-Test-Befund (2026-08-29, echte Installation mit Diagnose-Build): Rubberband statt Drag, Instrumentierung NICHT erreicht

Live-Test durchgefuehrt: `unterAssambly` per Doppelklick aktiv im Edit-Modus, nah an `BoxB`
herangezoomt, praezise auf den sichtbaren Wuerfel geklickt und gezogen.

**Ergebnis: beim Ziehen erscheint ein Auswahlrechteck (Rubberband-Selektion) statt der erwarteten
Objektbewegung** - so, als waere der Klick ins Leere gegangen, obwohl er visuell direkt auf dem
Wuerfel lag. **Im Log erscheint dabei KEINE einzige `Assembly-Diagnose:`-Zeile**, trotz mehrfachem
Versuch - die gesamte Instrumentierung aus `tryMouseMove()`/`getSelectedObjectsWithinAssembly()`/
`canDragObjectIn3d()` wird also gar nicht erreicht.

**Zwei bereits durch den Nutzer live widerlegte Nebenhypothesen:**
- `BoxB.ViewObject.Selectable` -> `True` (nicht die Ursache).
- (aus der vorigen Session bereits widerlegt: `EnableMovement` -> `True`, `Placement`-ReadOnly
  nicht gesetzt.)

**Einordnung:** dass die Instrumentierung komplett schweigt, obwohl sie an drei verschiedenen,
tief im Assembly-eigenen Drag-Pfad liegenden Stellen sitzt, spricht dafuer, dass die
Rubberband-vs-Objekt-Entscheidung VOR diesen drei Funktionen faellt - vermutlich schon beim
allerersten Mausklick-Event, respektive ob dieser ueberhaupt einen Pick-Treffer auf `BoxB`s Shape
erzielt. Als naechster, noch nicht durch einen Live-Test bestaetigter Schritt wurde zusaetzliche
Instrumentierung ganz am Anfang von `ViewProviderAssembly::mouseButtonPressed()` ergaenzt (VOR dem
bestehenden `isInEditMode()`-Check) - das ist der fruehestmoegliche Punkt innerhalb der
Assembly-eigenen `ViewProvider`-Klasse selbst; `ViewProvider::eventCallback()`
(`Gui/ViewProvider.cpp`, FreeCAD-Kern) ruft `self->mouseButtonPressed(...)` bei JEDEM
Maustasten-Event auf (bestaetigt durch Code-Lesen), unabhaengig davon, ob Coin3D einen Pick-Treffer
gefunden hat - die neue Logzeile zeigt also, ob diese Funktion ueberhaupt erreicht wird und was
`isInEditMode()`/`getDraggerVisibility()` in dem Moment liefern. **Noch nicht live getestet** -
das ist der naechste konkrete Schritt fuer die kommende Sitzung.

Patch aktualisiert: `patches/step2d-TEMP-drag-diagnose-logging.patch` (weiterhin ausschliesslich
`Gui/ViewProviderAssembly.cpp`, weiterhin bewusst als TEMPORAER markiert). Headless erneut
verifiziert (Build sauber, `strings` bestaetigt die neue Zeichenkette im Binary, normaler
`solve()`-Lauf zeigt weiterhin 0 Diagnose-Treffer). **In der Sandbox deployt, NICHT in der echten
Installation** - die echte Installation wurde vom Nutzer nach dem letzten Testlauf bereits wieder
exakt auf den Vor-Diagnose-Stand zurueckgesetzt (per `md5sum` gegen das Backup verifiziert) - fuer
den naechsten Test muss der aktualisierte Diagnose-Build (mit der neuen
`mouseButtonPressed()`-Instrumentierung) erst erneut kontrolliert eingespielt werden (siehe
"Betriebs-Lektion" oben fuer das genaue Vorgehen: immer beide `.so` zusammen, danach wieder
zuruecksetzen).

**Falls dieser naechste Test ebenfalls zeigt, dass `mouseButtonPressed()` gar nicht erreicht wird**
(gar keine `Assembly-Diagnose:`-Zeile mehr, auch nicht die neue): dann liegt die Ursache
nachweislich VOR jeder Assembly-eigenen `ViewProvider`-Logik, in FreeCADs generischem
Maus-/Editier-Event-Routing (`Gui/View3DInventorViewer.cpp`/`Gui/NavigationStyle.cpp`/
`Gui::ViewProvider::eventCallback()` selbst) oder sogar noch frueher beim Pick-Test von Coin3D
gegen `BoxB`s Shape/BoundBox - eine Untersuchung, die dann genau dort (FreeCAD-Kern, nicht mehr
Assembly-Modul) ansetzen muesste. **Falls `mouseButtonPressed()` dagegen erreicht wird** (Zeile
erscheint), aber `isInEditMode()` unerwartet `false` liefert: dann waere das eigentliche Problem,
dass der Edit-Modus zwischen Doppelklick-Aktivierung und dem eigentlichen Ziehversuch schon wieder
verloren geht (z.B. durch ein unerwuenschtes `resetEdit()`, siehe `slotActivatedVP()`) - ein
Kandidat, der bereits einmal (fuer Cross-Document-Signale) untersucht und ausgeschlossen wurde, aber
fuer einen ANDEREN Ausloeser erneut in Frage kaeme.

## Sandbox-3D-View-Untersuchung: Stand 2026-08-29 (Ende der Sitzung)

Eigener, von der urspruenglichen Drag/Joint-Frage GETRENNTER Nebenstrang: die Sandbox-Installation
(`/home/maxx/freecad/install-claude-sandbox`) zeigt eine komplett tote 3D-Ansicht (keine Geometrie
sichtbar, keine Interaktion), obwohl Baum/Menues/Symbolleisten normal funktionieren. Blockiert
JEDEN Live-Drag-Test in der Sandbox - deshalb musste fuer die Teilschritt-2c-Live-Tests weiter oben
die kontrollierte Ausnahme (temporaerer, sofort zurueckgesetzter Deploy in die echte Installation)
genutzt werden.

### Zwei Zwischenfaelle bei der Untersuchung (beide behoben, siehe unten fuer die Konsequenzen)

1. Ein Testlauf hat versehentlich `~/.config/FreeCAD/v26-3/user.cfg` (geteilt zwischen Sandbox UND
   der taeglich genutzten echten Installation) ueberschrieben - die komplette
   Fensteranordnung/Paneel-Layout des Nutzers ging verloren, nicht wiederherstellbar (kein Backup
   vorhanden), musste manuell neu eingerichtet werden. Ursache: KEIN bisheriges Start-Skript
   (weder `run-freecad-sandbox.sh` noch der Pristine-Test) isolierte `HOME`/`XDG_CONFIG_HOME` -
   beide Installationen teilten sich exakt dieselbe Konfigurationsdatei.
2. Beim Beheben von Zwischenfall 1 mit einem eigenen isolierten Testprofil wurde außerdem
   versehentlich `/home/maxx/freecad/install/lib/{AssemblyApp,AssemblyGui}.so` mit dem
   Diagnose-Build ueberschrieben (temporaere Ausnahme fuer einen Live-Test, siehe Teilschritt-2c-
   Abschnitt oben) und musste explizit wieder auf den Teilschritt-2c-Stand (ohne Diagnose-Code)
   zurueckgesetzt werden. **Seitdem NEUE, unveraenderliche Regel: `/home/maxx/freecad/install` wird
   vom Agenten AB SOFORT NIE WIEDER angefasst, auch nicht temporaer/kontrolliert mit Backup - keine
   Ausnahmen mehr.** Am Sitzungsende erneut verifiziert: `AssemblyApp.so`/`AssemblyGui.so` in der
   echten Installation unveraendert seit der Wiederherstellung (Timestamp `1788007339` fuer beide,
   `0` Diagnose-Strings, `resolvePartForMbD` mit 2 Fundstellen weiterhin vorhanden - exakt der
   Teilschritt-2c-Stand ohne jedes Diagnose-Logging).

### Isolationsmechanismus gebaut und verifiziert (Reaktion auf Zwischenfall 1)

`FREECAD_USER_HOME` (von FreeCAD selbst unterstuetzte Umgebungsvariable, siehe
`App::Application::getCustomPaths()`/`ExtractUserPath()` in `src/App/Application.cpp`) hat
nachweislich Vorrang vor den normalen XDG-Pfaden. **Wichtige, per Code-Lesen gefundene Falle:**
das Zielverzeichnis MUSS vorher schon existieren (`toNativePath()` prueft `dir.exists()`), sonst
wird der Wert STILLSCHWEIGEND verworfen und faellt zurueck auf den echten, geteilten Pfad - deshalb
im neuen Skript IMMER zuerst `mkdir -p`, danach erst `FREECAD_USER_HOME` setzen. Zusaetzlich
bestaetigt: wird NUR `FREECAD_USER_HOME` gesetzt, leitet FreeCAD automatisch
`FREECAD_USER_TEMP=<HOME>/temp` her, und genau dieser Wert deckt AUCH den Cache-Pfad ab
(`findPath(cacheHome, customTemp, ...)`) - eine separate Cache-Isolation ist NICHT noetig, mein
Mechanismus deckt Config+Data+Cache in einem Schritt ab.

**Verifiziert (headless, Timestamp-Vergleich vorher/nachher):** `App.ConfigGet('UserAppData')`/
`'UserConfigPath'`/`'UserCachePath'` zeigen mit gesetztem `FREECAD_USER_HOME` alle korrekt auf das
isolierte Profil; `~/.config/FreeCAD/v26-3/user.cfg` UND `~/.local/share/FreeCAD/v26-3` bleiben
dabei mit exakt unveraendertem Timestamp - keine Beruehrung der echten, geteilten Config/Data/Cache.

Neues, wiederverwendbares Skript im Scratchpad: `run-freecad-isolated.sh` (setzt `FC_BIN` per Env-
Override, Default zeigt auf die Sandbox-Installation, niemals auf `/home/maxx/freecad/install`).
**Ab sofort fuer JEDEN Testlauf (GUI-Live-Tests genauso wie eigene headless-FreeCADCmd-Skripte)
verpflichtend.**

**Geklaert (kein neuer Zwischenfall):** `~/.config/FreeCAD/v26-3/user.cfg` hatte sich nach der
Wiederherstellung noch einmal veraendert (Timestamp `1788009690`). Ursache: die letzten drei Checks
(NaviCube/Part_Box/Resize) liefen ueber das GEPINNTE Sandbox-Icon in der GNOME-Startleiste, das auf
das AELTERE, NICHT-isolierte `resources/run-freecad-sandbox.sh` zeigt (von der Hauptsitzung heute
frueh auf `main` gebaut, VOR meiner `run-freecad-isolated.sh`-Loesung) - kein neuer Fehler, nur ein
zu diesem Zeitpunkt noch veraltetes Icon.

**To-Do fuer eine kuenftige Sitzung (Aenderung auf `main`, NICHT auf `solver-root-cause-fix` - notiert,
nicht selbst umgesetzt):** `resources/run-freecad-sandbox.sh` (bzw. das gepinnte Desktop-Icon) sollte
auf denselben `FREECAD_USER_HOME`-Isolationsmechanismus wie `run-freecad-isolated.sh` umgestellt
werden, damit das bequeme, gepinnte Icon nicht erneut versehentlich die geteilte Config beschaedigen
kann. Zustaendig: die Hauptsitzung (arbeitet auf `main`).

### Ausgeschlossene Hypothesen fuer die tote 3D-Ansicht (alle live oder per Code/Log widerlegt)

Chronologisch, jede einzeln mit einem konkreten Test/Beleg widerlegt:

1. **`Placement`-`ReadOnly`** - `BoxB.getPropertyStatus("Placement")` liefert nur `Prop_NoRecompute`
   (Standard-Flag), keine Sperre.
2. **`EnableMovement`** (ViewObject-Property) - live `True` bestaetigt.
3. **`jointNestingPrefixMap`-Staleness** (Teilschritt 2, eigener Code) - strukturell ausgeschlossen:
   jeder Aufrufer im Drag-Pfad ruft nachweislich selbst vorher `getJoints()`/`solve()` auf; selbst
   bei einem hypothetischen Map-Miss waere das Ergebnis fuer nicht verschachtelte Faelle identisch
   zur alten Funktion (siehe Abschnitt weiter oben fuer die volle Herleitung).
4. **RUNPATH/Installationspfad** - eine BRANDNEUE, garantiert 0% agenteneigene Aenderungen
   enthaltende Kopie der echten Installation (`install-claude-sandbox-pristine-test`) zeigt
   IDENTISCH dieselbe tote 3D-Ansicht - beweist zweifelsfrei, dass es NICHTS mit Assembly-Code oder
   Agenten-Commits zu tun hat.
5. **Beschaedigte geteilte Config** - mit einem BRANDNEUEN, garantiert unkorrumpierten, isolierten
   `FREECAD_USER_HOME`-Profil (siehe oben) zeigt sich dieselbe tote 3D-Ansicht - widerlegt.
6. **`Selectable`** (ViewObject-Property von `BoxB`) - live `True` bestaetigt (im Rahmen des
   Rubberband-statt-Drag-Befundes, siehe Abschnitt weiter oben).
7. **Layout-/Widget-Groessenproblem** - `Gui.activeDocument().activeView().getViewer().
   getSoRenderManager().getViewportRegion().getViewportSizePixels()` liefert eine plausible,
   nicht-degenerierte Groesse; Fenster-Resize (verkleinern + maximieren) aendert nichts am toten
   Zustand.
8. **Kamera-/Clipping-Problem** - `getCameraNode()`s Position/Orientierung/near-far-Werte sehen
   nach Live-Check normal/nicht-degeneriert aus (keine NaN/Null/vertauschte Werte).
9. **Assembly-spezifisch** - ausgeschlossen: ein KOMPLETT VANILLA neues Dokument (Part-Werkbench,
   einfacher `Part::Box`, kein Assembly-Bezug) zeigt exakt dasselbe Bild.

**Verbleibender, gut eingegrenzter Befund:** das GL-Overlay-Rendering (NaviCube, oben rechts in der
3D-Ecke) funktioniert einwandfrei und normal. Die eigentliche SZENEN-Geometrie (Hauptrenderpass)
bleibt dagegen in JEDEM Dokument (Assembly wie auch simpler `Part::Box`) unsichtbar, unabhaengig
von Fenstergroesse/Resize. Das deutet auf ein Problem spezifisch im Haupt-Szenengraph-Renderpass
(getrennt vom Overlay-Renderpass) hin - z.B. der Scenegraph-Root wird nicht traversiert, oder der
Redraw-Callback fuer den Hauptpass ist nicht korrekt verbunden. Reine Log-/Property-/Config-basierte
Diagnose ist an dieser Stelle ausgereizt (auch `strace`-Vergleich der Datei-Zugriffe zwischen echter
Installation und Sandbox unter `QT_QPA_PLATFORM=offscreen` zeigte keine strukturellen Unterschiede -
`offscreen` umgeht aber vermutlich ohnehin die eigentliche GLX-Fensterinitialisierung komplett und
kann den Bug strukturell nicht exercisen).

### Naechster Schritt: Xvfb-basierte visuelle Selbstverifikation (vom Nutzer genehmigt, NOCH NICHT umgesetzt)

Ziel: ein virtuelles, unsichtbares X-Display (`Xvfb`) einrichten, damit der Agent selbststaendig
echte GLX-Fenster starten und per Screenshot visuell pruefen kann, ob Geometrie sichtbar ist - ohne
fuer jede Diagnose-Iteration einen Live-Test des Nutzers zu brauchen.

**Status:** `sudo apt-get install -y xvfb` scheitert non-interaktiv ("sudo: ein Terminal ist
erforderlich, um das Passwort zu lesen") - der Agent hat kein Passwort/keinen interaktiven
sudo-Zugang. **Muss vom Nutzer selbst ausgefuehrt werden** (`sudo apt-get install xvfb`), oder der
Agent braucht einen anderen Weg an sudo-Rechte (z.B. eine NOPASSWD-Sudoers-Regel fuer genau diesen
einen Befehl, falls gewuenscht - keine eigene Entscheidung des Agenten, nur als Option genannt).

**Sobald Xvfb verfuegbar ist, geplantes Vorgehen (noch nicht umgesetzt):**
1. `Xvfb :99 -screen 0 1280x1024x24 &` (oder `xvfb-run`) starten.
2. `DISPLAY=:99 QT_QPA_PLATFORM=xcb` (NICHT `offscreen`, das umgeht ja gerade den Bug) + das
   isolierte `run-freecad-isolated.sh`-Profil kombinieren, ein Dokument mit einfacher Geometrie
   oeffnen.
3. Screenshot ziehen (z.B. `import`/ImageMagick oder Qt's eigene Screenshot-Funktion per Python:
   `Gui.activeDocument().activeView().grabFramebuffer()` o.ae., falls verfuegbar) und selbst per
   `Read`-Tool visuell inspizieren.
4. Falls die Sandbox unter Xvfb ebenfalls tot ist: gezielt Coin3D/`View3DInventorViewer`-Quellcode
   fuer den Haupt-Renderpass durchgehen (z.B. `SoQtViewer`/`Quarter::QuarterWidget`-Konstruktion,
   `SoSceneManager`-Verbindung), jetzt mit der Moeglichkeit, jede Codeaenderung selbst per
   Screenshot zu verifizieren, statt weiter blind Live-Tests zu brauchen.

## Xvfb-Durchbruch (2026-08-29, spaeter Sitzungsteil): Bug ist real, reproduzierbar, und liegt NICHT an Dateiinhalt

**WICHTIG fuer eine neue Sitzung/einen neuen Agenten, der ohne die Chat-Historie dieser Sitzung
startet: dieser Abschnitt fasst ALLES zusammen, was noetig ist, um genau hier weiterzumachen.**
Xvfb wurde vom Nutzer installiert (`sudo apt-get install -y xvfb`, erledigt). Der Agent kann sich
seitdem SELBST einen echten, visuell ueberpruefbaren 3D-Rendering-Test bauen, OHNE einen Live-Test
durch den Nutzer zu brauchen.

### Der (korrekte!) Xvfb-Testaufbau, Schritt fuer Schritt

**1. Xvfb-Server starten** (einmalig, laeuft im Hintergrund weiter):
```bash
Xvfb :97 -screen 0 1280x1024x24 +extension GLX +render -noreset &
# kurz warten, dann pruefen:
DISPLAY=:97 glxinfo | grep "direct rendering"   # sollte "Yes" zeigen
```
Zum Zeitpunkt des Sitzungsendes lief dieser Xvfb-Prozess noch (PID war 47221, Display `:97`) -
falls er nicht mehr laeuft, obiger Befehl startet ihn neu (Display-Nummer `:97` ist beliebig
waehlbar, muss nur frei sein).

**2. WICHTIGE, mit viel Aufwand herausgefundene Screenshot-Methodik-Falle:** `QWidget.grab()`
(z.B. `Gui.getMainWindow().grab()`) zeigt bei JEDER Installation (auch der nachweislich
funktionierenden echten!) eine KOMPLETT SCHWARZE 3D-Ansicht - das ist ein Artefakt von Qt's
`grab()`, das nur die eigene Qt-Widget-Compositing-Pipeline rendert, aber nativ per GLX
gezeichneten Inhalt NICHT sieht. **Der korrekte Weg ist `QScreen.grabWindow(0)`** (liest die
ECHTEN Pixel direkt vom X-Server, sieht dadurch auch GLX-Inhalt):
```python
from PySide6.QtWidgets import QApplication
screen = QApplication.instance().primaryScreen()
pix = screen.grabWindow(0)   # 0 = Root-Fenster = ganzer Bildschirm
pix.save("/pfad/zu/screenshot.png")
```

**3. Funktionierendes, vollstaendiges Testskript** (Pfad war
`.../scratchpad/step2b_test/test_screenshot5.py`, ACHTUNG: das Scratchpad-Verzeichnis wird
zwischen Sitzungen geleert - der komplette Skriptinhalt steht deshalb hier, damit er jederzeit neu
angelegt werden kann):
```python
import sys
import time
import FreeCAD as App
import FreeCADGui as Gui


def log(msg):
    print(msg, flush=True)


log("STEP1: before showMainWindow")
Gui.showMainWindow()
mw = Gui.getMainWindow()

doc = App.newDocument("ScreenshotTest")
box = doc.addObject("Part::Box", "Box")
doc.recompute()

Gui.ActiveDocument = Gui.getDocument(doc.Name)
adoc = Gui.activeDocument()
view = adoc.activeView()
log(f"STEP: view={view}")

from PySide6.QtWidgets import QApplication, QMdiArea
qapp = QApplication.instance()
for i in range(10):
    qapp.processEvents()

# WICHTIG: die eigentliche Dokument-Tab wird nach newDocument() NICHT automatisch in den
# Vordergrund geholt (der "Start"-Willkommens-Tab bleibt sichtbar) - ohne diesen Schritt zeigt
# der Screenshot faelschlich den Start-Tab statt der 3D-Ansicht:
mdi_areas = mw.findChildren(QMdiArea)
for area in mdi_areas:
    for sub in area.subWindowList():
        if "ScreenshotTest" in sub.windowTitle():
            area.setActiveSubWindow(sub)
            sub.showMaximized()
            sub.raise_()

for i in range(40):
    qapp.processEvents()
    time.sleep(0.05)

view.viewIsometric()
view.fitAll()

for i in range(40):
    qapp.processEvents()
    time.sleep(0.05)

# QScreen.grabWindow(0), NICHT mw.grab() - siehe Begruendung oben!
screen = qapp.primaryScreen()
pix = screen.grabWindow(0)
out_path = "/pfad/zu/screenshot.png"   # <- anpassen
ok = pix.save(out_path)
log(f"saved={ok} path={out_path} size={pix.size().width()}x{pix.size().height()}")

log("DONE-SCREENSHOT-TEST")
sys.exit(0)
```

**4. Aufruf** (Beispiel fuer die Sandbox-Installation, analog fuer andere Installationspfade -
`FC_BIN` einfach austauschen):
```bash
ISOLATED_PROFILE=/pfad/zu/einem/leeren/Ordner   # z.B. im Scratchpad
mkdir -p "$ISOLATED_PROFILE"
source /home/maxx/Dokumente/FreeCAD-Development/.venv/bin/activate
export VIRTUAL_ENV="/home/maxx/Dokumente/FreeCAD-Development/.venv"
PYSIDE_QT="${VIRTUAL_ENV}/lib/python3.12/site-packages/PySide6/Qt"
export QT_PLUGIN_PATH="${PYSIDE_QT}/plugins"
export LD_LIBRARY_PATH="${PYSIDE_QT}/lib:/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONNOUSERSITE=1
export PYTHONPATH="${VIRTUAL_ENV}/lib/python3.12/site-packages"
export QT_QPA_PLATFORM=xcb        # NICHT offscreen - das wuerde die eigentliche GLX-Fenster-
                                   # initialisierung komplett umgehen und den Bug nicht zeigen
export DISPLAY=:97
export FREECAD_USER_HOME="$ISOLATED_PROFILE"   # WICHTIG: Ordner muss VOR dem Start existieren,
                                                # sonst wird der Wert von FreeCAD stillschweigend
                                                # verworfen (siehe Isolationsmechanismus oben)
export LIBGL_ALWAYS_SOFTWARE=1     # erzwingt Mesa-Software-Rendering, robuster unter Xvfb

timeout 60 /home/maxx/freecad/install-claude-sandbox/bin/FreeCAD --hidden test_screenshot5.py
```
Wichtig: `mw.show()` NICHT unbedingt noetig fuer den grabWindow()-Test (`Gui.showMainWindow()`
allein reicht) - ein zusaetzliches `mw.resize()+mw.show()` OHNE Fenstermanager in Xvfb hat in
einem frueheren Versuch zu einem 40s-Hang gefuehrt, der erst mit `LIBGL_ALWAYS_SOFTWARE=1` UND mehr
Geduld (laenger als 40s Timeout) verschwand - im finalen, oben gezeigten Skript wird deshalb ganz
bewusst auf `mw.resize()/mw.show()` verzichtet, `Gui.showMainWindow()` reicht.

### Ergebnis: Screenshots liegen unter `/home/maxx/freecad/agent-screenshots/`

- `1_real_install_WORKS.png` - echte Installation (`/home/maxx/freecad/install`): Wuerfel korrekt
  sichtbar, NaviCube, Achsenkreuz - alles normal.
- `2_sandbox_BROKEN.png` - Sandbox (`/home/maxx/freecad/install-claude-sandbox`): NaviCube UND
  Achsenkreuz sind sichtbar (Overlay-Rendering funktioniert), aber die Wuerfel-GEOMETRIE fehlt
  komplett - exakt das vom Nutzer beschriebene Symptom, jetzt objektiv per Screenshot bestaetigt.
- `3_pristine_copy_BROKEN_despite_identical_bytes.png` - eine BRANDNEUE `cp -a`-Kopie der ECHTEN
  Installation (`/home/maxx/freecad/install-claude-sandbox-pristine-test`, angelegt in einer
  fruehen Sitzung, NIE vom Agenten inhaltlich veraendert): zeigt DASSELBE Bild wie die Sandbox.
- `4_order_test_sandbox_FIRST_still_broken.png` / `5_order_test_real_SECOND_still_works.png` -
  siehe "Session-Reihenfolge-Hypothese" unten.

### Entscheidender Beweis: es liegt NICHT am Dateiinhalt

```bash
diff -rq /home/maxx/freecad/install /home/maxx/freecad/install-claude-sandbox-pristine-test
# Ergebnis: KEINE Ausgabe - 0 (null) Unterschiede in der GESAMTEN 294-MB-Baumstruktur.
```
Das schliesst zweifelsfrei aus: fehlerhaftes Kopieren, unterschiedlicher Build-Stand,
unterschiedlicher Dateiinhalt jeglicher Art (Binaries, Python-Skripte, Ressourcen, Icons, alles).
**Der einzige Unterschied zwischen einer funktionierenden und einer kaputten Installation ist der
Verzeichnispfad selbst, unter dem exakt dieselben Bytes ausgefuehrt werden.**

(Fuer die Sandbox selbst, die ja tatsaechlich eigene Code-Aenderungen des Agenten enthaelt, gilt
dieselbe Schlussfolgerung indirekt: `diff -rq install install-claude-sandbox` zeigt nur die
erwarteten `AssemblyGui.so`-Unterschiede plus eine voellig unrelated `.pyc`-Bytecode-Cache-Datei
eines CAM-Post-Prozessor-Skripts - siehe die Pristine-Kopie fuer den sauberen 0-Diff-Beweis OHNE
jede Assembly-Code-Aenderung.)

### Ausprobierte und WIDERLEGTE Erklaerungen fuer das "nur der Pfad zaehlt"-Phaenomen

1. **RUNPATH** (`readelf -d bin/FreeCAD` zeigt ein fest einprogrammiertes RUNPATH auf
   `/home/maxx/freecad/install/lib`, siehe fruehere Abschnitte oben) - erklaert zwar, dass ALLE
   Kopien ihre Kernbibliotheken (`FreeCADGui.so` etc.) tatsaechlich von der ECHTEN Installation
   laden (per `strace` bestaetigt: `openat(.../home/maxx/freecad/install/lib/libFreeCADGui.so...)`
   taucht auch im Sandbox-Trace auf) - aber da diese Datei laut obigem `diff -rq` byte-identisch
   ist, kann das NICHT die Ursache fuer unterschiedliches RENDER-Verhalten sein.
2. **Pfadlaenge** - eine Kopie an einem KUERZEREN Pfad (`/home/maxx/fc_s`, 27 Zeichen bis zur
   Binary, also kuerzer als die funktionierende echte Installation mit 38 Zeichen) zeigt trotzdem
   denselben Fehler - widerlegt.
3. **Mesa-Shader-Cache-Korruption** (`~/.cache/mesa_shader_cache`, geteilt zwischen allen
   Installationen) - Test mit `MESA_SHADER_CACHE_DIR` auf einen BRANDNEUEN, leeren Ordner
   umgeleitet (fuer die kurze-Pfad-Kopie) - Fehler bleibt bestehen - widerlegt.
4. **Session-Reihenfolge in Xvfb** - kompletter Neustart von Xvfb auf einem NIE zuvor benutzten
   Display (`:97`), Sandbox als ALLERERSTER Prozess getestet: weiterhin kaputt
   (`4_order_test_sandbox_FIRST_still_broken.png`). Echte Installation DANACH als zweiter Prozess
   in DERSELBEN Xvfb-Sitzung getestet: funktioniert weiterhin einwandfrei
   (`5_order_test_real_SECOND_still_works.png`). Reihenfolge spielt nachweislich keine Rolle.
5. **`CSF_*`-Umgebungsvariablen** (OCCT/OpenCASCADE-Ressourcenpfade, z.B.
   `CSF_MDTVFontDirectory`) - Code-Lesen in `Application.cpp` (`SaveEnv()`) zeigt: diese werden nur
   aus einer BEREITS BESTEHENDEN Shell-Umgebung uebernommen, nicht von FreeCAD selbst aus dem
   Home-Pfad berechnet - fuer beide Testlaeufe identisch, damit keine Erklaerung.
6. **`Application::getHomePath()`** (Linux-Implementierung, `/proc/self/exe`-basiert) wurde
   gegengelesen: liest korrekt den TATSAECHLICHEN Pfad der laufenden Binary aus (kein hartkodierter
   Wert) - das ist also grundsaetzlich pfad-korrekt/relozierbar, aber trotzdem tritt der Fehler
   auf. Was GENAU mit diesem korrekt ermittelten Pfad dann passiert (welche darauf aufbauende
   Ressourcen-Aufloesung fuer die Geometrie-Darstellung spezifisch fehlschlaegt), ist NICHT
   abschliessend geklaert - siehe naechster Abschnitt fuer die vielversprechendste Spur.

### Vielversprechendste, NICHT abschliessend verfolgte Spur: Tessellierung/BRepMesh wird in der kaputten Installation nie erreicht

Per `strace -f -e trace=openat,open` UNTER Xvfb+xcb (nicht `offscreen` - das haette den Bug gar
nicht gezeigt, siehe unten) fuer echte Installation UND Sandbox, mit demselben Testskript
(`test_screenshot5.py` oben), dann normalisiert verglichen (Installationspfade durch Platzhalter
ersetzt, damit nur ECHTE Unterschiede uebrigbleiben):

```bash
grep -oP '"[^"]*"' strace_xcb_real.log \
  | sed 's#/home/maxx/freecad/install-claude-sandbox#PREFIX#; s#/home/maxx/freecad/install\b#PREFIX#' \
  | sort -u > opens_real_norm.txt
# analog fuer die Sandbox mit demselben sed-Muster (WICHTIG: beide Installationspfad-Varianten in
# BEIDEN Dateien ersetzen, sonst entstehen Schein-Unterschiede durch die Normalisierung selbst!)
diff opens_real_norm.txt opens_sandbox_norm.txt
```

**Fund:** kurz NACHDEM der Warte-Cursor (Sanduhr, `.../cursors/watch`) geladen wird - also waehrend
`doc.recompute()`/die View-Erzeugung fuer das `Part::Box`-Objekt laeuft - probiert die ECHTE
(funktionierende) Installation GEZIELT nach folgenden Dateien, die in der Sandbox-Installation im
SELBEN Testlauf (identisches Dokument, identischer Box) KEIN EINZIGES MAL angefragt werden:

```
libirml.so.1           (mehrfach, an allen ueblichen Bibliothekspfaden probiert)
libiomp5.so             (mehrfach, an allen ueblichen Bibliothekspfaden probiert)
/sys/devices/system/cpu/online
/proc/meminfo
/proc/self/maps
/proc/sys/vm/nr_hugepages
/sys/kernel/mm/transparent_hugepage/enabled
```

`libirml`/`libiomp5` sind Intel-OpenMP/TBB-Laufzeitbibliotheken - **Achtung: sie sind auf diesem
System GAR NICHT installiert** (jede einzelne Anfrage endet mit `ENOENT`, auch im funktionierenden
Lauf) - das ist also NICHT selbst die Fehlerursache (die echte Installation kommt ja trotzdem ohne
sie klar), sondern ein sehr praeziser MARKER dafuer, dass ein bestimmter Code-Pfad ueberhaupt
ERREICHT wird: das Anfrage-Muster (Bibliothekssuche fuer einen Parallel-Compute-Backend, gefolgt
von CPU-Anzahl- und Hugepage-/Speicher-Allokator-Abfragen) ist ein klassisches Initialisierungs-
Muster fuer einen THREAD-POOL/PARALLELEN Backend - sehr wahrscheinlich OCCTs `BRepMesh`-
Tessellierung (die JEDE sichtbare Solid-Geometrie erst in ein Dreiecksnetz fuer Coin3D umwandeln
muss, bevor ueberhaupt etwas gezeichnet werden kann) oder ein aehnlicher OCCT-`TKParallel`-Pfad,
der bei OCCT-Buildvarianten mit TBB-Unterstuetzung beim ERSTEN Vernetzungs-Aufruf lazy initialisiert
wird.

**Schlussfolgerung/Hypothese (NICHT verifiziert, naechster konkreter Ansatzpunkt):** die kaputte
Installation (Sandbox, Pristine-Kopie, jede Kopie) ERREICHT diesen Tessellierungs-Initialisierungs-
Codepfad vermutlich NIE - nicht "rendert leer", sondern "kommt nie bis zur Mesh-Erzeugung". Warum
der Codepfad je nach Installationsverzeichnis unterschiedlich erreicht wird (trotz identischer
Bytes), ist die eigentliche, noch offene Frage. Konkrete naechste Schritte fuer eine kuenftige
Sitzung:
- gdb-Breakpoint auf `BRepMesh_IncrementalMesh`-Konstruktor bzw. dessen `Perform()`-Methode
  setzen, einmal in der echten Installation (sollte treffen) und einmal in der Sandbox (Test: wird
  der Breakpoint UEBERHAUPT erreicht?) - waere der definitive Beleg fuer/gegen diese Hypothese.
  `gdb` ist bereits auf dem System installiert (`/usr/bin/gdb`), kein Install-Schritt noetig -
  Achtung: ein `gdb -p <PID>`-Attach pausiert kurzzeitig ALLE Threads des Zielprozesses, bei einem
  eigenstaendig unter Xvfb gestarteten Testprozess (nicht der Session des Nutzers) ist das
  voellig unproblematisch.
- Alternativ: den relevanten FreeCAD-/PartGui-Code fuer "wann wird ein Shape tesselliert"
  durchsuchen (`grep -rn "BRepMesh_IncrementalMesh" src/`), um zu verstehen, unter welcher
  Bedingung `ViewProviderPartExt`/aehnliche Klassen die Tessellierung ueberhaupt ausloesen, und ob
  dabei irgendein Pfad-abhaengiger Vergleich/eine Pfad-abhaengige Fallunterscheidung existiert.
- Die vollstaendigen, unnormalisierten `strace`-Logs wurden NICHT dauerhaft gesichert (lagen im
  Scratchpad, `strace_xcb_real.log`/`strace_xcb_sandbox.log`, ca. 4300 Zeilen je Log) - bei Bedarf
  einfach den obigen Testaufbau (Abschnitt "Der Xvfb-Testaufbau") erneut mit
  `strace -f -e trace=openat,open -o out.log <FreeCAD-Aufruf>` davor packen.

### Clean-Build-Test (auf Nutzer-Wunsch, "zur vollstaendigen Sicherheit" trotz des 0-Diff-Befundes)

Der Nutzer wollte trotz des starken 0-Diff-Befundes zusaetzlich einen KOMPLETT FRISCHEN Build aus
einem sauberen, patch-freien Commit-Stand testen. Umgesetzt wie folgt:

```bash
# Sauberer Checkout OHNE jede eigene Aenderung, per git worktree (beruehrt das Haupt-Arbeits-
# verzeichnis /home/maxx/freecad/freecad-source NICHT):
cd /home/maxx/freecad/freecad-source
git worktree add --detach /home/maxx/freecad/freecad-source-clean bafe119010   # HEAD von main
cd /home/maxx/freecad/freecad-source-clean
git submodule update --init --recursive   # ca. 35s, benoetigt fuer OndselSolver/AddonManager/etc.
```

**WICHTIGE FALLE, mit einem fehlgeschlagenen ersten Build-Versuch entdeckt:** ein Build aus einem
BLOSS SAUBEREN Git-Checkout schlaegt auf diesem System OHNE WEITERES fehl (`error: no matching
function for call to 'QByteArray::QByteArray(std::string_view)'` in
`src/Gui/PreferencePages/DlgSettingsNavigation.cpp`) - das ist ein bereits bekannter,
dokumentierter Qt6-Versions-Kompatibilitaetsfehler (siehe `patches/README.md`), der NICHTS mit der
Assembly-/Rendering-Untersuchung zu tun hat. **Die drei reinen Build-Umgebungs-Patches muessen
IMMER zuerst angewendet werden, bevor auf diesem System ueberhaupt irgendetwas baubar ist**
(genau die Reihenfolge, die auch `update-and-rebuild-freecad.sh` verwendet):
```bash
cd /home/maxx/freecad/freecad-source-clean
git apply /home/maxx/Dokumente/FreeCAD-Development/FCProject/patches/freecad-cmake-disable-tests.patch
git apply /home/maxx/Dokumente/FreeCAD-Development/FCProject/patches/freecad-navigation-qbytearray-fix.patch
git apply /home/maxx/Dokumente/FreeCAD-Development/FCProject/patches/freecad-propertyeditor-qstring-fix.patch
```
Diese drei Patches sind KEINE Assembly-/Feature-Patches (sie aendern nichts an
Solver-/Rendering-Logik) - reine Kompiler-/Qt-Versions-Kompatibilitaet, siehe `patches/README.md`.

Danach konfiguriert (komplett getrennt von der echten Installation UND vom bestehenden
`build/`-Ordner):
```bash
cmake -S /home/maxx/freecad/freecad-source-clean -B /home/maxx/freecad/build-clean-test \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/home/maxx/freecad/install-clean-test \
  -DFREECAD_USE_EXTERNAL_COIN_PIVY=ON \
  -DPython3_EXECUTABLE=/home/maxx/Dokumente/FreeCAD-Development/.venv/bin/python
cmake --build /home/maxx/freecad/build-clean-test -- -j5   # j5 statt j12, RAM-Grund (15GB System)
```
Configure-Log bestaetigte identische Abhaengigkeitsversionen wie der bestehende Build (Coin3D
4.0.2, pivy 0.6.9, Qt 6.4.2, PySide 6.6.3, OCC 7.6.3) - kein Versions-Drift.

**STATUS BEIM SITZUNGSENDE: Build noch nicht fertig / Ergebnis noch nicht getestet.** Lief zuletzt
bei ca. 67% (Build-Log: `/home/maxx/freecad/logs/clean-build-full2.log`, PID war 67664, mit `-j5`
im Hintergrund). **Naechster Schritt fuer die anschliessende Sitzung:**
1. Pruefen, ob der Build fertig/erfolgreich ist: `tail -30 /home/maxx/freecad/logs/clean-build-full2.log`
   und `ls /home/maxx/freecad/build-clean-test/bin/FreeCAD`.
2. Falls fertig: `cmake --install /home/maxx/freecad/build-clean-test` (installiert nach
   `/home/maxx/freecad/install-clean-test`, dem in Schritt 2 oben angegebenen Prefix) - ODER
   direkt aus dem Build-Ordner testen, falls `bin/FreeCAD` dort schon lauffaehig ist (FreeCAD kann
   oft auch direkt aus dem Build-Baum laufen, ohne expliziten Install-Schritt - im Zweifel beides
   probieren).
3. Mit dem oben beschriebenen Xvfb-Testaufbau pruefen (`FC_BIN=.../install-clean-test/bin/FreeCAD`
   bzw. `.../build-clean-test/bin/FreeCAD`), Screenshot nach `/home/maxx/freecad/agent-screenshots/`
   speichern (z.B. `6_clean_build_test.png`).
4. Erwartung basierend auf dem 0-Diff-Befund: **vermutlich ebenfalls kaputt** (da der Bug
   nachweislich nicht am Dateiinhalt haengt) - falls der frische Build ABWEICHEND tatsaechlich
   FUNKTIONIERT, waere das ein hochinteressanter, dem 0-Diff-Befund scheinbar widersprechender
   Fund, der sofort genauer untersucht werden muesste (z.B. unterschiedliche RUNPATH-Werte durch
   die neue, andere `CMAKE_INSTALL_PREFIX`-Konfiguration - das waere dann tatsaechlich ein
   RUNPATH-bezogener Unterschied, anders als bei den reinen `cp -a`-Kopien, die das alte,
   unveraenderte RUNPATH behalten).

### Aufraeum-Hinweis fuer eine spaetere, ruhige Gelegenheit (kein Blocker)

Folgende, waehrend dieser Untersuchung angelegte Verzeichnisse/Prozesse sind aktuell (Sitzungsende)
noch vorhanden und wurden bewusst NICHT geloescht (falls die naechste Sitzung sie noch braucht):
- `/home/maxx/freecad/install-claude-sandbox-pristine-test` (294 MB, 0-Diff-Referenzkopie)
- `/home/maxx/fc_s` (294 MB, Kurzpfad-Testkopie)
- `/home/maxx/freecad/freecad-source-clean` (Git-Worktree, ca. 512 MB inkl. Submodule)
- `/home/maxx/freecad/build-clean-test` (Build-Ordner, waechst waehrend des Builds, aktuell >600 MB)
- `/home/maxx/freecad/install-clean-test` (Ziel-Installationsordner, wird erst bei `cmake --install`
  befuellt)
- Xvfb-Serverprozess auf Display `:97` (PID 47221 beim Sitzungsende - falls nicht mehr am Laufen,
  einfach per obigem Befehl neu starten)
- `/home/maxx/freecad/agent-screenshots/` (dauerhaft gedachter Ablageort fuer alle
  Diagnose-Screenshots dieser und kuenftiger Sitzungen - bewusst NICHT im FCProject-Git-Repo, da
  reine Diagnose-Bilder, keine Projektdateien)
Alle vier grossen Testverzeichnisse (`install-claude-sandbox-pristine-test`, `fc_s`,
`freecad-source-clean`, `build-clean-test`) koennen nach Abschluss dieser Untersuchung
folgenlos geloescht werden (`rm -rf`) - sie enthalten keine eigenstaendigen, nicht anderswo
gesicherten Ergebnisse. `install-clean-test` ebenso, sobald der Clean-Build-Test ausgewertet ist.

## ROOT CAUSE GEFUNDEN UND BEHOBEN (2026-08-29, Abschluss der Sandbox-3D-View-Untersuchung)

**Der Clean-Build-Test (siehe oben) hat die entscheidende Spur geliefert - die tote 3D-Ansicht ist
vollstaendig erklaert, reproduziert und behoben.**

### Clean-Build-Ergebnis

Build wurde fertiggestellt (`cmake --build` 100% durchgelaufen, keine Fehler) und installiert
(`cmake --install /home/maxx/freecad/build-clean-test`, Ziel `/home/maxx/freecad/install-clean-test`).
Per Xvfb+Screenshot getestet (exakt derselbe Testaufbau wie oben beschrieben) -
**Ergebnis: FUNKTIONIERT EINWANDFREI** (`6_clean_build_WORKS.png` in
`/home/maxx/freecad/agent-screenshots/`) - Wuerfel korrekt sichtbar, wie bei der echten
Installation.

Das war zunaechst UEBERRASCHEND angesichts des vorherigen 0-Diff-Befundes ("Dateiinhalt ist nicht
die Ursache") - der entscheidende Unterschied zwischen dem funktionierenden Clean-Build und den
kaputten `cp -a`-Kopien war aber sofort per `readelf` sichtbar:

```
readelf -d /home/maxx/freecad/install/bin/FreeCAD              | grep -i runpath
#   RUNPATH: [/home/maxx/freecad/install/lib]                    <- eigener, korrekter Pfad
readelf -d /home/maxx/freecad/install-claude-sandbox/bin/FreeCAD | grep -i runpath
#   RUNPATH: [/home/maxx/freecad/install/lib]                    <- FALSCH! zeigt auf die ECHTE
#                                                                    Installation, NICHT auf sich
#                                                                    selbst!
readelf -d /home/maxx/freecad/install-clean-test/bin/FreeCAD    | grep -i runpath
#   RUNPATH: [/home/maxx/freecad/install-clean-test/lib]         <- eigener, korrekter Pfad (neu
#                                                                    beim Konfigurieren berechnet)
```

### Root Cause

`RUNPATH` wird beim Bauen/Verlinken FEST IN DIE BINARY EINPROGRAMMIERT (aus
`CMAKE_INSTALL_PREFIX`, zum Konfigurationszeitpunkt). Eine `cp -a`-Kopie eines bereits fertig
gebauten `install`-Baums kopiert diesen fest einprogrammierten Wert MIT - die Kopie "weiss" beim
Start also nicht, dass sie an einem ANDEREN Ort liegt, und der dynamische Linker laedt ihre
Kernbibliotheken (`libFreeCADApp.so`, `libFreeCADGui.so`, `Part.so`, etc.) weiterhin von der
ECHTEN, urspruenglichen Installation (`/home/maxx/freecad/install/lib`) - bestaetigt per `strace`:
`openat(..., "/home/maxx/freecad/install/lib/libFreeCADGui.so", ...)` taucht auch im
Sandbox-Prozess auf, obwohl `install-claude-sandbox/bin/FreeCAD` die tatsaechlich gestartete Datei
ist.

Das allein waere harmlos, WENN diese Bibliotheken byte-identisch sind (sind sie, siehe 0-Diff-
Befund) UND nichts den TATSAECHLICHEN Ausfuehrungsort mit dem RUNPATH-geladenen Ort vergleicht.
**Irgendwo in der Coin3D-/OCCT-Initialisierungskette fuer die eigentliche Geometrie-Darstellung
(nicht fuer das GL-Overlay/NaviCube, das unabhaengig davon funktioniert) gibt es aber offenbar
GENAU so einen Vergleich bzw. eine Pfad-abhaengige Fallunterscheidung** - vermutlich zwischen
`Application::getHomePath()` (liest `/proc/self/exe`, liefert fuer eine Kopie korrekt DEREN
EIGENEN Pfad) und irgendeinem intern zusaetzlich berechneten "wo wurden meine Kernbibliotheken
tatsaechlich geladen"-Wert (der durch RUNPATH auf die ECHTE Installation zeigt) - bei einem
Mismatch zwischen beiden wird die Tessellierungs-/Mesh-Erzeugungs-Initialisierung fuer die 3D-
Geometrie-Darstellung offenbar stillschweigend uebersprungen (passend zum bereits dokumentierten
`libiomp5`/`libirml`/CPU-Anzahl-Befund weiter oben: dieser Codepfad wird in der Sandbox nie
erreicht). Die GENAUE Stelle im Code, die diesen Vergleich macht, wurde NICHT mehr gefunden (waere
der letzte, verbleibende Schritt fuer eine vollstaendige Erklaerung bis auf Zeilen-Ebene - siehe
"Verbleibende offene Frage" unten) - der Mechanismus (RUNPATH-vs-tatsaechlicher-Pfad-Mismatch) ist
aber durch den Fix im naechsten Abschnitt experimentell zweifelsfrei bestaetigt.

### Der Fix (verifiziert, funktioniert)

**`LD_LIBRARY_PATH` mit dem EIGENEN `lib`-Ordner voranstellen** - `LD_LIBRARY_PATH` hat beim
dynamischen Linker Vorrang vor dem im Binary einprogrammierten `RUNPATH`, zwingt den Linker damit,
die Kernbibliotheken tatsaechlich aus dem EIGENEN Installationsordner zu laden statt ueber RUNPATH
zur echten Installation umgeleitet zu werden:

```bash
export LD_LIBRARY_PATH="/home/maxx/freecad/install-claude-sandbox/lib:$LD_LIBRARY_PATH"
# (plus die uebrigen, bereits etablierten Qt/venv-Umgebungsvariablen, siehe Xvfb-Testaufbau oben)
```

**Verifiziert per Screenshot:** `7_sandbox_FIXED_with_LD_LIBRARY_PATH.png` - identischer Testaufbau
wie bei `2_sandbox_BROKEN.png` (dasselbe Binary, dasselbe Dokument, dieselbe Xvfb-Instanz), NUR mit
diesem einen zusaetzlichen `LD_LIBRARY_PATH`-Eintrag - Wuerfel jetzt korrekt sichtbar.

**Der wiederverwendbare Testaufbau wurde bereits entsprechend aktualisiert:**
`run-freecad-isolated.sh` (im Scratchpad, siehe frueherer Abschnitt fuer den vollstaendigen
urspruenglichen Inhalt) setzt jetzt automatisch
`LD_LIBRARY_PATH="<lib-Ordner relativ zu FC_BIN>:..."` - berechnet aus `FC_BIN`s eigenem Pfad
(`$(dirname "$FC_BIN")/../lib`), funktioniert also automatisch fuer JEDE Installation, nicht nur
die Sandbox. End-to-end mit dem tatsaechlich aktualisierten Skript nochmal gegengetestet (nicht nur
mit einem Wegwerf-Testaufruf) - funktioniert.

**Praktische Konsequenz fuer JEDE kuenftige Sitzung:** die Sandbox-Installation
(`/home/maxx/freecad/install-claude-sandbox`) selbst ist NIE das Problem gewesen und muss NICHT
neu gebaut/repariert werden - sie muss nur IMMER ueber `run-freecad-isolated.sh` (oder mit
demselben `LD_LIBRARY_PATH`-Trick von Hand) gestartet werden, NIE direkt per
`/home/maxx/freecad/install-claude-sandbox/bin/FreeCAD ...` ohne diesen Fix. **Damit ist der
urspruengliche Blocker fuer den eigentlichen Live-Drag-Test (Teilschritt-2-Diagnose-Logging fuer
den BoxB-Rubberband-Befund, siehe die entsprechenden Abschnitte weiter oben) beseitigt** - der
naechste sinnvolle Schritt ist, GENAU DIESEN Test (BoxB in `MinimalReproSub.FCStd`s eigener
Baugruppe anklicken/ziehen, `mouseButtonPressed()`-Diagnose-Logging beobachten) jetzt SELBSTAENDIG
in der reparierten Sandbox durchzufuehren, statt weiter auf einen Live-Test des Nutzers in der
echten Installation angewiesen zu sein.

### Verbleibende offene Frage (kein Blocker, rein akademisches Interesse)

Die EXAKTE Code-Stelle, die den RUNPATH-vs-tatsaechlicher-Pfad-Mismatch erkennt und daraufhin die
Tessellierungs-Initialisierung uebergeht, wurde nicht gefunden - waere per `gdb`-Breakpoint auf
`BRepMesh_IncrementalMesh` (Konstruktor oder `Perform()`) in EINEM per `LD_LIBRARY_PATH`
funktionierenden Sandbox-Lauf vs. einem NICHT gefixten Lauf direkt beobachtbar (wird der
Breakpoint im ungefixten Fall ueberhaupt erreicht?) - reine Neugier, kein Hindernis fuer die
Weiterarbeit, da der FIX bereits zuverlaessig funktioniert, unabhaengig davon, ob die genaue
Codezeile bekannt ist.

### Aktualisierte Aufraeum-/Statusliste (ersetzt die vorherige "Aufraeum-Hinweis"-Liste oben fuer den Sandbox-Teil)

- **Sandbox-3D-View-Bug: GELOEST.** `install-claude-sandbox` selbst bleibt unveraendert (keine
  Neuinstallation noetig) - ab jetzt IMMER mit `LD_LIBRARY_PATH`-Fix starten (siehe
  `run-freecad-isolated.sh`).
- `install-clean-test`/`build-clean-test`/`freecad-source-clean` (Clean-Build-Artefakte): haben
  ihren Zweck erfuellt (Beweis erbracht), koennen bei Gelegenheit geloescht werden, falls
  Speicherplatz gebraucht wird - werden fuer die eigentliche Drag/Joint-Untersuchung nicht mehr
  gebraucht (die laeuft ueber die reparierte Sandbox weiter).
- `install-claude-sandbox-pristine-test`/`fc_s`: ebenso, reine Beweis-Artefakte, koennen geloescht
  werden.
- Xvfb-Serverprozess auf Display `:97`: bleibt fuer die naechste Sitzung nuetzlich (spart
  Neustart), bei Bedarf per obigem Befehl neu starten falls nicht mehr aktiv.
- `/home/maxx/freecad/agent-screenshots/`: enthaelt jetzt 7 Screenshots (1-5 wie vorher, plus
  `6_clean_build_WORKS.png` und `7_sandbox_FIXED_with_LD_LIBRARY_PATH.png`) - bleibt als
  dauerhafter Diagnose-Ablageort bestehen.
- `run-freecad-isolated.sh` (Scratchpad) enthaelt jetzt den finalen, funktionierenden Stand
  inklusive `LD_LIBRARY_PATH`-Fix - **falls das Scratchpad zwischen Sitzungen geleert wurde,
  einfach aus dem Originaltext weiter oben in diesem Dokument PLUS dem `LD_LIBRARY_PATH`-Fix aus
  diesem Abschnitt neu zusammensetzen** (beide Textbloecke zusammen ergeben den kompletten,
  aktuellen Skriptinhalt).

## Teilschritt 2/2c: FINALER Beweis erbracht (2026-08-29, Abschluss)

Nachdem der Nutzer live bestaetigt hat, dass Ziehen in der reparierten Sandbox (mit dem
`LD_LIBRARY_PATH`-Fix) ueberhaupt wieder funktioniert ("jetzt ziehen auch funktioniert"), wurde
gezielt der eigentliche, urspruengliche Beweis fuer Teilschritt 2/2c automatisiert nachvollzogen:
**haelt sich der Slider-Joint zwischen `BoxA` und `BoxB` (2 Ebenen tief verschachtelt ueber
`MinimalReproSub.FCStd` -> `MinimalReproTop.FCStd` -> `MinimalReproGrandTop.FCStd`) beim Ziehen von
`BoxB` tatsaechlich an seine Achse, statt sich frei/falsch zu bewegen?** Das ist die Kernfrage, die
den ganzen Sommer ueber offen war (Root Cause: `objectPartMap`/`getMovingPartFromRef()` kollabierte
verschachtelte Teile auf denselben Wrapper) - "Objekt bewegt sich ueberhaupt" alleine waere kein
Beweis gewesen.

### Testaufbau

Scratch-Kopie der 3 Original-Testdateien (`MinimalReproGrandTop/Top/Sub.FCStd`, NICHT die
Originale) unter `$SCRATCH/drag_test_grandtop/`. Skripte (im Scratchpad, ggf. bei Bedarf aus den
Codebloecken unten rekonstruierbar, falls das Scratchpad zwischen Sitzungen geleert wurde):

- `inspect_joint_axis.py`: oeffnet `MinimalReproGrandTop.FCStd`, liest den Slider-Joint `Joint`
  (Label "Gleitverbindung", `JointType = Slider`) direkt aus dem `MinimalReproSub`-Dokument
  (`part1='MinimalReproSub#BoxA'`, `part2='MinimalReproSub#BoxB'`, per Solve-Log bestaetigt) und
  berechnet die tatsaechliche Gleitachse in Weltkoordinaten: `joint.Placement1.Rotation.multVec(...)`
  auf die lokalen Achsen (0,0,1)/(1,0,0)/(0,1,0) angewendet ergibt: lokale Z-Achse -> Welt-X
  `(1,0,0)`, lokale X-Achse -> Welt-Z `(0,0,1)`, lokale Y-Achse -> Welt-(-Y)`(0,-1,0)` (alle exakt
  bis auf Gleitkomma-Rauschen ~1e-17). Der FreeCAD-Konvention nach (Slider gleitet entlang der
  lokalen Z-Achse der Joint-Placement) ist die erwartete Gleitachse in diesem Testfall also exakt
  die Welt-X-Achse `(1, 0, 0)`.
- `test_drag_gt3.py`: oeffnet `MinimalReproGrandTop.FCStd`, aktiviert das MDI-Subfenster,
  `viewIsometric()`+`fitAll()`, liest `BoxA.Placement`/`BoxB.Placement` VOR jeder Interaktion
  (beide ~exakt am Ursprung, keine Rotation), ruft `adoc.setEdit(asm.Name, 0)` auf dem
  TOP-LEVEL-`Assembly`-Objekt von GrandTop auf (nicht auf einer verschachtelten Sub-Baugruppe -
  wichtig, da laut frueherer Untersuchung nur die aktiv editierte, typischerweise aeusserste
  `AssemblyObject`-ViewProvider-Instanz `preDrag()`/`doDragStep()` waehrend eines interaktiven
  Drags erhaelt), findet per `QApplication.widgetAt(QPoint(490, 350))` das echte 3D-View-Widget an
  `BoxB`s Bildschirmposition (aus dem vorherigen Screenshot `gt_step1_editmode.png` abgelesen),
  simuliert per rohem `QMouseEvent`+`QApplication.sendEvent()` einen Press -> 6 diagonale
  Move-Schritte (`dx,dy` von (5,5) bis (40,40)) -> Release, loggt `BoxA.Placement`/`BoxB.Placement`
  nach JEDEM Zwischenschritt sowie unmittelbar nach dem Release UND nach einem expliziten
  `doc.recompute()` danach, und speichert abschliessend einen Screenshot.

### Ergebnis (zwei unabhaengige Laeufe, reproduzierbar identisch bis auf Gleitkomma-Rauschen)

Waehrend des Ziehens selbst (Zwischenzustaende, VOR dem finalen Recompute) zeigte `BoxB` zunaechst
noch KEINE saubere Achsenbindung, z.B. im letzten Zwischenschritt (`dx=40,dy=40`):

```
BoxB.Placement (waehrend Drag, vor Release) = Pos=(2.4675, 0.6612, -1.8063)
```

Das entspricht NICHT reiner Bewegung entlang der Welt-X-Achse - Komponente entlang der Achse
(1,0,0): 2.4675; Komponente SENKRECHT zur Achse: Vektor (0, 0.6612, -1.8063), Laenge ~1.92mm, also
in derselben Groessenordnung wie die Achsenkomponente selbst. **Das ist aber KEIN Fehlerbefund**,
sondern normales, aus Einzel-Ebenen-Tests bereits bekanntes Verhalten: waehrend des laufenden Drags
zeigt der Dragger/das Objekt zunaechst eine ungefaehre, noch nicht vollstaendig constraint-geloeste
Zwischenposition (die Maus-Projektion wird erst grob uebernommen); die vollstaendige
Zwangsbedingungsloesung (OndselSolver) laeuft final bei `doDragStep()`-Abschluss bzw. beim
naechsten `recompute()`.

**Nach dem Release UND explizitem `doc.recompute()`** (das reale, nicht simulierte FreeCAD tut dies
beim Loslassen der Maustaste automatisch selbst):

Lauf 1:
```
AFTER RECOMPUTE: BoxA.Placement = Pos=(-7.53362e-16, 1.11022e-16, 1.55431e-15)   [~exakt Ursprung, unveraendert]
AFTER RECOMPUTE: BoxB.Placement = Pos=(2.46751, -1.02654e-16, 3.6998e-15)        [nur X != 0]
```

Lauf 2 (unabhaengiger Rerun, zur Reproduzierbarkeitspruefung):
```
AFTER RECOMPUTE: BoxA.Placement = Pos=(-4.29708e-16, 3.33067e-16, 2.22045e-15)   [~exakt Ursprung, unveraendert]
AFTER RECOMPUTE: BoxB.Placement = Pos=(2.46751, 1.19391e-16, 3.47776e-15)        [nur X != 0]
```

**Damit ist der Beweis erbracht:** nach vollstaendiger Constraint-Loesung liegt die Y- und
Z-Komponente von `BoxB`s Positionsaenderung bei ~1e-16 (reines Gleitkomma-Rauschen, nicht
unterscheidbar von exakt Null), waehrend die X-Komponente (die berechnete Gleitachse) den vollen
Betrag der Verschiebung traegt - UND dieser X-Wert ist zwischen beiden unabhaengigen Laeufen
identisch (2.46751), UND `BoxA` (starr mit dem geerdeten `BoxC` verbunden ueber die Kette
`BoxC -[Joint]- BoxD -[Joint002 Fixed]- BoxA`, alles ueber `MinimalReproTop`) bleibt in beiden
Laeufen exakt unbewegt. Auch die Rotation (Yaw-Pitch-Roll) von `BoxB` blieb waehrend des gesamten
Vorgangs unveraendert (`(0, 5.08889e-14, 0)`, reines Rauschen), wie es fuer einen reinen
Slider-Joint (keine Rotation) erwartet wird.

Visuelle Bestaetigung: `gt_step3_final_recomputed.png` (im Scratchpad) zeigt `BoxB` (blau markiert)
sichtbar entlang der Slider-Achse aus `BoxA` herausgeschoben, exakt wie fuer einen funktionierenden
Slider-Joint zu erwarten.

### Bedeutung

Das war zunaechst als der urspruenglich gesuchte, fehlende Beweis fuer Teilschritt 2/2c
interpretiert worden. **Diese Einschaetzung war VORSCHNELL/zu weitgehend - siehe die Korrektur
direkt im Anschluss.** Was hier tatsaechlich quantitativ nachgewiesen wurde (und weiterhin gueltig
bleibt, siehe unten): der SOLVER-KERN (`doc.recompute()` -> `AssemblyObject::execute()`/`solve()`
von Grund auf neu, mit der Teilschritt-2/2c-Identitaetsaufloesung) loest den 2-Ebenen-tief
verschachtelten Slider-Joint zwischen `BoxA` und `BoxB` korrekt und projiziert dessen Position
korrekt auf die Slider-Achse - das ist real und reproduziert. NICHT bewiesen war (und wurde faelschlich
so hingestellt) die Korrektheit des GESAMTEN interaktiven Ziehens inklusive Live-Rueckmeldung waehrend
der Mausbewegung und der Anzeige im Baum/3D-View danach.

## KORREKTUR (2026-08-29, unmittelbar danach): Gegenbefund des Nutzers - "finaler Beweis" war zu weitgehend

Der Nutzer hat in der REPARIERTEN Sandbox einen ECHTEN, manuellen Maus-Drag von `BoxB` in
`MinimalReproGrandTop.FCStd` durchgefuehrt und danach "neu berechnet" (GUI-Recompute bzw.
Python-Konsole). Ergebnis (Placement-Abfrage in der Python-Konsole):

```
Vorher:  Pos=(-46.5702, -26.3231, -44.9882)
Nachher: Pos=(-58.1395, -23.5576, -64.2033)
Delta:   dX=-11.5693, dY=+2.7655, dZ=-19.2151
```

Das ist EINDEUTIG NICHT achsengebunden - `dZ` (19.2) ist sogar groesser als `dX` (11.6), `dY` ist
ebenfalls klar von Null verschieden. Rotation blieb beide Male unveraendert. Das widerspricht
direkt dem oben dokumentierten "finalen Beweis" (dort: `dX=2.46751mm`, `dY/dZ~1e-16`).

### Nachvollzogene Ursachenanalyse

**1. Der automatisierte Test hat NICHT bewiesen, was er zu beweisen behauptete hat.** Ein Blick in
den tatsaechlichen Gui-Code (`Gui/ViewProviderAssembly.cpp`, `tryMouseMove()`, ca. Zeile 555-620)
zeigt: bei JEDER Mausbewegung wird zuerst eine rein UI-seitige, kinematische Naeherung `plc` berechnet
(je nach `DragMode` z.B. reine freie 3D-Translation entlang der Maus-Projektion, VOELLIG UNABHAENGIG
vom Solver) und per `propPlacement->setValue(plc)` DIREKT gesetzt. Erst DANACH wird (nur wenn die
Einstellung `SolveOnMove` aktiv ist, Standard: an) `assemblyPart->doDragStep()` aufgerufen, was den
echten MbD-Solver-Schritt ausfuehrt und die Platzierung eigentlich sofort korrigieren sollte. In
meinem automatisierten Test (`test_drag_gt3.py`, synthetische `QMouseEvent`s per
`QApplication.sendEvent()`) zeigten die ZWISCHENSCHRITTE waehrend des simulierten Drags ein klar
UNKORRIGIERTES, linear mit dem Pixel-Offset wachsendes Verhalten (z.B. bei `dx=40`:
`Pos=(2.4675, 0.6612, -1.8063)` - deutlich NICHT achsengebunden) - das deutet stark darauf hin,
dass die synthetischen Events entweder `doDragStep()` gar nicht pro Bewegung ausgeloest haben (ein
bekanntes Risiko bei ueber `sendEvent()` direkt injizierten statt echten nativen X11-Events, die
Coin3D/Quarter u.U. anders verarbeitet), oder `doDragStep()` zwar lief, aber aus einem anderen Grund
nicht sofort vollstaendig korrigierte. Der finale Beweis kam in meinem Test ausschliesslich aus dem
ZUSAETZLICHEN, von mir selbst nach dem Drag explizit aufgerufenen `doc.recompute()` - NICHT aus dem
eigentlichen interaktiven Zieh-Pfad selbst. Ich hatte also faktisch nur den SOLVER-KERN bei einem
erzwungenen Voll-Recompute getestet, nicht den echten interaktiven Drag-Erlebnis-Pfad - genau die
bereits vorher als "dritte, eigenstaendige Baustelle" dokumentierte Luecke.

**2. Trotzdem: der Solver-Kern selbst wurde zusaetzlich UNABHAENGIG von jeglicher Drag-Simulation
bestaetigt.** Um die "recompute() greift moeglicherweise nicht zuverlaessig"-Hypothese zu pruefen,
wurde `BoxB.Placement` PYTHON-SEITIG DIREKT (ganz ohne jede Maus-/Drag-Simulation) auf exakt den vom
Nutzer berichteten "Vorher"-Wert `Pos=(-46.5702, -26.3231, -44.9882)` gesetzt und anschliessend
`doc.recompute()` aufgerufen (`test_recompute_semantics.py`). Ergebnis:

```
Nach doc.recompute(): BoxA = Pos=(~0, ~0, ~0)  [unveraendert, weiterhin starr/geerdet]
                       BoxB = Pos=(-46.57020, 4.03e-15, 1.44e-14)  [Y/Z auf Rauschniveau geschnappt]
```

Das bestaetigt reproduzierbar (jetzt insgesamt 3x unabhaengig: 2x nach echtem simuliertem Drag, 1x
nach reiner direkter Werteinspritzung): ein vollstaendiger `recompute()` loest den verschachtelten
Slider-Joint IMMER korrekt achsengebunden, UNABHAENGIG vom Ausgangswert, UND widerlegt die
Hypothese, `recompute()` wuerde die Assembly bei einer reinen `Placement`-Aenderung an `BoxB` nicht
zuverlaessig als "touched" erkennen (sie wurde erkannt, der Solve lief tatsaechlich neu).

**3. Warum weicht der Nutzer-Befund dann trotzdem ab, wenn selbst der exakte "Vorher"-Wert bei mir
sauber snappt?** Das ist die eigentlich noch OFFENE Frage. Zwei naheliegende, bereits an anderer
Stelle in diesem Dokument/in MEMORY.md dokumentierte, unabhaengige Bugs kommen als Erklaerung in
Frage, OHNE dass Teilschritt 2/2c selbst dafuer verantwortlich sein muss:

- **AssemblyLink stale placement bug** (siehe MEMORY-Eintrag
  `project_fcproject_assemblylink_stale_placement_bug.md`): ein `App::Link`-Mirror-Objekt (und
  genau das ist `BoxB` in `MinimalReproGrandTop` - bestaetigt per `TypeId=App::Link`) kann nach
  einer Solver-Korrektur im Quelldokument weiterhin die ALTE, veraltete Position anzeigen, auch nach
  `recompute()`. Wenn der Nutzer die Placement ueber die Python-Konsole GENAU AN DIESEM
  GrandTop-lokalen `App::Link`-Objekt abgefragt hat (statt am eigentlichen Quellobjekt in
  `MinimalReproSub`), koennte der angezeigte "Nachher"-Wert schlicht STALE/UNSYNCHRONISIERT sein -
  voellig unabhaengig davon, ob der Solver im Hintergrund tatsaechlich korrekt (achsengebunden)
  geloest hat.
- **GroundedJoint/RigidGroup verschwindet bei verschachtelter flexibler Baugruppe** (siehe
  MEMORY-Eintrag `project_fcproject_grounded_joint_nested_flex_assembly_bug.md`): sollte in der
  LIVE-Session des Nutzers (durch vorangegangene Interaktionen, ohne dass dies im Dateisystem
  sichtbar wird, da nie gespeichert - siehe naechster Punkt) die starre Kette
  `BoxC (geerdet) -> BoxD -> BoxA` bereits beschaedigt/aufgeloest gewesen sein, waere `BoxA` selbst
  nicht mehr zuverlaessig fixiert und das gesamte Teilsystem koennte sich frei (nicht mehr nur
  1-DOF-slidend) bewegt haben - das wuerde ein Delta erklaeren, das in KEINER Achse sauber aufgeht.

**Wichtige einschraenkende Tatsache:** `git status`/`git diff` zeigen fuer
`patches/bugreport-nested-flex-joint-detach/*.FCStd` inzwischen KEINE Aenderungen mehr (die zu
Sitzungsbeginn dort sichtbaren lokalen Modifikationen sind nicht mehr vorhanden bzw. wurden nie auf
Platte gespeichert) - der exakte Live-Speicherzustand des Nutzer-Dokuments zum Testzeitpunkt
(insbesondere: Zustand von `GroundedJoint`/`Joint002`/Rigid Group, und ob `BoxA` sich waehrend des
Tests mitbewegt hat) konnte NICHT aus einer Kopie der Datei rekonstruiert werden, da er nur im
RAM der Live-Session existierte und nicht gesichert wurde. Ein 1:1-Nachvollzug des exakten
Nutzer-Szenarios war deshalb nicht moeglich.

### Ehrliche Neueinordnung

- **Solide bewiesen (3x reproduziert, davon 1x ganz ohne jede Drag-Simulation):** der
  SOLVER-KERN - `AssemblyObject::solve()`/`execute()` bei explizitem `recompute()` - loest den
  2-Ebenen-tief verschachtelten Slider-Joint zwischen `BoxA`/`BoxB` korrekt achsengebunden aus
  JEDER Ausgangsplatzierung heraus. Die Teilschritt-2/2c-Identitaetsaufloesung
  (`getJoints()`-Nesting-Prefix, `resolvePartForMbD()` in `getConnectedParts()`/
  `removeUnconnectedJoints()`) funktioniert fuer diesen Teil nachweislich.
- **NICHT bewiesen (im Gegenteil: ein konkreter Gegenbefund liegt vor):** dass der VOLLSTAENDIGE,
  vom Nutzer tatsaechlich erlebte interaktive Drag-Vorgang (inkl. Live-Rueckmeldung waehrend der
  Mausbewegung, Anzeige-Aktualisierung von `App::Link`-Mirrors, Zustand der geerdeten/starren Kette
  in einer laenger laufenden Live-Session) fuer dieses verschachtelte Szenario zuverlaessig
  achsengebunden ist bzw. dem Nutzer korrekt angezeigt wird. Die naheliegendsten Erklaerungen dafuer
  sind zwei SEPARATE, bereits vorher bekannte/dokumentierte Bugs (AssemblyLink-Stale-Placement,
  GroundedJoint/RigidGroup-Verschwinden bei verschachtelter flexibler Baugruppe) - nicht notwendig
  ein Fehler in Teilschritt 2/2c selbst - aber das ist eine HYPOTHESE, kein Beweis, solange der
  exakte Live-Zustand des Nutzer-Tests nicht rekonstruiert/wiederholt werden konnte.
- **Naechster sinnvoller Schritt (noch offen):** einen kontrollierten Live-Nachtest mit dem Nutzer
  wiederholen, bei dem VOR und NACH dem Drag zusaetzlich (a) `BoxA`s Placement (nicht nur `BoxB`s),
  (b) ob `GroundedJoint`/das Rigid-Group-Aequivalent noch existieren, und (c) die Placement
  DIREKT AM QUELLOBJEKT in `MinimalReproSub` (nicht nur am `App::Link`-Mirror in GrandTop) erfasst
  werden - erst damit laesst sich zwischen "Solver-Kern hat trotzdem einen Nesting-spezifischen
  Restfehler" und "separate, bereits bekannte Anzeige-/Grounding-Bugs taeuschen ein falsches Bild
  vor" sauber unterscheiden.

**Status Teilschritt 2/2c: SOLVER-KERN (recompute-basierte Neuberechnung) VERIFIZIERT - der
VOLLSTAENDIGE interaktive Live-Drag-Pfad ist WEITERHIN NICHT ABSCHLIESSEND BEWIESEN, ein
unerklaerter Gegenbefund des Nutzers steht offen. Die vorherige Formulierung "ABGESCHLOSSEN UND
VERIFIZIERT" war zu weitgehend und wird hiermit zurueckgenommen/praezisiert.**

---

## PAUSE-VERMERK (2026-08-29, Sessionende) - START HIER FUER DEN NAECHSTEN TAG/AGENTEN

Sitzung pausiert wegen Sessionlimit, NICHT wegen eines Blockers. Aktueller Branch:
`solver-root-cause-fix`, aktuellster Commit `949b0c5` (bereits gepusht, lokal == remote geprueft).
Kein uncommitteter Rest offen (`git status --short` zeigt nur das ungetrackte `.worktrees/`
Verzeichnis, das ist kein zu committender Inhalt).

**Konkreter naechster Schritt (siehe auch "Naechster sinnvoller Schritt" direkt oben in der
KORREKTUR-Sektion):** ein kontrollierter Live-Nachtest mit dem Nutzer in der reparierten Sandbox
(`install-claude-sandbox`, IMMER mit `run-freecad-isolated.sh`/`LD_LIBRARY_PATH`-Fix starten, siehe
weiter oben). Dabei VOR und NACH einem manuellen Drag von `BoxB` in `MinimalReproGrandTop.FCStd`
zusaetzlich zu `BoxB`s eigener Placement folgendes in der Python-Konsole erfassen:

1. `BoxA`s Placement (nicht nur `BoxB`s) - bleibt `BoxA` wirklich exakt fixiert, oder bewegt sich
   die vermeintlich starre Kette (`BoxC` (geerdet) -> `BoxD` -> `BoxA`) mit?
2. Existiert `GroundedJoint`/das Rigid-Group-Aequivalent im `MinimalReproTop`-Dokument nach dem
   Drag noch unveraendert (Bezug: bereits bekannter, separater Bug "GroundedJoint/RigidGroup
   verschwindet bei verschachtelter flexibler Baugruppe")?
3. Placement DIREKT am Quellobjekt in `MinimalReproSub` auslesen (nicht nur am `App::Link`-Mirror
   `BoxB` in GrandTop) - Bezug: bereits bekannter, separater Bug "AssemblyLink stale placement".

Ziel: unterscheiden, ob der abweichende Nutzer-Befund von gestern (siehe KORREKTUR-Sektion oben,
`dX=-11.5693, dY=+2.7655, dZ=-19.2151`, klar nicht achsengebunden) durch einen echten,
Nesting-spezifischen Restfehler im interaktiven Drag-Pfad selbst verursacht wird, oder durch einen
der zwei separaten, bereits dokumentierten Bugs (Stale-Placement-Anzeige bzw. verschwundene
Erdung) nur vorgetaeuscht wird. Erst danach laesst sich eine ehrliche Endeinschaetzung zu
Teilschritt 2/2c treffen.

**Bestaetigt unangetastet:** `/home/maxx/freecad/install` (die ECHTE, taegliche Installation) wurde
in dieser gesamten Sitzung zu keinem Zeitpunkt vom Agenten beschrieben - saemtliche Tests liefen
ausschliesslich gegen `/home/maxx/freecad/install-claude-sandbox` bzw. die Clean-Build-Verzeichnisse
(`install-clean-test` etc., beide unter eigenen, separaten Pfaden). Ein am Sitzungsende
beobachteter frischer Zeitstempel auf `/home/maxx/freecad/install/bin/FreeCAD` (21:16 Uhr) stammt
laut Ruecksprache vom Nutzer selbst (`update-and-rebuild-freecad.sh`, ein regulaerer, vom Nutzer
manuell angestossener Rebuild), nicht vom Agenten.

---

## 2026-08-30: Nachfolgesitzung - Nutzerbefund "BoxB laesst sich schon wieder nicht ziehen"

Sitzungsstart: Sandbox-Rendering zuerst gegengeprueft (per `run-freecad-isolated.sh`, neu erzeugt
aus dem Skriptinhalt im obigen Abschnitt, da Scratchpad zwischen Sitzungen geleert wird) - ein
frisches `Part::Box` in einem neuen Dokument wird nach Aktivierung des MDI-Subfensters korrekt
gerendert (Screenshot gepueft). **Der RUNPATH-Fix ist weiterhin intakt, der gestrige
Sandbox-3D-View-Bug ist NICHT wieder aufgetreten.** Das heutige Symptom hat also eine andere
Ursache.

### Befund 1 (bestaetigte konkrete Ursache fuer den heutigen Bugreport): Pick-Konflikt durch verwaistes PartDesign-Objekt

Beim automatisierten Nachstellen des Drags (identische Methodik wie gestern, `test_drag_gt3.py`-
Nachfolger) auf der aktuellen, committeten `MinimalReproGrandTop.FCStd` bewegte sich `BoxB`
**ueberhaupt nicht** - alle 6 Zwischenschritte identisch mit dem Ausgangswert. Diagnose-Log zeigt
die Ursache eindeutig:

```
getSelectedObjectsWithinAssembly() - Preselection: sub='Body.Box.Face2'
  -> getMovingPartFromSel() liefert 'MinimalReproGrandTop#Body'.
canDragObjectIn3d('MinimalReproGrandTop#Body') -> true.
```

Der Mausklick an `BoxB`s erwarteter Bildschirmposition trifft ein **anderes, voellig unbeteiligtes
Objekt**: einen `PartDesign::Body` ("Körper") mit einer `PartDesign::AdditiveBox` ("Box") darin,
der DIREKT UNTER dem obersten `Assembly`-Objekt selbst haengt (nicht Teil der eigentlichen
Joint-Testkette) und sich raeumlich mit `BoxB` ueberlappt. **Dieses Objekt ist bereits im
aktuellen `git HEAD` (`27a080a`, dem einzigen Commit dieser Datei ueberhaupt) fest eingebacken** -
verifiziert per direktem Auslesen von `git show HEAD:...MinimalReproGrandTop.FCStd` und
Entpacken/Untersuchen von `Document.xml`/`GuiDocument.xml` (Visibility=true, `Expand
name="Assembly"` -> `Expand name="Body"` bestaetigt die Elternschaft). Sehr wahrscheinlich
entstanden durch versehentliches "Neuer Koerper" (Part-Design-Werkzeugleiste, sichtbar in JEDEM
Screenshot dieser gesamten Untersuchung) bei aktiver Auswahl des `Assembly`-Knotens.

**Gegenprobe (bestaetigt):** `Body.Visibility = False` gesetzt, derselbe automatisierte Drag
wiederholt - Ergebnis IDENTISCH zu gestern Nachmittags finalem, erfolgreich verifiziertem Beweis:

```
Preselection: sub='Assembly001.unterAssambly.BoxB.Edge5' -> getMovingPartFromSel() liefert 'MinimalReproGrandTop#BoxB'.
canDragObjectIn3d('MinimalReproGrandTop#BoxB') -> true.
AFTER RECOMPUTE: BoxA=(~0,~0,~0)  BoxB=(2.46751347, 1.19e-16, 3.26e-15)
```

**Damit ist der HEUTIGE Bugreport vollstaendig erklaert und ist KEIN Assembly-Solver-/Drag-Pfad-
Regressions-Bug:** ein verwaistes, testfremdes Objekt in der Testdatei verdeckt/ueberlagert `BoxB`
und faengt den Klick ab. Wahrscheinlich passiert dem Nutzer in der echten Sandbox exakt dasselbe.
**Empfehlung:** das `Body`/`Box`-Objekt aus der committeten `MinimalReproGrandTop.FCStd` entfernen,
oder beim Testen `BoxB` gezielt ueber den Baum (nicht per Blick-Klick in die 3D-Ansicht) anwaehlen.
Diese Bereinigung wurde NICHT eigenmaechtig vorgenommen (nur in Scratch-Kopien getestet) - erst auf
Nutzerwunsch, per "Bugfix vor Konstruktion"-Regel.

### Befund 2 (separat, bestaetigt, aber NICHT ursaechlich fuer den heutigen Bugreport): inkonsistente eingebackene Placement in MinimalReproSub.FCStd

Unabhaengig davon zeigt der committete `MinimalReproSub.FCStd`: `BoxB.Placement` steht auf
`Pos=(51.3373, 0, ~0)`, waehrend `Joint.Distance=0.0mm` ist - fuer einen frisch/konsistent
geloesten Zustand sollten beide zusammenpassen (Distance=0 impliziert eine Position nahe
Placement1≈Placement2, also nahe Null, nicht 51.3373). Verifiziert per `git show
HEAD:...MinimalReproSub.FCStd` -> `Document.xml`: der Wert `Px="51.3373126983642578"` ist
tatsaechlich so committet, keine Sitzungs-Verunreinigung. Korrektur getestet (BoxB.Placement auf
Identitaet gesetzt, gespeichert) - behebt die Inkonsistenz, war aber fuer den heutigen
"kann nicht gezogen werden"-Bugreport NICHT die Ursache (Drag blieb mit UND ohne diese Korrektur
gleichermassen blockiert, bis Befund 1 behoben wurde). Bleibt als separate, kleinere
Fixture-Unsauberkeit dokumentiert, niedrige Prioritaet.

Nebenbefund dabei: standalone geoeffnet (nur `MinimalReproSub.FCStd`, ohne Top/GrandTop) meldet
der allererste automatische Solve-Versuch beim Dokumenten-Oeffnen "skipped - no grounded part
found", und ein nachfolgender reiner `doc.recompute()` OHNE vorherige Property-Aenderung loest
GAR KEINEN neuen Solve-Versuch aus (kein neues "Solving..." im Log) - `recompute()` ist ein
No-Op wenn nichts als "touched" markiert ist. Erst eine tatsaechliche Property-Schreiboperation
(z.B. `boxb.Placement = ...`) markiert genug, damit der naechste `recompute()` wirklich neu loest.
Passt zum bereits dokumentierten "freecadcmd cold-load"-Bugmuster, ist vermutlich derselbe,
bereits bekannte, GUI-irrelevante Headless-Sonderfall - nicht weiter verfolgt.

### Befund 3 (NEU, potenziell die WICHTIGSTE Erkenntnis heute): drei Ebenen derselben Baugruppe liefern nach einer Joint-DOF-Aenderung DREI VERSCHIEDENE, widerspruechliche Placement-Werte

Der Nutzer meldete (ueber den Koordinator) parallel einen zweiten, unabhaengigen Befund aus dem
ECHTEN Projekt (`CNC3018_032_A_FuehrungsBaugruppe400`, `Joint009`/`Joint017` zwischen
Halterbaugruppe und Fuehrung_400): waehrend des Bearbeitens im Joint-Dialog (Live-Vorschau) sieht
alles korrekt aus, nach dem Schliessen (OK) NICHT mehr - Log zeigt beide Male sauber "computed (2
joint(s), 2 grounded part(s))"/"finished successfully", keine Fehlermeldung. Das passt vom Muster
her NICHT zu Befund 1 (kein Maus-Pick involviert), sondern eher zur separat dokumentierten
"AssemblyLink stale placement"-Bug-Kategorie. Um das direkt zu pruefen, wurde ein daten-seitiges
Analog-Experiment gemacht (ohne den echten Task-Dialog per Gui zu treiben, was headless erheblich
aufwendiger waere): `Joint.Distance` DIREKT per Python auf 20mm gesetzt (das ist exakt das, was
die Live-Vorschau des Dialogs bei einer Eingabe intern tut) - erst mit `sub_doc.recompute()`
(Analog zu "Dialog noch offen"), dann zusaetzlich mit `doc.recompute()` auf GrandTop (Analog zu
"Dialog mit OK geschlossen/committed"). Ergebnis, alle drei Ebenen SOFORT DANACH abgefragt:

```
Sub-source BoxB       = (51.3373, 0, ~0)          <- UNVERAENDERT, hat die Distance-Aenderung ueberhaupt nicht mitbekommen
GrandTop BoxB (Mirror) = (~0, ~0, ~0)              <- UNVERAENDERT, hat die Distance-Aenderung auch nicht uebernommen
Top BoxB (Zwischen-Mirror) = (93.16, 30.0, ~0)     <- HAT sich geaendert, aber auf einen dritten, nochmal ANDEREN Wert
```

**Drei Ebenen derselben Baugruppen-Hierarchie fuer DASSELBE logische Objekt (`BoxB`) liefern nach
derselben Joint-Aenderung DREI unterschiedliche, sich gegenseitig widersprechende Werte** - keiner
davon ist offensichtlich "der eine richtige". Das ist vermutlich keine Fixture-Verunreinigung wie
Befund 1/2, sondern ein echtes, tieferliegendes Synchronisationsproblem der
"Adressieren-statt-Kopieren"-Architektur aus Teilschritt 2: jede Ebene (`Sub`, `Top`, `GrandTop`)
fuehrt bei `recompute()` ihren EIGENEN, unabhaengigen Solve desselben (adressierten, nicht
kopierten) Joints durch, aber es gibt offensichtlich KEINEN Mechanismus, der das Ergebnis EINER
Ebene als kanonisch auf die anderen zurueckschreibt/mit ihnen abgleicht - jede Ebene sieht dabei
scheinbar einen leicht anderen Ausschnitt/Zustand des Gesamtsystems (z.B. je nachdem, was in dieser
Ebene selbst schon als "touched"/aktuell gilt) und loest entsprechend zu einem eigenen, von den
anderen Ebenen abweichenden Ergebnis. **Das passt strukturell sehr gut zum CNC3018-Befund**
("sieht waehrend der Bearbeitung korrekt aus, nach dem Schliessen nicht mehr" - plausibel, wenn
die im Dialog sichtbare Live-Vorschau von EINER Ebene gespeist wird, aber nach dem OK/Commit eine
ANDERE Ebene das sichtbare/gespeicherte Placement liefert) und ist eine ernsthaftere, generellere
Erkenntnis als die reinen Drag-Symptome der letzten zwei Tage.

**Status: NUR DIAGNOSTIZIERT, NICHT BEHOBEN.** Ein echter Fix wuerde vermutlich bedeuten, entweder
(a) eine einzige kanonische Solve-Instanz/Quelle der Wahrheit fuer eine gegebene, mehrfach
adressierte Baugruppe festzulegen und alle anderen Ebenen sich NUR danach richten zu lassen, statt
jede Ebene unabhaengig neu zu loesen, oder (b) nach jedem Solve explizit alle Ebenen, die dieselbe
adressierte Unterbaugruppe referenzieren, aktiv neu zu touchen/synchronisieren. Beides ist ein
groesserer Architektur-Eingriff, kein kleiner Patch - passt zur "Bugfix vor Konstruktion"-Regel:
erst weiter reproduzieren/eingrenzen (idealerweise auch mit dem echten CNC3018-Fall, per
Joint-Dialog statt Distance-Property-Injektion, um auszuschliessen dass der Task-Dialog selbst
noch einen zusaetzlichen, eigenen Bug beitraegt), bevor ein Fix versucht wird.

### KORREKTUR zu Befund 3 (direkt im Anschluss, gleicher Tag): Methodik war fehlerhaft - Kernaussage bleibt aber bestehen, jetzt sauberer belegt

Beim Weiterverfolgen wurde klar: **das obige Experiment (Setzen von `Joint.Distance = 20`) hat
gar nichts Relevantes veraendert.** Ein Blick in `JointObject.py` (`addDistanceProperty()`,
Docstring der `Distance`-Property) stellt klar: *"This is the distance of the joint. It is used
only by the Distance joint and Rack and Pinion (pitch radius), Screw and Gears and Belt (radius1)"*
- die `Distance`-Property wird fuer einen **Slider**-Joint (unser `BoxA`/`BoxB`-Joint) GAR NICHT
zur Steuerung der Kinematik verwendet, nur fuer andere Jointtypen. Bestaetigt auch im C++-Code
(`AssemblyObject::makeMbdJointOfType()`, Fall `JointType::Slider`): es wird ein leeres
`ASMTTranslationalJoint` erzeugt, ohne dass irgendein Distanzwert hineingereicht wird - die
tatsaechliche Ausgangsposition fuer den Solver kommt aus den AKTUELLEN `Placement`-Werten der
verbundenen Teile, nicht aus `Distance`. Die drei Werte in der Tabelle oben (`Sub`=51.3373,
`Top`=93.16/30, `GrandTop`=~0) waren also bereits VOR dem `Distance`-Edit exakt so vorhanden (das
zeigt ein Kontroll-Snapshot direkt nach dem Oeffnen, noch vor jeder Aenderung) - mein `Distance`-
Edit hat schlicht nichts bewirkt und war fuer einen Slider-Joint die falsche Testmethode.

**Die eigentliche, korrekte Kernaussage bleibt aber bestehen und wurde jetzt SAUBER, mit der
tatsaechlichen DOF-Steuergroesse (der `Placement`-Eigenschaft des bewegten Teils selbst, exakt wie
sowohl der interaktive 3D-Drag als auch vermutlich ein Slider-Dialog es tun), neu belegt:**

```
A) Frisch geoeffnet (BASELINE, nichts editiert):
   GrandTop.BoxB = (~0, ~0, ~0)         Top.BoxB = (93.16, 30, ~0)        Sub.BoxB = (51.3373, 0, ~0)
   -> alle drei bereits unterschiedlich, VOR jeder Interaktion (reine Oeffnen+Solve-Divergenz)

B) GrandTop.BoxB.Placement direkt um +20mm entlang der Schiebeachse verschoben (Analog zu echtem
   Drag/Dialog-Eingabe), NOCH KEIN recompute():
   GrandTop.BoxB = (20, ~0, ~0)         Top.BoxB = UNVERAENDERT (93.16,30,~0)   Sub.BoxB = UNVERAENDERT

C) doc.recompute() auf GrandTop (Analog zu Drag-Loslassen / Dialog-OK):
   GrandTop.BoxB = (20, ~-1.7e-15, ~-1.8e-15)   <- korrekt achsengebunden geloest (Teilschritt 2/2c
                                                     bestaetigt sich hier erneut)
   Top.BoxB = WEITERHIN UNVERAENDERT (93.16, 30, ~0)
   Sub.BoxB = WEITERHIN UNVERAENDERT (51.3373, 0, ~0)

D) ZUSAETZLICH Top und Sub explizit einzeln recompute()t:
   Alle drei Werte bleiben EXAKT wie in (C) - auch ein gezielter, expliziter Recompute von Top/Sub
   holt die Aenderung NICHT nach.
```

**Damit ist der eigentliche Kern von Befund 3 jetzt sauber, mit der richtigen Methode, belegt:**
GrandTop's eigener Solve aktualisiert korrekt NUR seine eigene Sicht auf `BoxB` (das ist konsistent
mit dem bereits verifizierten Teilschritt 2/2c) - aber diese Korrektur wird **zu keinem Zeitpunkt**
auf `Top`s oder `Sub`s EIGENE, lokal gespeicherte `Placement`-Werte zurueckgeschrieben, selbst nicht
nach einem gezielten, expliziten `recompute()` genau dieser Dokumente. Diese bleiben dauerhaft bei
ihrem eigenen, ZUERST (beim allerersten Oeffnen) berechneten - und bereits untereinander
abweichenden - Stand eingefroren. Das erklaert das CNC3018-Verhaltensmuster ("waehrend der
Bearbeitung sieht alles korrekt aus, nach dem Schliessen nicht mehr") strukturell plausibel, WENN
die im Dialog sichtbare/gespeicherte Ansicht von einer anderen Ebene gespeist wird als der,
die den Drag/die Eingabe tatsaechlich korrekt geloest hat - bleibt aber weiterhin eine Hypothese
fuer den CNC3018-Fall selbst (dort mit vermutlich anderen Jointtypen als Slider), keine 1:1
uebertragene Tatsache.

**Praktische Konsequenz fuer die Interpretation von Befund 3 insgesamt:** die Kernbeobachtung
("mehrere Ebenen derselben adressierten Baugruppe divergieren dauerhaft, kein Synchronisations-
Mechanismus") ist jetzt mit der methodisch korrekten Vorgehensweise bestaetigt, nicht nur mit der
fehlerhaften `Distance`-Injektion. Die vorgeschlagenen naechsten Schritte unten bleiben unveraendert
gueltig.

### Naechster konkreter Schritt

1. Den echten Joint-Bearbeiten-Dialog-Fall (nicht nur die Property-Injektion) headless oder per
   Live-Test nachstellen: Dialog oeffnen, Wert aendern (Live-Vorschau beobachten), OK klicken,
   DANACH an allen drei Ebenen (Sub/Top/GrandTop bzw. im echten Projekt: die entsprechenden
   Zwischenebenen) dieselben Placement-Werte vergleichen wie in Befund 3.
2. Klaeren, ob es EINE bevorzugte/kanonische Ebene gibt (z.B. immer die aeusserste, aktiv
   editierte), deren Ergebnis fuer die Anzeige/Speicherung massgeblich sein sollte, und ob/wie die
   anderen Ebenen aktuell (fehlerhaft) daran vorbei ihre eigenen, abweichenden Werte in die
   sichtbaren Properties schreiben.
3. Erst danach einen Fix-Ansatz entwerfen - dies ist wahrscheinlich groesser als Teilschritt 2/2c.

### Live vom Nutzer bestaetigt (2026-08-30, echter manueller Drag statt Skript-Injektion)

Zwei Vorbedingungen mussten dafuer erst manuell behoben werden (beide reine Laufzeit-/Sitzungs-
Artefakte, NICHT in den committeten Dateien): (1) `BoxB.ViewObject.Selectable` stand in
`MinimalReproSub` auf `False` (im committeten Blob korrekt `true` - vermutlich FreeCADs eigenes
Verhalten, das Objekte ausserhalb des gerade aktiven Bearbeiten-Kontexts beim Wechseln zwischen
verschachtelten Edit-Sessions non-selectable setzt und nicht zurueckdreht), (2) dasselbe fuer
BoxB in GrandTop - beide per Python live auf `True` zurueckgesetzt (fuer alle Objekte in allen drei
Dokumenten, sicherheitshalber).

Danach echter manueller Maus-Drag von BoxB in GrandTop, sofort danach alle drei Ebenen abgefragt:

```
MinimalReproGrandTop Vector(-3.734313964843803, 3.2298967246899077e-16, 2.4698859019752063e-14)
MinimalReproTop      Vector(93.15994071960449, 29.999999999999993, 3.1872600636767494e-15)
MinimalReproSub      Vector(18.652050971984863, -1.6151856500403399e-15, 6.000855880838115e-15)
```

**Bestaetigt Befund 3 vollstaendig mit einem ECHTEN interaktiven Drag, nicht nur Skript-Injektion:**
GrandTop UND Sub loesen jeweils fuer sich achsengebunden (Y/Z ~0), aber zu ZWEI verschiedenen
X-Werten (-3.73 vs. 18.65) - Top bleibt exakt beim allerersten Baseline-Wert (93.16/30.0)
eingefroren, komplett unveraendert durch die Interaktion. Drei Ebenen, drei sich widersprechende
Ergebnisse fuer dasselbe logische BoxB, genau wie in der Skript-Version - jetzt aber mit einem
echten Maus-Drag reproduziert. Nutzerentscheidung: als bestaetigter, dokumentierter Stand stehen
lassen, kein Fix-Versuch in dieser Sitzung - naechste Schritte bleiben wie oben beschrieben.

## 2026-09-02: Fortsetzung - Teilschritt 2/2b/2c im Baum wiederhergestellt + echte Root Cause fuer Befund 3 gefunden (Fix noch NICHT vollstaendig)

Nutzerauftrag: "weiter am Fix fuer #31855 [gemeint: #32171] arbeiten. Adressieren statt
kopieren." Erste Ueberraschung: `freecad-source` hatte KEINE der Teilschritt-2/2b/2c-Aenderungen
mehr im Baum (`resolvePartForMbD`, `jointNestingPrefixMap`, `resolveJointReference` - alles
fehlte komplett, nur noch als eigenstaendige `.patch`-Dateien vorhanden, nie in
`freecad-assembly-jointobject.patch` gefaltet). Vermutlich durch einen der vielen
Clean-Rebuild-Tests der Sandbox-3D-View-Untersuchung verlorengegangen. Alle vier Patches
(`step2-resolve-joint-reference`, `step2b-getjoints-nesting-prefix`, `step2c-
removeunconnectedjoints-addressing`) von Hand neu in den aktuellen Baum eingearbeitet (rohes
`git apply` schlug wegen zwischenzeitlicher Aenderungen fehl), `step3-objectpartmap-addressing`
BEWUSST NICHT uebernommen (bleibt reine, aktuell wirkungslose Infrastruktur - subPath fliesst
nirgends ein, siehe eigener Kommentar im Patch selbst).

**Befund 3 sauber reproduziert** mit der wiederhergestellten Infrastruktur (identisch zum
2026-08-30-Befund: GrandTop loest korrekt achsengebunden, Top und Sub bleiben beide dauerhaft
auf ihrem jeweiligen Baseline-Wert eingefroren).

**Root Cause gefunden** (C++-Instrumentierung von `resolvePartForMbD()`/`setNewPlacements()`,
danach wieder entfernt): `AssemblyUtils::resolveJointReference()` (Teilschritt 2) UND das
bereits laenger bestehende Vorbild `getMovingPartFromSel()` haben BEIDE denselben Fehler beim
Dokumentwechsel waehrend des Sub-Pfad-Walks - der Code prueft `obj->isLink()`, um bei einem
Cross-Document-Sprung `doc` auf das verlinkte Dokument umzuschalten. **`Assembly::AssemblyLink`
erbt aber von `App::Part`, NICHT von `App::Link`** - `isLink()` ist fuer eine verschachtelte
AssemblyLink also IMMER `false`, `doc` bleibt faelschlich beim AUFRUFENDEN (aeusseren)
Dokument stehen. Der naechste Pfadabschnitt (z.B. "BoxB") findet dadurch ein gleichnamiges
Objekt im FALSCHEN, aeusseren Dokument - naemlich die alte, vom noch aktiven Kopier-Mechanismus
(`AssemblyLink::synchronizeComponents()`) dort gespiegelte Kopie - statt des echten,
tiefer liegenden Zielteils. Live bewiesen: vor dem Fix loeste
`resolvePartForMbD(..., nestingPrefix="Assembly001.unterAssembly.")` faelschlich zu
`MinimalReproGrandTop#BoxB` auf (der lokale Mirror), nach dem Fix korrekt zu
`MinimalReproSub#BoxB` (das echte Teil).

**Fix implementiert:** in `resolveJointReference()` zusaetzlich zum bestehenden
`obj->isLink()`-Zweig ein `freecad_cast<Assembly::AssemblyLink*>(obj)`-Zweig ergaenzt, der bei
Erfolg `doc = assemblyLink->getLinkedAssembly()->getDocument()` setzt - funktional das fehlende
Aequivalent zum bestehenden `App::Link`-Zweig, nur ueber `getLinkedAssembly()` statt
`getLinkedObject()`. `getMovingPartFromSel()` (das GUI-Auswahl-Vorbild mit demselben Fehler)
BEWUSST NICHT mitgeaendert - hoeheres Risiko/breiterer Blast-Radius (aktiv genutzter
GUI-Auswahl-Pfad bei jeder Joint-Erstellung), separate Entscheidung noetig.

**ABER: Fix allein reicht nicht - deckt eine ZWEITE, tiefere Inkonsistenz auf.** Nach dem Fix
werden ALLE Joints (auch die simplen, EINSTUFIGEN wie BoxC-BoxD und BoxA-BoxD auf Top-Ebene,
die VOR dem Fix noch "durch Zufall" funktionierten) von `removeUnconnectedJoints()` als "nicht
erreichbar" verworfen. Ursache: `getGroundedParts()`/`fixGroundedParts()` (unveraendert, nutzt
weiterhin `getAssemblyComponents()` - liest NUR lokale `Group`-Mitglieder INNERHALB des
jeweils aufrufenden Dokuments, findet also die lokalen MIRROR-Kopien) und
`resolvePartForMbD()` (jetzt korrekt, liefert die ECHTEN, tief verschachtelten Objekte in
Sub/Tops EIGENEN Dokumenten) arbeiten in ZWEI VERSCHIEDENEN IDENTITAETSRAEUMEN - der
Erreichbarkeits-Vergleich (`isObjInSetOfObjRefs`) findet dadurch nie eine Uebereinstimmung.
Vor dem AssemblyLink-Fix "funktionierte" das nur zufaellig, weil BEIDE Seiten (Grounding UND
Referenzaufloesung) auf dieselben lokalen Mirror-Kopien kollabierten - kein echtes Funktionieren,
zwei sich gegenseitig aufhebende Bugs.

**Status:** C++-Fix fuer `resolveJointReference()` ist im Baum, kompiliert sauber, in der
Sandbox getestet (0 Regressionen bei Symbol-/Ladeverhalten) - aber NICHT ausreichend fuer einen
funktionalen Fix von Befund 3, und fuer die einstufigen Faelle sogar ein Rueckschritt gegenueber
dem vorherigen (zufaelligen) Verhalten. NICHT auf die echte Installation deployt, NICHT
committed. Naechster, groesserer Schritt: `getGroundedParts()`/`getAssemblyComponents()` auf
dieselbe adressierungsbewusste Logik umstellen wie `resolvePartForMbD()` (oder umgekehrt: einen
gemeinsamen Normalisierungsschritt einfuehren, der BEIDE Seiten vor dem Vergleich in denselben
Identitaetsraum bringt) - deutlich groesserer Eingriff als die bisherigen Teilschritte, noch
nicht begonnen.

**Wichtiger Nebenbefund waehrend dieser Sitzung (operational, nicht Assembly-spezifisch):** ein
per `cp` (statt `cmake --install`) nach `install/lib` deploytes `.so` behaelt sein RUNPATH aus
dem Build-Ordner bei - jeder weitere `cmake --build` im Build-Ordner bricht dadurch sofort die
ECHTE, laufende Installation, OHNE dass `install/` je direkt anfasst wird. Live beim Nutzer
aufgetreten ("keine Menue eintraege fuer Assembly und keine Icons"), mit `patchelf --set-rpath`
behoben (kein Neubau noetig). Neues Skript `/home/maxx/freecad/deploy-so.sh` kombiniert `cp`
+ RUNPATH-Fix in einem Schritt, ersetzt ab sofort jedes rohe `cp` fuer `.so`-Deploys - siehe
[[project_fcproject_runpath_build_dir_hazard]]-Memory fuer die volle Herleitung.

## 2026-09-02 (spaeter am selben Tag): Teilschritt 2e - Identitaetsraum-Angleichung umgesetzt, Erreichbarkeits-Regression behoben, aber ein NEUER, tieferer Befund gefunden

Nutzerauftrag im Anschluss an den Tooltip-Zwischenauftrag: "committen und dann weitermachen an
rigid" - gemeint war der oben als naechster Schritt notierte `getGroundedParts()`-Fix.

### Umgesetzt: `canonicalizeForMbD()`

Neue Methode `AssemblyObject::canonicalizeForMbD(App::DocumentObject* obj)` (AssemblyObject.h/
.cpp), die genau den oben beschriebenen Identitaetsraum-Bruch behebt: liegt `obj` als LOKALE
Spiegel-Kopie irgendwo im eigenen `Group`-Baum dieser AssemblyObject-Instanz (Top-Down-Suche,
nicht InList-Aufstieg - Begruendung s.u.), wird derselbe Namenspfad stattdessen durch die ECHTEN,
ueber `AssemblyLink::getLinkedAssembly()` erreichten verschachtelten AssemblyObject-Instanzen neu
aufgeloest. Liegt `obj` nicht im lokalen Baum (schon ein echtes Objekt, oder unzusammenhaengend),
wird es unveraendert zurueckgegeben - garantiert Rueckwaertskompatibilitaet fuer jeden
nicht-verschachtelten Fall.

Eingehaengt an zwei Stellen:
1. **`getMbDData()`** (zentraler Kanonisierungs-Punkt, ganz oben in der Funktion): jeder Aufrufer
   (Joints via `resolvePartForMbD()`, geerdete Teile via `fixGroundedPart()`, Rigid-Cluster-
   Reprasentanten, ...) landet dadurch garantiert auf demselben Pointer fuer ein und dasselbe
   reale Teil - vorher legte `objectPartMap` (pointer-keyed) fuer den lokalen Mirror-Pointer und
   den echten Pointer ZWEI GETRENNTE MbD-Teile an, wodurch ein geerdetes Teil und der es
   bewegende Joint nie im selben Constraint-Graphen landeten. Das ist vermutlich die eigentliche
   numerische Kernursache von Befund 3, nicht nur die Erreichbarkeits-Filterung.
2. **`removeUnconnectedJoints()`**: `groundedObjs` wird vor der Traversal komplett kanonisiert
   (`canonicalGroundedObjs`), damit der Traversal-Startpunkt im selben Identitaetsraum liegt wie
   die per `resolvePartForMbD()` aufgeloesten Joint-Endpunkte in `getConnectedParts()`.

**Bug waehrend der Umsetzung gefunden und behoben:** der erste Implementierungsversuch hat den
lokalen Namenspfad per InList-AUFSTIEG von `obj` aus eingesammelt (`inList.front()`), analog zu
`getContext()`/`getJointContextName()`. Live per temporaerem Debug-Logging beobachtet: fuer
`GrandTop#BoxC` lieferte das faelschlich `Top#Joint` statt `Top#BoxC` - `BoxC`s `InList` enthaelt
naemlich NICHT NUR seinen Group-Elternknoten, sondern JEDES Objekt, das es per irgendeiner
Property referenziert, unter anderem den Joint, der `BoxC` als `Reference1`/`Reference2` haelt -
und dessen Position im `InList`-Vector ist nicht deterministisch der Elternknoten. Ersetzt durch
eine eindeutige Top-Down-Suche (`findLocalGroupPath()`, neue anonyme Namespace-Funktion direkt
vor `canonicalizeForMbD()`) ueber `Group.getValues()`/`AssemblyLink::Group.getValues()` -
Group-Mitgliedschaft ist unzweideutig. **Nebenbefund:** derselbe InList-Aufstieg-Bug steckt
vermutlich auch in `getContext()` (JointObject.py) und `getJointContextName()`
(AssemblyObject.cpp) - dort aber nur diagnostisch (Konsolen-Meldungen/Tooltip-Text) genutzt,
also niedrigere Prioritaet, nicht in dieser Sitzung angefasst.

### Zusaetzlich noetig: `getGroundedParts()` rekursiv ergaenzt

Nach dem `canonicalizeForMbD()`-Fix zeigte ein Live-Test (`test_befund3_fix.py`, 3-Boxen-Repro):
die zuvor kaputte Erreichbarkeit fuer die EINSTUFIGEN Top-Joints (BoxC-BoxD, BoxA-BoxD) war
behoben ("computed (2 joint(s)..." statt "0 joint(s)") - aber der ZWEI Ebenen tief verschachtelte
Sub-Joint (BoxA-BoxB) wurde weiterhin als "nicht erreichbar" entfernt. Debug-Logging zeigte:
`getGroundedParts()` von GrandTop lieferte ueberhaupt nur `Origin` + `BoxC` - Subs eigenes
geerdetes `BoxA` (2 Ebenen tiefer) tauchte gar nicht erst auf. Der bestehende Mechanismus
(`isReadOnly()`-Flag der lokalen `Placement`-Spiegel-Kopie ueber `getAssemblyComponents()`)
synchronisiert das Grounding-Flag offenbar nur EINE Ebene tief zuverlaessig, nicht rekursiv durch
mehrere Verschachtelungsstufen.

Fix: `getGroundedParts()` ruft jetzt zusaetzlich rekursiv `nestedAssembly->getGroundedParts()`
fuer jede FLEXIBLE (nicht rigide) verschachtelte `AssemblyLink` auf (via `getSubAssemblies()` +
`getLinkedAssembly()`, exakt dasselbe Muster wie `getJoints()`s bestehende `subJoints`-
Rekursion) und mischt das Ergebnis in die eigene `groundedSet` ein - diese Objekte liegen bereits
im "echten" Identitaetsraum der jeweils rekursiv aufgerufenen Instanz und brauchen keine weitere
Uebersetzung.

**Ergebnis nach beiden Fixes (verifiziert):** `test_befund3_fix.py` zeigt fuer GrandTop
`getGroundedParts()` jetzt korrekt 6 Eintraege (u.a. `MinimalReproSub#BoxA` mit Fullname-Praefix,
nicht mehr nur `MinimalReproGrandTop#Origin`/`BoxC`), und `'MinimalReproGrandTop#Assembly'
computed (3 joint(s), 6 grounded part(s))` - alle drei Joints (Top-BoxC/BoxD, Top-BoxA/BoxD,
Sub-BoxA/BoxB) werden jetzt korrekt als erreichbar erkannt und OHNE JEDE "entfernt"-Meldung in
den Solve aufgenommen. **Die urspruengliche Erreichbarkeits-Regression aus dem vorigen
Sitzungsabschnitt ist damit vollstaendig behoben**, klar besser als der Stand vor dieser Sitzung
(der zwei-Ebenen-Sub-Joint war dort NIE erreichbar, nicht mal durch Zufall).

### NEUER, noch offener Befund: geloeste Placements propagieren trotzdem nicht zurueck

Trotz korrekter Erreichbarkeit blieb `Sub.BoxB.Placement` in `test_befund3_fix.py` nach einer
manuellen `+20mm`-Verschiebung von `GrandTop.BoxB` UND mehreren `recompute()`-Aufrufen exakt
unveraendert bei seinem urspruenglichen Wert (51.337, 0, 0) - `setNewPlacements()` schreibt zwar
nachweislich (Code gelesen, nicht mehr per Instrumentierung verifiziert) direkt auf den
kanonisierten - bei einem tief verschachtelten Teil also ECHTEN - `DocumentObject*`-Pointer via
`objectPartMap`. **Vermutliche Ursache:** jede Verschachtelungsebene loest weiterhin
UNABHAENGIG VONEINANDER (eigene `AssemblyObject::solve()`-Instanz, eigener `mbdAssembly`, eigene
`objectPartMap`) - durch Teilschritt 2b rekursiv (`getJoints(subJoints=true)`) bekommt aber JEDE
Ebene (nicht nur die aeusserste) inzwischen den KOMPLETTEN, bis in die tiefste Verschachtelung
reichenden Joint-Graphen zu sehen und loest ihn EIGENSTAENDIG fuer sich. Im Log sind pro
`recompute()`-Durchlauf mehrere `'...#Assembly' computed (...)`-Meldungen zu sehen (z.B. sowohl
`MinimalReproTop#Assembly` als auch `MinimalReproGrandTop#Assembly`) - jede schreibt potenziell
unterschiedliche numerische Ergebnisse auf DIESELBEN realen Objekte (z.B. `Sub.BoxB`), je nachdem
von welcher Ebene aus gerechnet wurde (andere Ankerpunkte/Ausgangswerte). Welche Ebene zuletzt in
der Dokument-Abhaengigkeits-Reihenfolge drankommt, gewinnt - nicht deterministisch aus Nutzersicht,
und die vom Nutzer tatsaechlich bearbeitete Ebene (hier: GrandTop, wo die `+20mm`-Aenderung
gemacht wurde) gewinnt NICHT zuverlaessig.

**Das ist vermutlich die eigentliche Wurzel von Befund 3** (drei Ebenen liefern drei
widerspruechliche Werte) - nicht (nur) die Identitaetsraum-Trennung von heute, sondern
grundsaetzlich MEHRERE, redundante, einander ueberschreibende Solver-Laeufe fuer denselben
physischen Teilbaum. Ein echter Fix braucht vermutlich eine Entscheidung, WELCHE Ebene fuer einen
gegebenen Teilbaum "die massgebliche" ist (z.B.: eine verschachtelte `AssemblyObject`-Instanz
sollte ihren eigenen `solve()` GAR NICHT erst ausfuehren, wenn sie bereits transitiv Teil eines
groesseren, von einer aeusseren Ebene aus geloesten Baums ist) - ein deutlich groesserer,
invasiverer Eingriff (`execute()`/Recompute-Trigger-Logik) als die heutigen Aenderungen, der
sorgfaeltig gegen den Einzel-Baugruppen-Fall, rigide verschachtelte Baugruppen und den
"Sub-Dokument alleine geoeffnet"-Fall abgesichert werden muss. **Nicht in dieser Sitzung
begonnen** - Nutzerentscheidung noetig, ob/wie weitergemacht wird.

**Status Ende Sitzung:** `canonicalizeForMbD()` + `getGroundedParts()`-Rekursion sind im Baum,
kompilieren sauber, in der Sandbox getestet (Erreichbarkeits-Regression nachweislich behoben,
0 Regressionen fuer den vorher schon funktionierenden Ein-Ebenen-Fall). NICHT auf die echte
Installation deployt, NICHT committed (arbeitet noch nicht Ende-zu-Ende korrekt - der neue
Befund oben bleibt bestehen). Testskript: `test_befund3_fix.py` im Scratchpad dieser Sitzung,
Repro-Dateien: `patches/bugreport-nested-flex-joint-detach/MinimalRepro{GrandTop,Top,Sub}.FCStd`.
Als Patches gesichert: `patches/freecad-assembly-addressing-utils.patch` (neu, AssemblyUtils.cpp/
.h), `freecad-assembly-jointobject.patch` (AssemblyObject.cpp/.h-Teil aktualisiert),
`freecad-assembly-grounded-joint-nested-flex.patch` (AssemblyLink.cpp/.h-Teil aktualisiert) -
alle per Worktree-Test gegen einen frischen Checkout verifiziert (Ergebnis byte-identisch zur
Arbeitskopie). Liegen auf `main` in FCProject, uncommitted (wie alle Feature-Patches dort).

**Nutzerentscheidung (2026-09-02):** fuer heute hier stoppen, der verbleibende Fix ist deutlich
groesser/riskanter. Vom Nutzer explizit als offene Fragen fuer die naechste Sitzung genannt (noch
nicht recherchiert, nur notiert):
- **Threading/Nebenlaeufigkeit:** laufen die mehreren, sich ueberschreibenden `solve()`-Aufrufe
  der verschiedenen Ebenen tatsaechlich in verschiedenen Threads, oder alle im selben (nur
  zeitlich nacheinander, per Dokument-Abhaengigkeits-Reihenfolge)? Relevant fuer die Wahl des
  Fixes - ein reines Reihenfolge-/Ownership-Problem (single-threaded) braucht einen anderen
  Loesungsansatz als ein echtes Nebenlaeufigkeits-Problem (Locking/Reentrancy).
- **Solve-Richtung:** sollte die Loesung "von oben nach unten" propagieren (die AEUSSERSTE Ebene
  ist massgeblich, loest den kompletten Baum, innere Ebenen uebernehmen nur das Ergebnis) oder
  "von unten nach oben" (die INNERSTE/urspruenglich bearbeitete Ebene ist massgeblich, das
  Ergebnis wird nach aussen durchgereicht)? Nutzer unsicher, was hier besser ist - noch keine
  Antwort, direkt zu Beginn der naechsten Sitzung klaeren, bevor mit der Umsetzung begonnen wird.

## 2026-09-03: Frage 1 (Threading) geklärt - reines Ownership-/Reihenfolge-Problem, keine Nebenläufigkeit

Ablaufdiagramm erstellt (`assembly-solver-sandbox/assembly-solve-sequence-fig1-5.html`, 5 Figuren:
Haupt-solve()-Sequenz, getJoints()-Rekursion, getGroundedParts()-Rekursion,
canonicalizeForMbD()-Identitaetsangleichung, das ungeloeste Mehrfach-Solve-Problem als
Sequenzdiagramm) - auch als Claude-Artifact veroeffentlicht.

Danach die Threading-Frage aus dem vorigen Abschnitt im echten FreeCAD-Core-Quellcode
nachverfolgt (nicht nur aus dem Log geschlossen): **FreeCAD hat tatsaechlich einen echten
Hintergrund-Recompute-Worker-Thread** (`Application::recomputeWorker()`, `src/App/
Application.cpp`) - ein dediziertes `std::thread`-Mitglied, seit Anwendungsstart laufend, mit
eigener Request-Queue (`_recomputeRequests`), standardmaessig aktiv
(`EnableAsyncRecompute=true`). Jedes `App::DocumentObject` ist per virtueller Methode
`canRecomputeOnWorker()` (Default `true` in `DocumentObject.h`) grundsaetzlich fuer diesen
Worker "freigegeben" - `Assembly::AssemblyObject` ueberschreibt das NICHT, waere also potenziell
worker-faehig.

**Aber:** dieser Worker wird nur ueber `Application::queueRecomputeRequest()` erreicht, und der
EINZIGE Aufrufer im gesamten Quellbaum ist `Gui/CommandDoc.cpp` - die "Dokument neu berechnen"
(Refresh)-Toolbar-Aktion. Der fuer diese Untersuchung eigentlich relevante Pfad -
`assembly.recompute(True)` in `CommandSolveAssembly.py` (Taste Z, der Weg, den jeder explizite
Solve in diesem Codebereich nimmt) - ruft stattdessen `DocumentObjectPy::recompute()` auf
(`App/DocumentObjectPyImp.cpp`), das DIREKT `DocumentObject::recomputeFeature()` ->
`Document::_recomputeFeature()` aufruft (`App/Document.cpp`) - komplett synchron im
aufrufenden Thread, OHNE die Queue/den Worker-Thread je zu beruehren. Auch `Document::recompute()`
selbst (Python `doc.recompute()`, `DocumentPyImp.cpp`) geht direkt, synchron, keine Queue.
Der interne `topoSortedObjects`-Schleifendurchlauf in `Document::recompute()` ist eine
gewoehnliche sequenzielle `for`-Schleife, keine Parallelisierung (`isFineGrainedRecomputeEnabled()`
steuert nur, WELCHE Objekte als "PendingRecompute" markiert werden, nicht ob parallel gearbeitet
wird).

**Antwort auf Nutzerfrage 1:** fuer den hier untersuchten Fall (Z-Taste / jeder programmatische
`recompute()`-Aufruf) ist es garantiert EIN Call-Stack, sequenziell, kein zweiter Thread beteiligt
- Fig. 5s Sequenzdiagramm-Darstellung war korrekt, jetzt aber durch den tatsaechlichen
Bindungs-Pfad belegt statt nur durch Log-Indizien. Der echte Async-Worker existiert zwar, ist
aber nur ueber die GUI-"Refresh"-Schaltflaeche erreichbar - ein separater, hier nicht relevanter
Pfad (fuer eine vollstaendige Absicherung eines kuenftigen Fixes trotzdem im Hinterkopf behalten:
falls `Assembly::AssemblyObject` jemals `canRecomputeOnWorker()` NICHT explizit auf `false` setzt
und jemand jenen Refresh-Knopf place benutzt, waere die Async-Variante prinzipiell erreichbar -
aktuell aber kein beobachteter Pfad). **Damit ist der verbleibende Fix ein reines
Ownership-/Reihenfolge-Problem** (wer darf fuer einen gegebenen Teilbaum schreiben), kein
Locking-/Nebenlaeufigkeits-Problem - vereinfacht die Loesungssuche fuer Frage 2 (oben/unten)
erheblich, da kein Thread-Safety-Aspekt mehr beruecksichtigt werden muss.

Fig.-5-Bildunterschrift im Artifact/HTML entsprechend mit dem konkreten Bindungs-Pfad
aktualisiert. Frage 2 (welche Ebene ist massgeblich) bleibt offen, Nutzerentscheidung noetig,
bevor mit der Umsetzung begonnen wird.

## 2026-09-03: Frage 2 beantwortet ("von oben nach unten") - Fix umgesetzt und verifiziert

Nutzerentscheidung: "von oben nach unten besser, oder einzig moeglich" - die aeusserste,
umfassendste AssemblyObject-Instanz ist massgeblich fuer einen gegebenen Teilbaum, innere Ebenen
loesen sich selbst nicht mehr eigenstaendig.

**Umsetzung:** neue Methode `AssemblyObject::isNestedUnderFlexibleParent() const`
(AssemblyObject.h/.cpp, neben `getSubAssemblies()`) - das Gegenstueck dazu: durchsucht
`getInList()` (funktioniert dank `App::PropertyXLink` dokumentuebergreifend, bestaetigt durch
Code-Lektuere von `AssemblyLink::LinkedObject`) nach einer FLEXIBLEN (nicht rigiden)
`AssemblyLink`, die auf diese Instanz zeigt. `AssemblyObject::execute()` ruft `solve()` nur noch
auf, wenn das NICHT der Fall ist; andernfalls eine informative Konsolen-Meldung ("skipped its own
solve() - nested under a flexible parent assembly...").

**Warum das reicht, ohne Rekursionstiefe explizit zu verfolgen:** die Regel "loese dich nicht
selbst, wenn IRGENDEINE flexible AssemblyLink auf dich zeigt" kaskadiert von selbst korrekt durch
beliebig tiefe Verschachtelung - bei GrandTop->Top->Sub sieht sowohl Sub (Top zeigt flexibel auf
Sub) als auch Top (GrandTop zeigt flexibel auf Top) eine eigene flexible Elternebene und loesen
beide nicht selbst; nur GrandTop hat keine eigene Elternebene und loest tatsaechlich - dessen
bereits bestehende `getJoints()`/`getGroundedParts()`-Rekursion (Teilschritt 2/2e) erreicht dabei
automatisch BEIDE tieferen Ebenen in einem einzigen, konsistenten Solve.

**Bewusst nicht betroffen:** interaktives Ziehen. `ViewProviderAssembly.cpp` ruft
`preDrag()`/`doDragStep()`/`solve()` direkt auf der gerade im Bearbeiten-Modus aktiven Instanz
auf (Code gelesen, bestaetigt: kein Umweg ueber `execute()`) - laufende, direkte Interaktion
bleibt exakt wie bisher, nur das Ergebnis nach einem vollstaendigen Recompute (Taste Z, Dokument
neu berechnen, Schliessen+Neuladen) folgt jetzt der neuen Regel.

**Bekannte, akzeptierte Einschraenkungen** (nicht blockierend, nicht heute angegangen):
- `syncGroundedJoints()` (haelt `GroundedJoint`-Objekte mit dem `isReadOnly()`-Flag synchron) laeuft
  fuer eine dauerhaft verschachtelte, selbst nie solvende Instanz nicht mehr - rein kosmetische
  Staleness (verwaiste `GroundedJoint`-Objekte, falls jemand direkt in der Sub-Datei Erdung
  umschaltet), keine Auswirkung auf die numerische Loesung (die liest `Placement.isReadOnly()`
  direkt, nicht ueber `GroundedJoint`-Objekte).
- Rigid Groups, deren Mitglieder in einer verschachtelten flexiblen AssemblyLink liegen, werden
  weiterhin nicht rekursiv aufgeloest (`rebuildRigidClusters()` bleibt pro Instanz lokal) - Rigid
  Group ist laut Nutzer ohnehin separat als buggy bekannt, bewusst nicht mitgeloest.

**Verifiziert (`test_befund3_fix.py`/`test_befund3_fix2.py`/`test_one_level.py`/
`test_standalone_sub.py`, 3-Boxen-Repro, `install-claude-sandbox`):**
- **2 Ebenen (GrandTop->Top->Sub):** Sub UND Top melden "skipped its own solve()", nur GrandTop
  loest (`computed (3 joint(s), 6 grounded part(s))`, keine "entfernt"-Meldung mehr). Das ECHTE,
  tief verschachtelte `Sub#BoxB` (nicht eine lokale Spiegel-Kopie - die vorherige Testmethode vom
  02.09. testete versehentlich ein unabhaengiges, gleichnamiges Testobjekt in GrandTops eigenem
  Dokument, keine echte Verbindung) absichtlich abseits der Slider-Achse verschoben (+20/+5/+3) ->
  nach `GrandTop.recompute(True)`: Y/Z korrekt auf ~0 zurueckgeschnappt, X (die erlaubte
  Achsen-Bewegung) korrekt erhalten (71.337 = 51.337+20). **Das ist der erste Nachweis, dass eine
  Aenderung tatsaechlich Ende-zu-Ende durch alle Verschachtelungsebenen hindurch korrekt geloest
  wird - der eigentliche Kern von Befund 3.**
- **1 Ebene (Top->Sub, `MinimalReproTop.FCStd` allein geoeffnet):** Sub meldet "skipped", Top
  loest, gleiches korrektes Achsen-Verhalten (63.337 = 51.337+12, Y/Z auf ~0).
  Verschachtelung nicht regressiv.
- **0 Ebenen (`MinimalReproSub.FCStd` allein geoeffnet, kein Top/GrandTop geladen):** KEINE
  "skipped"-Meldung, Sub loest sich normal selbst (`computed (1 joint(s), 2 grounded part(s))`),
  korrektes Achsen-Verhalten (66.337 = 51.337+15). Bestaetigt: die Sperre greift nur, wenn eine
  flexible Elternebene TATSAECHLICH geladen ist, kein falsch-positiver Fall.

Als Patch gesichert (`freecad-assembly-jointobject.patch`, AssemblyObject.cpp/.h-Teil erneut
aktualisiert, per Worktree-Test gegen einen frischen Checkout verifiziert - alle 7 Feature-Patches
zusammen wenden sauber an, Ergebnis byte-identisch zur Arbeitskopie). NICHT auf die echte
Installation deployt, NICHT committed - Nutzer-Bestaetigung/Live-Test noch ausstehend.

## 2026-09-03 (Fortsetzung): getMovingPartFromSel()/isPartConnected() gefixt und verifiziert, aber tieferer Befund - lokale Spiegel-Objekte werden bei flexiblen Baugruppen nie mit dem Solver-Ergebnis synchronisiert

Live-Test des Nutzers (Maus-Drag in GrandTop und Top, jeweils per Doppelklick in den
Bearbeiten-Modus) zeigte: Boxen lassen sich weiterhin frei in jede Richtung ziehen, trotz der
gestern verifizierten Solver-Fixes.

**Zwei echte, verifizierte Bugs gefunden und behoben** (per Diagnose-Logging live nachvollzogen,
Logs unter `~/freecad/logs/freecad-sandbox-*.log`):

1. **`getMovingPartFromSel()`** (`AssemblyUtils.cpp`) hatte denselben `isLink()`-Bug wie
   `resolveJointReference()` - beim Passieren einer verschachtelten FLEXIBLEN `AssemblyLink`
   wurde `doc` nie umgeschaltet, der Namens-Walk suchte danach im FALSCHEN (aeusseren) Dokument
   weiter. Fix: beim non-rigid-AssemblyLink-`continue()` zusaetzlich `doc =
   asmLink->getLinkedAssembly()->getDocument()`. **Verifiziert per Log:** fuer eine 2 Ebenen tief
   verschachtelte Referenz (`Assembly001.unterAssambly.BoxB.Face2`) liefert die Funktion jetzt
   korrekt `MinimalReproSub#BoxB` (das ECHTE Objekt) statt nullptr/falscher lokaler Kopie.

2. **`isPartConnected()`** hatte dieselbe Identitaetsraum-Luecke wie `removeUnconnectedJoints()`
   (gestern gefixt, aber diese Schwesterfunktion dabei uebersehen) - `groundedObjs` blieb
   unkanonisiert. Fix: `canonicalizeForMbD()` auf `obj` UND auf jedes `groundedObj` angewendet,
   bevor verglichen wird. **Verifiziert per Log:** `isPartConnected('MinimalReproSub#BoxB')`
   liefert jetzt `TRUE` (vorher waere es lautlos `FALSE` gewesen, wodurch `preDrag()` das Teil
   komplett unbeschraenkt liesse).

**Aber: der eigentliche Live-Symptom (freies Ziehen) bleibt bestehen** - dritter, tieferer und
groesserer Befund gefunden, NICHT behoben:

`AssemblyLink::synchronizeComponents()` (Zeile ~508-513):
```cpp
// If the assemblyLink is rigid, then we keep all placements synchronized.
if (isRigid()) {
    for (const auto& [sourceObj, linkObj] : objLinkMap) {
        syncPlacements(sourceObj, linkObj);
    }
}
```
Das lokale Spiegel-Objekt (`App::Link`-Element, das TATSAECHLICH in der 3D-Ansicht gerendert und
mit der Maus gezogen wird) bekommt seine Placement **nur bei `Rigid=true`** laufend vom echten
Quellobjekt zurueckgespiegelt. Bei einer FLEXIBLEN Unterbaugruppe (unser Fall) passiert das nie.

**Erklaert den scheinbaren Widerspruch** zwischen den gestrigen (skript-basierten,
direkt-auf-dem-echten-Objekt operierenden) Tests, die korrektes Achsen-Verhalten zeigten, und dem
heutigen Maus-Test: das Skript testete nur die SOLVER-Schicht (jetzt nachweislich korrekt - Fig. 1
bis 4 des Ablaufdiagramms). Der Maus-Drag geht ueber eine KOMPLETT ANDERE Schicht
(GUI/Rendering/Coin3D) - `getMovingPartFromSel()` liefert zwar jetzt korrekt das echte Objekt fuer
die SOLVER-SEITE (`docsToMove`/`preDrag()`/`doDragStep()`), aber was der Nutzer sieht und
tatsaechlich per Maus zieht, ist die lokale Spiegel-Kopie - und die bekommt das korrekt berechnete
Solver-Ergebnis nie mitgeteilt, weil der Sync-Mechanismus fuer flexible Faelle schlicht fehlt.

**Status:** NICHT behoben, groesserer Eingriff als die bisherigen Fixes, dieselbe Codezone wie der
bereits einmal abgestuerzte `synchronizeGroundedAndRigidJoints()`-Versuch (siehe
[[project_fcproject_grounded_joint_nested_flex_assembly_bug]]). Nutzerentscheidung noetig, wie
weiter vorgegangen wird, bevor hier weitergebaut wird.

**Nebenbefund (Nutzeranfrage, zurueckgestellt):** derselbe InList-Aufstieg-Bug wie in
`canonicalizeForMbD()`s urspruenglichem, fehlerhaftem Versuch (siehe Abschnitt "2026-09-02 (spaeter
am selben Tag)") steckt auch in `getContext()` (JointObject.py, Tooltip) und
`getJointContextName()` (AssemblyObject.cpp, Konsolen-Meldungen) - beide laufen bei mehreren
gleichzeitig geladenen, verschachtelten Dokumenten faelschlich ueber ein FREMDES `AssemblyLink`-
Objekt aus einem anderen Dokument (per `App::PropertyXLink` im InList sichtbar), statt beim echten
lokalen Elternobjekt zu bleiben - erzeugt irrefuehrende, doppelte Pfade wie
"Assembly.Assembly001.Assembly.unterAssambly...". **Nutzerwunsch: spaeter beheben** (gleiches
robustes Top-Down-Gruppensuche-Muster wie in `canonicalizeForMbD()`/`findLocalGroupPath()`
anwendbar), nicht in dieser Sitzung - explizit nicht vergessen.

Diagnose-Logging (FCPROJECT-DEBUG in `getMovingPartFromSel()`/`isPartConnected()`) ist noch im
Baum, muss vor einem Commit/Deploy wieder entfernt werden.

## 2026-09-03 (Fortsetzung 3): Live-Drag-Diagnose - drei weitere reale Bugs gefunden+gefixt, vierter Bug gefunden+gefixt aber noch NICHT live verifizierbar

Nutzer testete nach den ersten beiden Fixes (getMovingPartFromSel/isPartConnected) weiterhin per
Maus in `MinimalReproGrandTop.FCStd` und `MinimalReproTop.FCStd`. Weitere drei Bugs gefunden und
behoben, alle live per Diagnose-Logging bestaetigt oder durch Datei-Inspektion hergeleitet:

**3. `ViewProviderAssembly::canDragObjectIn3d()`** nutzte `assemblyPart->hasObject(obj, true)` -
eine rein lokale, pointer-basierte Suche (`GroundExtension::hasObject()`, Code gelesen und
bestaetigt) - fuer ein ECHTES, ueber eine verschachtelte flexible AssemblyLink erreichtes Objekt
(seit Fix 1 das, was `getMovingPartFromSel()` tatsaechlich zurueckgibt) IMMER `false`. Der Drag
wurde dadurch komplett abgelehnt, BEVOR `preDrag()`/`isPartConnected()` je aufgerufen wurden -
live per Log bestaetigt (bei GrandTop, 2 Ebenen, KEIN einziger `isPartConnected()`-Aufruf im Log
nach den `getMovingPartFromSel()`-Aufrufen, waehrend bei Top (1 Ebene) durchaus welche auftraten).
Fix: neue Methode `AssemblyObject::hasRealObject()` (App.h/.cpp) - lokaler Fast-Path plus
Kandidatensuche via `canonicalizeForMbD()`, gleiches Muster wie `syncLocalMirrorPlacement()`.
`canDragObjectIn3d()` darauf umgestellt (AssemblyGui.so betroffen, zusammen mit AssemblyApp.so
deployt).

**4. `AssemblyLink::synchronizeComponents()`-Luecke - lokales Spiegel-Objekt bekam Solver-
Ergebnis nie mitgeteilt** (die im vorigen Abschnitt bereits beschriebene Luecke): `if
(isRigid()) { syncPlacements(...); }` - bei flexiblen Baugruppen fehlt der Ruecksync komplett.
Ein direkter Fix in AssemblyLink.cpp scheitert an der Ausfuehrungsreihenfolge (dessen execute()
laeuft VOR dem solve() der Baugruppe, per Debug-Logging bestaetigt). Stattdessen neue Methode
`AssemblyObject::syncLocalMirrorPlacement()`, aufgerufen direkt im Anschluss an
`setNewPlacements()`s Schreiben des kanonischen Werts - synchron, ohne Reihenfolge-Abhaengigkeit.
Sucht per `collectLocalMirrorCandidates()` (neue anonyme Namespace-Funktion, gleiches
Abstiegsmuster wie `collectComponentsRecursively()`) alle lokalen Kandidaten und prueft per
`canonicalizeForMbD()`, welcher davon dem echten Objekt entspricht. **Verifiziert per Skript**
(`test_mirror_sync.py`): echte und lokale BoxB-Placement stimmen nach einer absichtlichen
Off-Axis-Verschiebung des echten Objekts + Recompute exakt ueberein.

**5. `addConnectedFixedParts()` (in `getMbDData()`s `bundleFixed`-Block) schrieb DIREKT, ohne
Kanonisierung, in `objectPartMap`** - umgeht damit den in `getMbDData()`s Kopf eingehaengten
`canonicalizeForMbD()`-Aufruf komplett, weil dieser Block den Eintrag manuell setzt statt
rekursiv `getMbDData()` aufzurufen. Gefunden beim Nachvollziehen von Testfall `MinimalReproTop`
(1 Ebene, KEIN Nesting am Joint selbst noetig!): die Datei hat NUR EIN "BoxA"/"BoxB"-Objektpaar
im gesamten Top-Dokument (`App::Link`, lokale Spiegel-Kopie INNERHALB von `unterAssambly`s
eigener Group) - `Joint002` (Starrer Verbund) im TOP-LEVEL JointGroup referenziert dieses
`App::Link "BoxA"` DIREKT (kein Nesting am Joint noetig, da der Joint selbst nicht verschachtelt
ist, nur sein ZIEL zufaellig innerhalb einer verschachtelten Baugruppe liegt). Ohne Kanonisierung
bekam dieses `App::Link "BoxA"` beim `bundleFixed`-Pfad einen SEPARATEN MbD-Koerper (gebunden an
BoxDs Koerper, ROHER Pointer als Schluessel) - waehrend Subs ECHTER Slider-Joint (BoxA<->BoxB)
ueber die adressierungsbewusste Rekursion beim ECHTEN `Sub#BoxA` landet - ZWEI GETRENNTE
MbD-Koerper fuer dasselbe reale Teil, exakt dasselbe Muster wie der urspruengliche
`canonicalizeForMbD()`-Fix es fuer Grounding/Joint-Referenzen behoben hat, nur an einer
uebersehenen dritten Stelle. Fix: `part1`/`part2` in `addConnectedFixedParts()` durch
`canonicalizeForMbD()` geschickt, bevor sie als `objectPartMap`-Schluessel verwendet werden.
Gleichzeitig `getJointsOfPart()` ebenso kanonisiert (wird von `addConnectedFixedParts()` UND von
mehreren anderen Stellen genutzt, gleiches Identitaetsraum-Problem).

**Wichtige Erkenntnis beim Testen von Fix 5:** `bundleFixed` wird NUR waehrend `preDrag()` auf
`true` gesetzt - ein normaler `recompute()`/Solve()-Aufruf (wie ihn jedes Skript-basierte Testen
bisher nutzte) durchlaeuft diesen Codepfad NIE. Das erklaert praezise das vom Nutzer beobachtete
"D laesst sich bewegen, aber erst nach dem Berechnen richtig" - WAEHREND des Ziehens (bundleFixed,
bis heute fehlerhaft) driftete die Kette auseinander, NACH einem vollstaendigen Recompute
(bundleFixed=false, nutzt den bereits gestern korrekt kanonisierten regulaeren Joint-Pfad) schnappt
alles wieder korrekt zusammen. **Fix 5 ist NICHT per Skript verifizierbar** (kein Python-Zugriff
auf `preDrag()`/`doDragStep()`) - Verifikation erfordert einen echten Maus-Drag, steht noch aus.

**Status Ende Sitzung (Nutzer kurzzeitig abwesend):** Fixes 1-5 alle im Baum, kompilieren sauber,
1+2+3+4 live/pekriptet verifiziert, Fix 5 nur durch Code-Analyse begruendet + strukturell analog
zu den bereits verifizierten Fixes, ausstehende Live-Maus-Verifikation. Diagnose-Logging (jetzt
durchgehend `Base::Console().log()`, keine Popup-Stoerung mehr) bleibt vorerst im Baum. Bekannter,
NICHT in dieser Sitzung angegangener sechster Befund: Container-Frame-Vererbung fehlt komplett
(`unterAssambly` als Fixed-Ziel bewegt sein eigenes Placement, aber NICHT die absolute Position
seiner eigenen Inhalte BoxA/BoxB - zwei voneinander unabhaengige Solver-Teilsysteme ohne
Kopplung) - separate, groessere Baustelle, noch nicht begonnen.

## 2026-09-03 (Fortsetzung 4): sechster Bug gefunden+behoben - resolvePartForMbD() kanonisierte nicht konsequent, Reachability-Kette komplett wiederhergestellt

Per Xvfb-Skript (Nutzer explizit: "das kannst du auch in xvfb machen") systematisch
weiterverfolgt, warum BoxA in `MinimalReproTop` nicht mit BoxD mitwandert.

**Wichtiger Nebenfund per XML-Inspektion der Testdatei:** `MinimalReproSub`s eigener
`GroundedJoint` erdet `BoxA` (`ObjectToGround` -> "BoxA"), UNABHAENGIG davon, dass `Joint002` in
Top `BoxA` zusaetzlich per Starrem Verbund an `BoxD` koppelt. Das ist ein ECHTER Modell-
Widerspruch (BoxA soll gleichzeitig "unbeweglich geerdet" UND "starr an BoxD gekoppelt" sein) -
erklaert die vom Solver gemeldeten "2 redundant joint(s): Joint, Joint002". Verifiziert: nach
Entfernen von Subs `GroundedJoint` (Skript, `test_grounding_conflict.py`) meldet der Solver
"finished successfully" ohne Redundanz-Warnung.

**Sechster echter Code-Bug gefunden:** `resolvePartForMbD()` kanonisierte sein Ergebnis NICHT,
bevor es zurueckgegeben wird - nur `getMbDData()` (der JOINT-VERARBEITUNGS-Pfad) canonicalisiert
intern. `removeUnconnectedJoints()`/`getConnectedParts()` (der REACHABILITY-Pfad) nutzen
`resolvePartForMbD()` dagegen DIREKT, ohne Umweg ueber `getMbDData()`. Fuer einen NICHT
verschachtelten Joint (leeres `nestingPrefix`, z.B. `Joint002` im Top-Level-JointGroup), dessen
Referenz zufaellig auf ein lokales Spiegel-Objekt INNERHALB einer verschachtelten flexiblen
AssemblyLink zeigt (`App::Link "BoxA"` in `unterAssambly`), liefert `resolveJointReference()`
nur den ROHEN lokalen Pointer - ohne Prefix hat sie keinen Grund, weiter durch
`getLinkedAssembly()` aufzuloesen. Subs EIGENER Slider-Joint (ueber subJoints-Rekursion,
Prefix="unterAssambly.") landet dagegen beim ECHTEN `Sub#BoxA`. Zwei UNTERSCHIEDLICHE Pointer
fuer dasselbe reale Teil - `removeUnconnectedJoints()` sah dadurch nie eine Verbindung zwischen
Joint002s Seite und Subs Slider-Joint, verwarf Letzteren live bestaetigt als "nicht erreichbar"
(sobald die oben beschriebene Redundanz entfernt wurde und BoxA nicht mehr trivial als
"geerdet" - und damit als Traversal-Startpunkt - selbst schon im Erreichbarkeits-Graphen war).

Fix: `resolvePartForMbD()` kanonisiert jetzt BEIDE Rueckgabepfade (den `resolveJointReference()`-
Erfolgsfall UND den `getMovingPartFromRef()`-Ruecksfall) per `canonicalizeForMbD()`, bevor sie
zurueckgegeben werden - zentraler Choke-Point, alle Aufrufer (`removeUnconnectedJoints()`,
`getConnectedParts()`, `isMbDJointValid()`, `handleOneSideOfJoint()` via `getMbDData()`,
`getRackPinionMarkers()`) profitieren einheitlich, keine Aenderung an den Aufrufern selbst
noetig. Idempotent fuer bereits-kanonische (echte, tief verschachtelte) Objekte - kein Regress
fuer bereits funktionierende Faelle.

**Verifiziert** (Skript, Grounding-Konflikt entfernt): `'MinimalReproTop#Assembly' computed
(3 joint(s), 3 grounded part(s))`, "finished successfully", KEINE "entfernt"-Meldung mehr -
alle drei Joints (BoxC-BoxD Slider, BoxA-BoxD Fixed, Subs BoxA-BoxB Slider) laufen jetzt als EIN
konsistentes System. BoxB reagiert jetzt sichtbar auf BoxDs Bewegung (vorher komplett
eingefroren) - Betrag/Richtung nicht exakt pruefbar (Rohtest per direkter Placement-Injektion +
`recompute()` ist keine perfekte Drag-Simulation fuer eine 3-Joint-Kette, siehe Fix-5-Abschnitt),
aber die QUALITATIVE Kopplung ist eindeutig wiederhergestellt.

**Mit der UNVERAENDERTEN Originaldatei** (Subs Erdung weiterhin aktiv, wie beim Nutzer): BoxD
bleibt beim selben Rohtest (direkte Placement-Injektion) an seiner Ausgangsposition - die
Redundanz laesst den Solver offenbar BoxAs eigene Erdung als massgeblich behandeln. Das ist
vermutlich (noch nicht mit echtem Maus-Drag verifiziert) der GENAUE Ursprung des vom Nutzer
beobachteten "BoxA folgt BoxD nicht" - ein Modellproblem der TESTDATEI selbst (BoxA war
urspruenglich fuer den eigenstaendigen Sub-Test geerdet, Joint002 kam spaeter fuer den
Top-Level-Test dazu, nie bereinigt), NICHT (mehr) ein Code-Bug.

**Status Ende Sitzung:** sechs reale Bugs (1-6) gefunden und behoben, 1/2/4/6 per Skript
verifiziert, 3 (canDragObjectIn3d) strukturell/durch Log bestaetigt, 5
(addConnectedFixedParts/bundleFixed) nur durch Code-Analyse begruendet, Live-Maus-Verifikation
fuer 3/5 steht weiterhin aus. NICHT committed, NICHT auf die echte Installation deployt.
Empfehlung fuer den naechsten Live-Test: entweder das ECHTE `MinimalReproTop.FCStd` mit dem
Wissen testen, dass BoxA/BoxD wegen der Datei-eigenen Redundanz vermutlich nicht sauber
zusammenspielen werden (Testdatei muesste dafuer bereinigt werden - Subs GroundedJoint entfernen
oder Joint002 entfernen), ODER eine BEREINIGTE Kopie testen (wie
`test_grounding_conflict.py` es tut) fuer eine aussagekraeftige Pruefung der eigentlichen
Code-Fixes.

## 2026-09-04: achter Bug - syncLocalMirrorPlacement() lief nicht, wenn das kanonische Objekt selbst unveraendert blieb

Nutzerauftrag: "warum boxen nach dem laden haben eine position, wenn taste z wird gedrück die
werden nicht linear platziert" - selbststaendig bearbeiten, bezog sich auf
`MinimalReproTop.FCStd`.

**Root Cause:** `setNewPlacements()` rief `syncLocalMirrorPlacement()` (Fix 4, 2026-09-03) nur
INNERHALB des `isSame()`-Checks auf - lief also nur, wenn sich der kanonische (echte) Placement-
Wert selbst aenderte. Fuer ein bereits korrekt geerdetes Teil (`Sub#BoxA`, immer (0,0,0)) trifft
das NIE zu - die lokale Spiegel-Kopie (`Top#BoxA`, App::Link, direkt aus der Datei mit dem
Placement (46.22,30,0) geladen) wurde dadurch NIE korrigiert, obwohl sie logisch laengst falsch
stand. Sichtbar als: `BoxD` (per Fixed-Joint002 an diese veraltete lokale Kopie gekoppelt)
"springt" beim ersten Z auf eine augenscheinlich willkuerliche Position.

**Fix:** `syncLocalMirrorPlacement(obj, newPlacement)` jetzt UNBEDINGT aufgerufen, ausserhalb des
`isSame()`-Checks - die Funktion hat selbst bereits einen eigenen `isSame()`-Guard, unnoetige
Schreibvorgaenge auf bereits korrekte Kopien bleiben also weiterhin aus.

**Verifiziert per Skript** (`test_z_linearity.py`): `Top#BoxA` folgt jetzt korrekt `Sub#BoxA`
nach Taste Z (vorher haengengeblieben bei (46.22,30,0), jetzt (0,0,0)).

**Verbleibender Sprung von `BoxD` mit der UNVERAENDERTEN Testdatei bestaetigt Testdatei-
Redundanz, kein Code-Bug** (`test_z_no_redundancy.py`): mit entfernter Redundanz (Subs
`GroundedJoint` entfernt) laeuft die komplette Kette sauber auf einer Linie (Y=30 durchgehend
bei BoxC/BoxD/BoxA/BoxB), "finished successfully", keine Redundanz-Warnung.

**Nebenfund bei `GrandTop` (2 Ebenen), noch nicht bearbeitet:** die mittlere Ebene (`Top`)
synchronisiert ihre EIGENE lokale Spiegel-Kopie nicht mehr, wenn sie ihren Solve an `GrandTop`
abgibt (`isNestedUnderFlexibleParent()`) - `GrandTop`s `syncLocalMirrorPlacement()` erreicht nur
ihre eigenen lokalen Kopien (`Assembly001.BoxA` etc.), nicht Tops separates Dokument. Betrifft
nur mehrstufige Verschachtelung.

**Status:** Fix im Baum, Patch aktualisiert (`freecad-assembly-jointobject.patch`), per
Worktree-Test gegen frischen Checkout verifiziert. NICHT committed, NICHT deployt.

## 2026-09-04 (Fortsetzung): neunter Bug - getGroundedParts()-Rekursion war zu aggressiv, "Redundanz" war gar keine Testdatei-Eigenschaft

Wichtige Nutzerkorrektur zum vorigen Abschnitt: die vermeintliche "Testdatei-Redundanz" (BoxA
gleichzeitig in Sub geerdet und in Top per Joint002 an BoxD gekoppelt) ist KEIN
Modellierungsfehler der Testdatei. Nutzerzitat: "in aktuellem freecad hat es funktioniert. wenn
minimal sub wird in top eingeschlossen dann erdung wird auf box d übertragen." - im
unveraenderten FreeCAD (vor Teilschritt 2e) war `getGroundedParts()` NICHT rekursiv, Subs eigene
Erdung war fuer die aeussere Ebene schlicht unsichtbar - die Erdung "wanderte" dadurch effektiv
komplett zur aeusseren Kette (BoxC). Mein eigener Fix von vorgestern (rekursives Einsammeln
verschachtelter Erdungen, noetig um den urspruenglichen Reachability-Bug zu beheben) hat dabei
diese - eigentlich erwuenschte - Prioritaet zerstoert: er zieht eine verschachtelte Erdung jetzt
IMMER mit rein, auch wenn das betroffene Teil bereits ueber eine ganz normale Joint-Kette von der
AEUSSEREN Erdung dieser Ebene aus erreichbar ist.

**Fix:** vor dem Uebernehmen einer verschachtelten Erdung wird jetzt geprueft, ob das betroffene
Teil bereits ueber die (adressierungsbewusste) Joint-Kette von der LOKALEN, nicht-rekursiven
Erdung dieser Ebene aus erreichbar ist (per bereits existierender `traverseAndMarkConnectedParts()`/
`isObjInSetOfObjRefs()`-Maschinerie, wiederverwendet). Wenn ja: die aeussere Kette hat Vorrang,
die verschachtelte Erdung wird NICHT zusaetzlich uebernommen. Nur wenn ein verschachteltes Teil
auf KEINE andere Weise erreichbar waere (z.B. eine nicht an die aeussere Kette angebundene
Unterbaugruppe), zaehlt seine eigene Erdung weiterhin.

**Verifiziert per Skript+Screenshot, UNVERAENDERTE Originaldatei `MinimalReproTop.FCStd`:**
- Vorher: `computed (3 joint(s), 4 grounded part(s))`, "finished with 2 redundant joint(s)",
  Boxen chaotisch verteilt (Screenshot `compare_with_redundancy.png`).
- Nachher: `computed (3 joint(s), 3 grounded part(s))`, "finished successfully", alle Boxen
  sauber auf einer Linie (Screenshot `after_grounding_fix.png`, identisch zum vorher nur mit
  manuell entfernter Erdung erreichten Ergebnis - jetzt aber ohne die Datei anzufassen).
  Zahlenwerte: BoxC=(0,30,0), BoxD=Sub#BoxA=lokale BoxA-Kopie=(23.11,30,0),
  Sub#BoxB=lokale BoxB-Kopie=(51.34,30,0).

`Sub` allein und `Top` (1 Ebene) beide erneut regressionsfrei bestaetigt. `GrandTop` (2 Ebenen)
zeigt weiterhin die bereits bekannte, SEPARATE Luecke (mittlere Ebene synchronisiert ihre eigene
lokale Kopie nicht, wenn sie an GrandTop abgibt) - kein Rueckschritt durch diesen Fix, aber auch
noch nicht geloest.

**Status:** Fix im Baum, Patch aktualisiert, per Worktree-Test verifiziert. NICHT committed,
NICHT deployt.

## 2026-09-04 (Fortsetzung): zehnter Bug - canonicalizeForMbD() loeste nicht ueber
verschachtelte Ebenen hinweg auf, GrandTop verhielt sich anders als Top

**Nutzerauftrag (wörtlich):** "ja, bitte comitte und grandtop dsoll sich auch richtig
(entsprechend modell - schubgelenk) verhalten wie top. test: vergleich modell - grandtop"

Nach dem Commit von Fix 8+9 (Zwischenstand) direkt weiter am `GrandTop`-Verhalten (2
Verschachtelungsebenen) gearbeitet, mit dem Ziel, den bereits bekannten "GrandTop
synchronisiert die mittlere Ebene nicht" - Befund tatsaechlich zu beheben statt nur zu
dokumentieren.

**Testinfrastruktur-Umweg (lehrreich, aber zeitaufwaendig):** ein automatisiertes
GrandTop-Skript in Xvfb hing wiederholt scheinbar in einer Endlosschleife - "Ungespeichertes
Dokument"-Dialog blockierte, kein `FCPROJECT-TEST: DONE`-Marker im Bash-Redirect-Log sichtbar,
selbst nach `setsid`-Entkopplung und komplett frischem Xvfb-Display. Erst ein C++-Backtrace
(`backtrace()`/`backtrace_symbols()`, temporaer in `MainWindow::closeEvent()` eingebaut) loeste
das Raetsel: der Aufruf-Stack fuehrte zurueck zu `QTimer::timeout` -> genau dem eigenen
`QtCore.QTimer.singleShot(300, Gui.getMainWindow().close)` aus dem Testskript selbst! Das Skript
war die GANZE ZEIT erfolgreich fertig gelaufen (inkl. Screenshot) - nur die eigene
Bash-Redirect-Log-Datei blieb an einer Pufferungsgrenze mitten im Wort haengen, WAEHREND
FreeCADs eigenes `--log-file` (per Skript-Aufrufzeile immer mitgeloggt) den kompletten,
erfolgreichen Ablauf longstamm zuverlaessig enthielt. Lektion fuer kuenftige Xvfb-Tests: bei
Verdacht auf einen haengenden Prozess IMMER zuerst das echte `--log-file` unter
`~/freecad/logs/` pruefen, nicht nur die eigene Bash-Redirect-Datei (siehe auch
[[project_fcproject_live_log_file_access]]) - und der abschliessende "Speichern?"-Dialog beim
Schliessen ist normales, erwuenschtes Verhalten (das Skript schliesst am Ende bewusst das
Hauptfenster), kein Bug.

**Eigentlicher Befund (per Skript+Screenshot am unveraenderten `MinimalReproGrandTop.FCStd`
bestaetigt):** nach `GrandTop.recompute(True)` blieb das ECHTE `Sub#BoxA` dauerhaft am Ursprung
stehen, waehrend `Top#BoxA` (Tops EIGENE lokale Spiegel-Kopie) korrekt auf die
Fixed-Joint-Zielposition bewegt wurde - UND `GrandTop`s EIGENE tief verschachtelte lokale Kopie
ebenfalls unbewegt blieb. Im 3D-View von `GrandTop` ueberlappten sich dadurch mehrere Boxen an
der Ursprungsposition (Screenshot `grandtop_after_z.png`, Stand vor dem Fix) statt sauber entlang
der Schubgelenk-Achse zu liegen wie im laengst verifizierten `Top`-Alleinstand-Fall.

**Root Cause:** `canonicalizeForMbD()` sucht das uebergebene Objekt per `findLocalGroupPath()`
zunaechst im EIGENEN lokalen `Group`-Baum von `this`. Schlaegt das fehl, gab die Funktion das
Objekt bisher UNVERAENDERT als "bereits kanonisch" zurueck. Das ist korrekt, wenn `obj` ein
bereits echtes, fremddokument-natives Objekt ist (z.B. von `resolveJointReference()` geliefert) -
aber FALSCH, wenn `obj` die lokale Spiegel-Kopie einer VERSCHACHTELTEN Unterbaugruppe ist:
`Top#BoxA` liegt im Dokument von `Top`, nicht im `Group` von `GrandTop` - `GrandTop`s eigene
Suche findet dort nur den `AssemblyLink`-Container (`Assembly001`), niemals `Top#BoxA` selbst.
Bei `Joint002` ("StarrerVerbund", NICHT verschachtelt, leerer `nestingPrefix`), dessen
Referenz2 zufaellig auf `unterAssambly`s lokale `BoxA`-Kopie zeigt, blieb `resolvePartForMbD()`
dadurch bei `Top#BoxA` als vermeintlich kanonischem Ziel stehen, statt bis zum tatsaechlich
tiefsten echten Objekt `Sub#BoxA` durchzuloesen - `setNewPlacements()` schrieb den geloesten Wert
folglich auf `Top#BoxA` STATT auf `Sub#BoxA`. Da `syncLocalMirrorPlacement()`s eigener
Kandidatenabgleich ebenfalls gegen dieses falsche `realObj` (`Top#BoxA` statt `Sub#BoxA`)
verglich, matchte nicht einmal `GrandTop`s EIGENE, korrekt bis `Sub#BoxA` durchaufloesende lokale
Kopie - sie blieb dadurch ebenfalls unsynchronisiert.

**Fix:** wenn `findLocalGroupPath()` fehlschlaegt, wird jetzt an JEDE eigene (nicht rigide)
Unterbaugruppe delegiert, deren EIGENES Dokument zum Zielobjekt passt:
```cpp
for (auto* asmLink : getSubAssemblies()) {
    if (!asmLink || asmLink->isRigid()) continue;
    AssemblyObject* nested = asmLink->getLinkedAssembly();
    if (!nested || nested == this || nested->getDocument() != obj->getDocument()) continue;
    return nested->canonicalizeForMbD(obj);
}
return obj;
```
`canonicalizeForMbD()` ist dadurch rekursiv - loest automatisch beliebig viele
Verschachtelungsebenen bis zum tatsaechlich tiefsten echten Objekt auf, ganz ohne eine zweite,
parallele Namenspfad-Logik zu pflegen (derselbe Grundsatz wie beim Rest der
"Adressieren statt Kopieren"-Arbeit).

**Verifiziert per Skript+Screenshot** auf der UNVERAENDERTEN Originaldatei
`MinimalReproGrandTop.FCStd`:
- Vorher: `Sub#BoxA=(0,0,0)` (unveraendert), `GrandTop#BoxA=(0,0,0)` (unveraendert),
  `Top#BoxA=(46.22,30,0)` (einzig bewegtes Duplikat) - Boxen im Screenshot ueberlappend am
  Ursprung gestapelt.
- Nachher: `Sub#BoxA=GrandTop#BoxA=Top#BoxD=(23.11,30,0)`, `Sub#BoxB=GrandTop#BoxB=(51.34,30,0)`,
  `BoxC=(0,30,0)` - vier sauber getrennte, entlang der Achse aufgereihte Boxen im Screenshot,
  ZAHLENWERTE IDENTISCH zum laengst verifizierten `Top`-Alleinstand-Ergebnis (siehe Fix 9 oben:
  "BoxC=(0,30,0), BoxD=Sub#BoxA=lokale BoxA-Kopie=(23.11,30,0), Sub#BoxB=(51.34,30,0)") - `GrandTop`
  verhaelt sich jetzt nachweislich exakt wie `Top`.

**Nebenbefund, bestaetigt kein Rueckschritt:** `syncGroundedJoints()`s Neuanlage-Zweig fuer
`GroundedJoint`-Marker (Verdacht auf die laenger bekannte "GroundedJoint-Vervielfachung",
[[project_fcproject_groundedjoint_proliferation_bug]]) nutzte fuer die Objektsuche ebenfalls
IMMER `getDocument()` (das Dokument DIESER Assembly) statt `part->getDocument()` (das Dokument
des zu erdenden Teils) - bei einem cross-document 'part' waere das potenziell derselbe
Identitaetsraum-Fehler. Vorsorglich mitgefixt (siehe Code), im GrandTop-Testfall aber ZERO
`GroundedJoint`-Neuanlagen beobachtet (0 Treffer fuer den entsprechenden Debug-Log), also nicht
live als eigenstaendiger Bug bestaetigt - reine Haertung.

**Weiterhin offen, jetzt bestaetigt NICHT sichtbarkeitsrelevant fuer GrandTop selbst:** Tops
EIGENE, separate lokale Spiegel-Kopie (`Top#BoxA`, in Tops eigenem Dokument) bleibt nach einem
GrandTop-only-Solve weiterhin unsynchronisiert (derselbe, bereits dokumentierte
Zwischenebenen-Gap) - betrifft nur den Fall, dass `Top` UNABHAENGIG von `GrandTop` separat
geoeffnet/angezeigt wird. Fuer `GrandTop`s eigene 3D-Ansicht irrelevant. Nicht weiter verfolgt.

**Status:** Fix im Baum, Patch (v8) aktualisiert und per Worktree-Test verifiziert, committed.
