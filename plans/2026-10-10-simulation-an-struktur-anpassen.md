# Plan: Assembly-Simulation an unsere Struktur anpassen

Stand: 2026-10-10. Bezug: docs/CONSTRAINTS.md (C1 starr/flexibel, C2 Originale unverändert,
C3 Erdung als Kontext-Eigenschaft, C4 Dateikompatibilität).

## Kontext

Die Simulation (Arbeitsbereich Assembly → „Simulation erstellen“) besteht aus:

- **Python:** `CommandCreateSimulation.py`. Enthält das Simulations-Objekt, Motion-Objekte
  (Gelenk + Bewegungsart + Formel), den Aufgabendialog mit Abspielen und das Video-/GIF-Export.
- **C++:** `AssemblyObject::generateSimulation()` baut das MbD-Modell, hängt die Motions an die
  Gelenke (`makeMbdJoint()`) und rechnet alle Bilder mit `runKINEMATIC()`.
  `updateForFrame()` schreibt ein Bild zurück (`setNewPlacements()`).

## Bestandsaufnahme (headless an PROJ_CNC3018, Rotor-Gelenk von BG67 als Antrieb)

Test: Motion „Angular“, Formel `initialValue + 2π·time` (eine Umdrehung pro Sekunde), in BG43,
BG37 (BG43 flexibel) und BG22.

| Punkt | Ergebnis |
|---|---|
| Läuft durch, Bilder entstehen | ja, in allen drei Kontexten |
| Originale in anderen Dokumenten verändert | nein (C2 eingehalten) |
| Buchse folgt dem Rotor | ja, exakt |
| **Startwinkel** | **falsch**: beim ersten Bild springt der Rotor (BG43 −30°, BG37 +12°, BG22 +8°), danach läuft er mit diesem Versatz korrekt weiter |
| Dauer `generateSimulation()` | BG43 0,6 s, BG37 3,5 s, **BG22 9,7 s** |
| Gelenk-Auswahlliste | enthält verschachtelte Gelenke; in BG22 kommen Gelenke **doppelt** vor (mehrfach eingebaute Unter-BG) |

### Ursachen

1. **Startwinkel (`initialValue`):** `getJointCurrentValue()` (AssemblyUtils.cpp) misst den
   Gelenkwert über `getGlobalPlacement(nullptr, ref)`, also an den **Originalen** im Dokument des
   Gelenks. Im Kontext einer äußeren BG gilt aber der Stand der **Spiegel** (C2). Der Unterschied
   zwischen beiden wird beim ersten Bild als Sprung sichtbar.
2. **Dauer:** `generateSimulation()` baut das MbD-Modell wie `solve()`, aber ohne
   `ScopedIdentityGraph`. Jede Auflösung baut ihren eigenen IdentityGraph neu, dieselbe Ursache
   wie beim früheren BG22-Hänger (todo-bg22-gui-recompute-loop).
3. **Doppelte Gelenke:** `Joints` (Python) liefert die Gelenk-OBJEKTE aller Instanzen, ohne Instanzpfad. Ist eine
   Unter-BG zweimal eingebaut, erscheint ihr Gelenk zweimal. Die Motion speichert nur das
   Gelenk-Objekt (`Joint`, XLink). In `makeMbdJoint()` wirkt sie deshalb auf **alle** Instanzen
   dieses Gelenks.
4. **Bezeichnung in der Auswahlliste:** nur `joint.Label` (z.B. „Joint“). Verschachtelte Gelenke
   sind so nicht unterscheidbar.

## Umsetzung

### Schritt 1 — Startwinkel aus dem Kontext (Fehler, klein)
- In `makeMbdJoint()` den aktuellen Gelenkwert aus den **Solver-Körpern dieses Kontexts**
  bestimmen. JCS je Seite = Startlage des MbD-Körpers (`getMbdPlacement(data.part)`, also die
  Instanz samt Container-Kette) × `offsetPlc` × Gelenk-Placement relativ zum Teil, genau wie
  `handleOneSideOfJoint()` den Marker setzt.
- Die eigentliche Formel (Winkel aus der x-Achse, Abstand mit Vorzeichen) als Hilfsfunktion aus
  `getJointCurrentValue()` herausziehen und von beiden nutzen. `getJointCurrentValue()` selbst
  bleibt für die übrigen Aufrufer unverändert.

### Schritt 2 — ein IdentityGraph je Simulation (Leistung, klein)
- `ScopedIdentityGraph graphScope(this, scopedGraph, /*fresh=*/true);` am Anfang von
  `generateSimulation()`, wie in `solve()`. Ziel BG22: deutlich unter 9,7 s.

### Schritt 3 — Gelenk-Auswahlliste eindeutig machen (UX, klein)
- Je Gelenk-INSTANZ ein Eintrag (siehe Schritt 4), angezeigt als „Gelenk — eingebaute Instanz“
  statt nur „Joint“/„Gleitverbindung“.
- Gelenke in **starren** Unter-BGs erscheinen nicht. Sie werden im äußeren Kontext nicht gelöst
  (C1); `getJoints()` liefert sie heute schon nicht.

### Schritt 4 — Motion pro Instanz (Nutzerentscheidung 2026-10-10)
Ein Gelenk einer mehrfach eingebauten Unter-BG ist in der äußeren BG **je Instanz ein eigenes
Gelenk**: eigener Schlitten, eigene Stellung, im Solver ein eigenes MbD-Gelenk
(`jointInstanceName()`). Eine Motion muss deshalb genau eine Instanz antreiben können.

- **Speichern ohne neue Property (C4):** Die vorhandene Property `Motion.Joint`
  (`App::PropertyXLinkSubHidden`) nimmt neben dem Gelenk-Objekt einen Unterpfad auf. Dort steht
  der Verschachtelungspfad der Instanz, also das `nestingPrefix` des JointRef, z.B.
  `CNC3018_032_A_FuehrungsBaugruppe401.`.
- **Auswertung:** In `makeMbdJoint()` greift die Motion nur, wenn Gelenk-Objekt UND
  `nestingPrefix` übereinstimmen. Ist der Unterpfad leer (alte Dateien, Gelenk direkt in der BG),
  gilt die Motion wie bisher für jede Instanz.
- **Auswahlliste:** je Instanz ein Eintrag, bezeichnet mit dem Namen der eingebauten Instanz.
- **Geprüft 2026-10-10** an einer Kopie von BG22:
  - Gepatcht: Unterpfad bleibt nach zweimaligem Speichern und Laden unverändert, ohne Warnung;
    in der Datei als `<XLink … name="Joint" sub="CNC3018_032_A_FuehrungsBaugruppe401."/>`.
  - Vanilla (install-clean-test): öffnet die Datei, liest den Unterpfad und behält ihn beim
    Speichern bei. Vanilla ignoriert ihn bei der Auswertung, die Motion wirkt dort also wie
    bisher auf alle Instanzen.

### Schritt 5 — Tests
- Neuer Kinematik-Test `test_simulation_nested` mit Fixturen flach, verschachtelt und doppelt
  verschachtelt (aus den bestehenden Revolute-Fixturen):
  - Bild 0 = Ausgangslage, Bild k = Startwinkel + Formel (Toleranz 1e-6)
  - Gelenke erfüllt
  - Originale unverändert
  - Abbrechen des Dialogs stellt die Ausgangslage wieder her
- Volle Matrix, Messung an BG22 (Dauer, Startwinkel).

### Nicht im Umfang
- Gelenkgrenzen während der Simulation: Upstream schaltet sie bewusst ab, in diesem Plan nicht
  geändert. Seit 2026-10-11 als TODO vorgemerkt (docs/ARCHITECTURE.md §3.2c), weil der normale
  Solve Grenzen inzwischen beachtet.
- Drehformel in `tryMouseMove()` (lokale × globale Lage, ARCHITECTURE 3.2b): betrifft das Ziehen,
  nicht die Simulation.

## Umsetzungsstand 2026-10-10

| Schritt | Stand |
|---|---|
| 1 Startwinkel aus dem Kontext | umgesetzt. Kein Sprung mehr; BG43/BG37/BG22 identisch: Bild 0 = Ausgangslage, ab Bild 1 time 0…1 s, exakt 360° bei `initialValue + 2π·time`. |
| 2 ein IdentityGraph je Simulation | umgesetzt, aber **ohne Zeitgewinn**: gdb-Stichproben zeigen 97 % der ~10 s in `runKINEMATIC()` (OndselSolver, 101 Zeitschritte). Nicht unser Code. |
| 3 Auswahlliste je Instanz | umgesetzt. Neue Python-Methode `AssemblyObject.getJointInstances()` → (Gelenk, Instanzpfad); `UtilsAssembly.getJointInstancesOfType()`/`jointInstanceDisplayName()`; Anzeige „Gelenk — Instanz“, Tooltip mit vollem Pfad. |
| 4 Motion pro Instanz | umgesetzt. `motionMatchesJointInstance()` in `makeMbdJoint()`; Unterpfad in `Motion.Joint`. BG22: mit Pfad `…401` bewegt sich nur dieser Schlitten, ohne Pfad beide. |
| 5 Tests | `test_simulation_nested` (flach, verschachtelt, doppelt verschachtelt; Original im Unterdokument vorher um 30° gedreht, damit Original ≠ Spiegel), 3/3. Volle Matrix 21/21. |

Nicht durch Fixtur abgedeckt: Motion pro Instanz (keine Revolute-Fixtur mit doppelt eingebauter
Unter-BG), nur an PROJ_CNC3018/BG22 headless geprüft.
