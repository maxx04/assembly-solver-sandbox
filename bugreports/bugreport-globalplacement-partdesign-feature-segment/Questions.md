# Root-Cause-Befund: `getPlacementOf()` rekursiert fälschlich in PartDesign-Feature-Segmente

**Datum:** 2026-09-01
**Betroffene Version:** FreeCAD 26.3.0 (Git, selbst kompiliert), **Core-App-Modul**
(`src/App/DocumentObject.cpp`, `src/App/Link.cpp`) - NICHT Assembly-spezifisch, betrifft jede
Funktion, die `DocumentObject::getPlacementOf()` für einen mehrstufigen Sub-Pfad durch ein
PartDesign-Feature aufruft.
**Repro-Datei:** [repro.zip](./repro.zip) - `CNC3018_023_A_Halterbaugruppe.FCStd` +
verlinkte Teile (Halter, Halter_V1, Nutenstein), alle im selben Ordner entpacken.

## Zusammenhang mit früheren Befunden

Direkte Fortsetzung von `patches/bugreport-fixed-joint-no-coincidence/` (Update 1: eigener
`_rewire_joint()`-Bug in FCProject, gefixt) und
`patches/bugreport-groundedjoint-deletion-race/` (Update: `syncGroundedJoints()`-Race-Condition,
gefixt). Nach BEIDEN Fixes blieb ein sauberer, konstanter Translations-Versatz von 117,7mm
zwischen den beiden Fixed-Joint-Referenzflächen bestehen, den weder `matchJCS()` noch ein
vollständiger Solve ("Z") beheben konnten - das führte hierher, zur dritten, tiefsten Ursache
in dieser Kette.

## Beobachtung

`AssemblyObject::getGlobalPlacement(ref)` (`UtilsAssembly.py`) ruft am Ende
`rootObj.getPlacementOf(subName, targetObj)` auf - eine Core-`App::DocumentObject`-Methode
(`src/App/DocumentObject.cpp`, mit `App::Link`-Override in `src/App/Link.cpp`), die einen
mehrstufigen Sub-Pfad wie `"Halter_V1_Link.Pocket.Face10"` rekursiv auflöst, indem sie bei
jedem Punkt-getrennten Segment prüft, ob ein Dokumentobjekt mit diesem Namen existiert, und
falls ja, in dessen `getPlacementOf()` weiter rekursiert (mit dessen eigener
`Placement`-Property als Faktor).

**Für einstufige Pfade** (z.B. `"Halter.Face14"`, keine PartDesign-Feature-Qualifikation
nötig) funktioniert das korrekt: `"Face14"` ist kein echter Objektname, die Objektsuche
schlägt fehl, die Funktion fällt sauber auf die `Placement` des Links selbst zurück - das
IST die semantisch richtige globale Placement.

**Für mehrstufige Pfade durch ein PartDesign-Feature** (z.B. `"Halter_V1_Link.Pocket.Face10"`,
nötig wenn die referenzierte Fläche nicht zum aktuellen Tip-Feature gehört, siehe
`UtilsAssembly.addTipNameToSub()`) geht es schief: **"Pocket" IST ein echtes Dokumentobjekt**
(das PartDesign-Feature selbst) - die Funktion findet es und rekursiert fälschlich hinein,
multipliziert also mit `Pocket`s eigener, für die 3D-Position bedeutungsloser
`Placement`-Property, statt bei der `Placement` des umgebenden Links zu bleiben.

## Live-Beweis

Direkter Vergleich am Repro (nach Fix der beiden vorherigen Bugs, sodass `Reference1`
korrekt `['Pocket.Face10', 'Pocket.Face10']` trägt):

```python
>>> halter.Placement
Placement [Pos=(-261.079,26.3122,87.1094), ...]

>>> UtilsAssembly.getGlobalPlacement(joint.Reference1)   # SOLLTE identisch sein
Placement [Pos=(-162.556,5.81083,63.1545), ...]           # ist es NICHT
```

Für die Nutenstein-Seite (`Reference2`, einstufiger Pfad `"Face5"`) stimmt
`getGlobalPlacement()` dagegen exakt mit `nutenstein.Placement` überein - bestätigt, dass der
Bug spezifisch am mehrstufigen Pfad hängt, nicht an `getGlobalPlacement()`/`getJcsGlobalPlc()`
im Allgemeinen.

**Auswirkung auf den Assembly-Solver:** `getJcsGlobalPlc()` (Placement1/Reference1 kombiniert
mit der - falschen - `getGlobalPlacement()`) liefert dadurch eine falsche globale
JCS-Position für die Halter-Seite. Rotation stimmt (die steckt vollständig in `Placement1`,
nicht in `getGlobalPlacement()`), aber die Translation ist um genau die Differenz zwischen
der echten Link-Placement und der fälschlich durchs Feature "durchgerechneten" Placement
versetzt - am Repro exakt 117,733mm. Weder `matchJCS()` noch ein vollständiger Solve können
das beheben, weil BEIDE dieselbe fehlerhafte `getGlobalPlacement()`-Berechnung verwenden.

## Fix

`patches/freecad-app-getplacementof-partdesign-feature.patch` - in
`DocumentObject::getPlacementOf()` und (beide Zweige) `Link::getPlacementOf()`: bevor in ein
gefundenes Sub-Objekt rekursiert wird, prüfen ob es ein `PartDesign::Feature` ist (per
`Base::Type::fromName()`, keine harte Modul-Abhängigkeit von PartDesign nötig - App darf
architekturell nicht von Mod/PartDesign abhängen). Falls ja: NICHT rekursieren, stattdessen
sofort die bisher akkumulierte Placement zurückgeben - ein PartDesign-Feature im Sub-Pfad ist
reine Historien-/TNP-Adressierung innerhalb EINES Bodies, keine eigenständig platzierte
Entität.

**Live verifiziert (Xvfb-Sandbox):** nach dem Fix stimmen `getGlobalPlacement(Reference1)` und
`halter.Placement` exakt überein, Face10- und Face5-CenterOfGravity landen exakt am selben
Punkt, gemessener Flächenabstand **0,000mm** (vorher 117,733mm) - stabil über Neuladen,
`matchJCS()` und vollständigen Solve hinweg. Kein Crash, keine "Cannot create object"-Fehler.

## Die eigentliche Frage (an Assembly-/Core-Kenner)

1. Ist der Fix-Ansatz (PartDesign::Feature-Segmente von der Rekursion ausschließen) korrekt,
   oder gibt es legitime Fälle, in denen ein PartDesign::Feature-Objekt IM Sub-Pfad tatsächlich
   eine eigene, für die globale Placement relevante Transformation beitragen sollte (z.B. bei
   `PartDesign::Body`-internen Multi-Body-Konstruktionen)?
2. Sollte die Prüfung generischer sein - z.B. auf `Part::Feature` innerhalb desselben
   `PartDesign::Body` statt spezifisch auf `PartDesign::Feature`, um auch andere,
   ähnlich gelagerte Fälle abzudecken (reine PartDesign-Historie vs. z.B. `Sketcher::Sketch`,
   das laut `UtilsAssembly.isBodySubObject()`-Python-Pendant bewusst ALS eigenständig
   adressierbar behandelt wird)?
3. Warum tritt dieser Bug offenbar nicht schon länger/häufiger in freier Wildbahn auf, obwohl
   `addTipNameToSub()` mehrstufige Pfade wie `"Body.Feature.Face1"` als STANDARD-Adressierung
   für nicht-Tip-Flächen einführt? Ist das ein Nischenfall (nur bei Cross-Document-Links auf
   PartDesign-Bodies über eine bestimmte Body-Historie-Konstellation), oder trifft es öfter zu,
   nur bisher nicht mit der Assembly-Solver-Symptomkette in Verbindung gebracht?
