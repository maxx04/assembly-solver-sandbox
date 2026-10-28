# Faktenblatt: Fixed-, Slider- und Drehgelenk landen auf falschen 180-Grad-Lösungen (#32477) – Lösungsvorschlag

Stand 2026-10-28. Unterlage für den Nutzer, um Maintainer auf Interesse anzusprechen (FreeCAD-Regel:
Issue-/PR-Texte schreibt ein Mensch). Kein fertiger Text. Bezug: FreeCAD Issue #32477 (eigener
Bericht vom 2026-09-08, Material in `bugreports/fixed-joint-180-degree-flip/`).

## 1. Problem (Vanilla nachgewiesen)

OndselSolver legt die Drehung einiger Gelenke nur über Richtungskosinusse fest:

| Gelenk | Bedingungen (OndselSolver) | Lösungen |
|---|---|---|
| Fixed (`FixedJoint`) | `yI·xJ = zI·xJ = zI·yJ = 0` | 4: deckungsgleich, 180° um x, y oder z |
| Slider (`TranslationalJoint`) | dieselben drei | 4 |
| Drehgelenk, Zylinder (`RevoluteJoint`, `CylindricalJoint`) | `zI·xJ = zI·yJ = 0` | 2: z gleich oder entgegengesetzt |

Newton landet auf der Lösung, die der Startlage am nächsten ist. FreeCAD selbst kennt aber nur zwei
gewollte Lagen: deckungsgleich oder „umgekehrt“ = 180° um x (z-Achsen gegeneinander; Knopf
„Ausrichtung wechseln“ → `JointObject.flipOnePart()`, `preSolve()`/`matchJCS()` wählt beim Anlegen
nach der z-Richtung, `UtilsAssembly.arePlacementSameDir()`). **180° um y oder z ist nie gewollt.**

Repro `orientation_roots.py` (flach: geerdete Box A, freie Box B, Gelenk Oberseite A ↔ Unterseite B,
B aus verschiedenen Startlagen), FreeCAD Weekly 2026.10.21:

| Start (Drehung von B) | Fixed Vanilla | Fixed Sandbox | Slider Vanilla | Slider Sandbox |
|---|---|---|---|---|
| 0° / 60° um x | deckungsgleich | deckungsgleich | deckungsgleich | deckungsgleich |
| 120° um x | 180° x | 180° x | 180° x | 180° x |
| **120° um y** | **180° y** | 180° x | **180° y** | 180° x |
| **120° um z** | **180° z** | deckungsgleich | **180° z** | deckungsgleich |
| **170° um z** | **180° z** | deckungsgleich | **180° z** | deckungsgleich |
| 150° um (1,1,0) | 180° x | 180° x | 180° x | 180° x |
| 100° um (1,1,1) | deckungsgleich | deckungsgleich | deckungsgleich | deckungsgleich |

Drehgelenk: flach in beiden gleich (z richtet sich nach der Startlage). Der Gewinn dort ist, dass
die einmal gewählte Richtung nicht mehr wechseln kann und verschachtelte Spiegel die Richtung aus
dem Original bekommen (siehe 3.).

Praxisfall (Sandbox-Projekt CNC3018): eine Mutter (Fixed am Träger) stand in der einbauenden
Baugruppe um 180° umgeklappt und wurde so gespeichert; der Konstrukteur hätte jedes Teil prüfen
müssen.

## 2. Lösung im Solver (Patch `ondselsolver-unique-joint-orientation.patch`)

Gegen unverändertes OndselSolver `4be80ee` (Stand im FreeCAD-Submodul): `git apply` sauber,
eigenständiger Build ok.

- Neuer Baustein `AxisAngleIeqcJeqc` / `AxisAngleConstraintIqcJqc`: Winkel atan2(sin, cos) aus zwei
  frei wählbaren Richtungskosinussen. Abgeleitet von `AngleZIeqcJeqc` (gleiche Ableitungen,
  stetiger Winkel) – nur die Richtungskosinusse unterscheiden sich.
- `FixedJoint`, `TranslationalJoint`: `zI·xJ = 0` + Winkel von J um z von I = 0
  (`AngleZConstraintIqcJqc`, schon vorhanden) + Winkel um x = 0. Einzige Lösung: deckungsgleich.
  Bei kleiner Drehung sind die drei Gleichungen genau die drei Drehkomponenten (unabhängig).
- `RevoluteJoint`, `CylindricalJoint`: Kippwinkel von zJ um x und um y von I = 0 statt
  `zI·xJ = zI·yJ = 0`. Bei zJ = −zI wären die Kippwinkel 180°, diese Lösung entfällt; Drehung um z
  bleibt frei.
- **Ausnahme Motion:** treibt eine Motion das Gelenk, ist das Bezugssystem I zeitabhängig
  (`EndFrameqct`). Für die Winkelbausteine gibt es keine zeitabhängige Variante (die alten
  Richtungskosinusse haben `…IqctJqc`) – ohne Ausnahme stürzte `runKINEMATIC` ab. Solche Gelenke
  behalten die alten Bedingungen (Simulation startet aus der gelösten Lage). Sauberer: eine
  `…IqctJqc`-Variante des Winkelbausteins.

## 3. Nötige FreeCAD-Seite (nur zusammen mit 2.!)

Der Solver verlangt danach **Deckung**. Gelenke, die heute „umgekehrt“ stehen, würden ohne
FreeCAD-Anteil umklappen. Darum muss FreeCAD die gewollte Variante beim Modellaufbau in den Marker J
einrechnen (Marker J um 180° um x drehen, wenn „umgekehrt“). Gelenkgrenzen und Motion-Winkel
messen an der x-Achse von J, die diese Drehung nicht ändert.

Variantenwahl in der Sandbox (`AssemblyObject::jointVariant()`):
1. Lage nahe einer der früheren Lösungen (< 1e-3): genau diese – was heute steht, bleibt stehen
   (auch 180° y/z aus älteren Dateien, damit nichts sichtbar umspringt).
2. Verschachteltes Gelenk weit daneben (neuer/veralteter Spiegel): Variante aus seinem eigenen
   Dokument (Sandbox-spezifisch).
3. Sonst: deckungsgleich oder umgekehrt nach z-Richtung, wie `preSolve()`.

Für Upstream (keine Verschachtelung wie in der Sandbox) reichen 1. und 3. Die Sandbox-Fassung
nutzt Sandbox-Infrastruktur (`resolvePartForMbD()`, IdentityGraph) und ist kein direkter
Upstream-Patch; Code: `src/Mod/Assembly/App/AssemblyObject.cpp` (`jointVariant()`,
`snapFixedJointRelation()`, `jcsInPartFrame()`), Sandbox-Commits `bbc18c61c2`, `aee9404743`.

Alternative zur Diskussion: die Variante dauerhaft im Gelenk speichern (Eigenschaft
„umgekehrt“, vom Knopf gesetzt). Wäre eindeutiger, ändert aber das Dateiformat.

## 4. Offene Frage an die Maintainer

Das Skript zu #32477 erwartet bei entgegengesetzten JCS-z-Achsen im Start „deckungsgleich“.
FreeCADs eigenes `preSolve()` wählt dort „umgekehrt“. Welche Semantik gilt? Der Solver-Teil (2.)
ist davon unabhängig – er macht die Lösung nur eindeutig; die Wahl trifft FreeCAD (3.).

## 5. Prüfung in der Sandbox

- Kinematik-Testreihe 39/39 (flach, verschachtelt, doppelt, Duplikate, Simulation, Grenzen).
- Gelenktyp-Matrix 64 Fälle beim Ziehen und per Simulation.
- Ziehen-Rundumtest am Projekt (24 Gelenke), BG22: umgeklappte Mutter steht richtig,
  Starr/Flexibel-Umschalten ohne Widerspruch.

## 6. Nicht abgedeckt

- `PlanarJoint` (u.a. Abstand Ebene–Ebene) und `ParallelAxesJoint`: dort ist „z gleich oder
  entgegengesetzt“ teilweise gewollt – zu klären (Sandbox-TODO).
- Zeitabhängige Variante der Winkelbausteine (siehe 2., Ausnahme Motion).

## Dateien

- `orientation_roots.py` – Repro und Tabelle oben.
- `ondselsolver-unique-joint-orientation.patch` – Solver-Teil gegen OndselSolver `4be80ee`.
- `../fixed-joint-180-degree-flip/` – ursprünglicher Bericht #32477.
