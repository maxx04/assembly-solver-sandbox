# Root-Cause-Befund: `syncGroundedJoints()` löscht `GroundedJoint` bei einer Race Condition beim Laden

**Datum:** 2026-09-01
**Betroffene Version:** FreeCAD 26.3.0 (Git, selbst kompiliert), Assembly-Workbench
**Repro-Datei:** [groundedjoint-race-repro.zip](./groundedjoint-race-repro.zip) - enthält
`CNC3018_023_A_Halterbaugruppe.FCStd` + die zwei verlinkten Teile
(`CNC3018_006_B_Halter.FCStd`, `CNC3018_018_B_M5-Nutenstein.FCStd`), alle im selben Ordner
entpacken, dann die Hauptdatei öffnen. Enthält ein `GroundedJoint` (auf `CNC3018_006_B_Halter`)
und ein `Fixed`-Joint (`Halter.Face14` ↔ `Nutenstein.Face5`).

## Zusammenhang mit früheren Befunden

Dies ist die tatsächliche Root Cause hinter mehreren zuvor unabhängig beobachteten Symptomen:

- `patches/bugreport-fixed-joint-no-coincidence/` (2026-08-30/31) - der dort beschriebene
  "Fixed-Joint deckt die Flächen nicht" hatte zunächst einen ECHTEN, aber eigenständigen
  Ursachen-Anteil in FCProjects eigenem `PartExchangeWindow.py` (siehe dortiges "Update 1"),
  aber selbst nach dessen Fix blieb ein Bauteil komplett unbeweglich - das führt hierher.
- Bereits am 2026-08-30 live beobachtet (siehe Projekt-Notizen): "wenn ich mache neue gleiche
  starre Verbindung dann tut das geerdete Teil rumspringen ... neu berechnen hilft nicht" -
  passt zum hier beschriebenen Mechanismus (kein kanonisches Zurückkehren, sobald das
  GroundedJoint-Objekt einmal weg ist).

## Beobachtung

`AssemblyObject::getGroundedParts()` erkennt ein geerdetes Teil **nicht** über das
Vorhandensein eines `GroundedJoint`-Dokumentobjekts, sondern ausschließlich über das
`ReadOnly`-Statusflag der `Placement`-Property des Teils:

```cpp
// AssemblyObject::getGroundedParts()
for (auto part : allParts) {
    auto propPlc = part->getPlacementProperty();
    if (propPlc && propPlc->isReadOnly()) {
        groundedSet.insert(part);
    }
}
```

Dieses Flag wird nicht zuverlässig aus der Datei restauriert, sondern beim Laden per
Python-Proxy-Callback neu gesetzt:

```python
# JointObject.py, class GroundedJoint
def onDocumentRestored(self, joint):
    self.migrationScript(joint)
    self.setReadOnly(joint, True)   # setzt Placement/LinkPlacement auf ReadOnly
```

`AssemblyObject::syncGroundedJoints()` (aufgerufen am Anfang **jedes** `solve()`-Aufrufs)
gleicht GroundedJoint-Objekte mit diesem Flag ab und hat zwei Zweige:

```cpp
if (isReadOnly && !hasJoint) {
    // GroundedJoint fehlt, aber Flag ist gesetzt -> neu anlegen (Selbstheilung)
}
else if (!isReadOnly && hasJoint) {
    // Flag ist NICHT gesetzt, GroundedJoint existiert noch
    // -> Annahme: Nutzer hat die Sperre manuell aufgehoben, Joint ist verwaist
    getDocument()->removeObject(it->second->getNameInDocument());  // LÖSCHEN
}
```

**Das Problem:** `AssemblyObject::onChanged()` löst bereits während des Ladens (sobald Teile
in die `Group`-Property einsortiert werden, was Teil des normalen Restore-Ablaufs ist) selbst
ein `solve()` aus:

```cpp
void AssemblyObject::onChanged(const App::Property* prop) {
    if (prop == &Group) {
        ...
        updateSolveStatus();   // ruft ggf. solve() auf, falls mbdAssembly/mbdSystem fehlt
    }
    ...
}
```

Ob zu **diesem frühen Zeitpunkt** `GroundedJoint.onDocumentRestored()` (Python) das
`ReadOnly`-Flag bereits gesetzt hat, ist **nicht deterministisch** - abhängig von der
Reihenfolge, in der FreeCAD während des Dokument-Restores C++-Property-Änderungen und
Python-Proxy-Callbacks verarbeitet.

## Live-Beweis (C++-Instrumentierung, siehe `patches/assembly-architecture-overview.md` bzw.
## `project_fcproject_solver_data_fix_and_import_component`-Memory für die volle Herleitung)

Temporäres `Base::Console().warning()`-Logging in `getGroundedParts()`/`syncGroundedJoints()`:

- **Minimaler Testfall** (Datei öffnen, sofort prüfen, keine weiteren Aktionen): Flag korrekt
  gesetzt (`Placement`-Property-Status = `['ReadOnly', 'LockDynamic']`), `GroundedJoint`
  bleibt im Dokument erhalten.
- **Testfall mit mehreren `assembly.touch(); assembly.recompute(True)`-Aufrufen** (= exakt
  das, was die Taste "Z"/"Solve Assembly" laut `CommandSolveAssembly.py` macht) direkt nach
  dem Laden: beim ersten `getGroundedParts()`-Aufruf ist das Flag noch **nicht** gesetzt
  (`isReadOnly=0` für Halter UND Nutenstein) - `solve()` bricht mit "no grounded part found"
  ab. Am Ende dieses Testlaufs zeigt Halters `Placement`-Property-Status nur noch
  `['LockDynamic']` - das `ReadOnly`-Flag ist vollständig verschwunden (nicht nur verzögert
  gesetzt), und `doc.getObject("GroundedJoint")` liefert `None`.

## Wirkung

Sobald `syncGroundedJoints()`s Lösch-Zweig einmal in diesem Zeitfenster greift, wird das
`GroundedJoint`-Objekt **dauerhaft aus dem Dokument entfernt**. Danach gibt es kein Objekt
mehr, dessen `onDocumentRestored()` das `ReadOnly`-Flag je wieder setzen könnte - der
Selbstheilungs-Zweig (`isReadOnly && !hasJoint` → neu anlegen) greift nie, weil seine
Vorbedingung (`isReadOnly == true`) nie wieder eintritt. Ergebnis: **`getGroundedParts()`
liefert für dieses Teil dauerhaft leer, jeder weitere `solve()`-Versuch (auch über die Taste
"Z") bricht mit "no grounded part found" ab oder - falls noch ein anderes Teil geerdet ist -
lässt das eigentlich fixierte Bauteil komplett unbeweglich, egal welche Joint-Constraints
eigentlich gelten sollten.**

## Repro-Schritte

1. `groundedjoint-race-repro.zip` entpacken, `CNC3018_023_A_Halterbaugruppe.FCStd` öffnen.
2. Direkt nach dem Öffnen (per Python-Konsole oder Makro, **nicht** warten):
   ```python
   doc = App.ActiveDocument
   asm = [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"][0]
   for _ in range(4):
       asm.touch()
       doc.recompute(None, True, True)
   print(doc.getObject("GroundedJoint"))  # -> haeufig None statt <App::FeaturePython object>
   halter = doc.getObject("Joint").Reference1[0]
   print(halter.getPropertyStatus("Placement"))  # -> ['LockDynamic'] statt ['ReadOnly', 'LockDynamic']
   ```
3. Zum Vergleich: Datei neu öffnen, **sofort** (ohne Schritt 2) obige zwei Prints ausführen -
   liefert normalerweise `<App::FeaturePython object>` bzw. `['ReadOnly', 'LockDynamic']`.
4. Nichtdeterminismus beachten: das Zeitfenster ist eine echte Race Condition - je nach
   Maschinenlast/Event-Loop-Timing kann Schritt 2 auch beim ersten Versuch schon "sauber"
   bleiben; mehrfach wiederholen bzw. die Schleife in Schritt 2 vergrößern erhöht die
   Trefferquote.

## Die eigentliche Frage

1. Ist die Analyse oben korrekt - insbesondere: läuft `onChanged(&Group)` während des
   Dokument-Restores wirklich potenziell VOR den `onDocumentRestored()`-Callbacks der
   Kind-Objekte (hier: `GroundedJoint`), oder gibt es eine Reihenfolge-Garantie, die das
   ausschließen sollte (und die hier aus einem anderen Grund nicht greift)?
2. Ist `syncGroundedJoints()`s "lösche verwaistes GroundedJoint bei aufgehobener Sperre"-Logik
   grundsätzlich sinnvoll (für den interaktiven Fall: Nutzer hebt die Sperre manuell per
   Rechtsklick auf), aber fehlt ihr eine Sicherung gegen das Ausführen während/kurz nach dem
   Dokument-Restore (ähnlich der bereits vorhandenen `isRestoring()`-Prüfung am Anfang der
   Funktion, die aber nur die eigentliche Restore-Phase abdeckt, nicht die unmittelbar
   danach folgenden automatischen Solve-Versuche)?
3. Gibt es einen bevorzugten Fix-Ansatz - z.B. `GroundedJoint.onDocumentRestored()`
   zuverlässig vor dem ersten automatischen `onChanged(&Group)`-getriebenen Solve laufen
   lassen (Reihenfolge-Garantie erzwingen), oder `syncGroundedJoints()`s Lösch-Kriterium um
   eine Bestätigung über mehrere Solve-Zyklen hinweg erweitern, statt beim allerersten
   Treffer sofort zu löschen?

## FIX (2026-09-01): implementiert und live verifiziert

Umgesetzt als `patches/freecad-assembly-groundedjoint-race-condition.patch` (siehe dortige
`patches/README.md`-Sektion für die volle Beschreibung). Ansatz: Variante aus Frage 3 -
`syncGroundedJoints()`s Lösch-Zweig verlangt jetzt eine **zweite Bestätigung in einem
späteren `solve()`-Aufruf**, statt beim ersten Treffer sofort zu löschen. Ein einmaliger
Race-Treffer beim Laden räumt sich dadurch selbst wieder aus (das Flag ist beim nächsten
Solve normalerweise korrekt gesetzt), während der echte Anwendungsfall (Nutzer hebt die
Sperre dauerhaft manuell auf) weiterhin zuverlässig löscht.

**Live-Verifikation (Xvfb-Sandbox, 3x reproduziert, jedes Mal identisches Ergebnis):**
Repro-Sequenz aus diesem Dokument (Datei laden, Nutenstein manuell um 500mm verschieben,
mehrfach `assembly.touch(); assembly.recompute(True)` aufrufen) ergibt jetzt:
- `GroundedJoint` bleibt im Dokument erhalten, `ReadOnly`-Flag bleibt über alle Zyklen hinweg
  korrekt gesetzt.
- Der Solver bringt das verschobene Bauteil zuverlässig zurück in die korrekte Position.
- Gemessener Fixed-Joint-Flächenabstand (Face14↔Face5) danach: **0.0mm** - exakte Deckung.

Getestet an der SAUBEREN ZIP-Repro-Datei (`vor/CNC3018_023_A_Halterbaugruppe.FCStd` aus
`patches/bugreport-fixed-joint-no-coincidence/halterbaugruppe-nutenstein-repro.zip`). An der
ECHTEN, bereits anderweitig beschädigten Projektdatei (verwaistes Duplikat-Link-Objekt aus
einem früher nicht sauber rückgängig gemachten Teiletausch) bleibt das Grounding weiterhin
gestört - das ist eine separate Datenintegritätsfrage dieser einen Datei, keine Wiederholung
dieses Race-Condition-Bugs (das Duplikat-Objekt scheint nicht Teil der Assembly-`Group` zu
sein, dazu wäre eine eigene Untersuchung nötig).
