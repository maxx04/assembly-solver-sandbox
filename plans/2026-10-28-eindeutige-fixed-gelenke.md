# Plan: Fixed-Gelenke im Solver eindeutig machen (statt 180-Grad-Mehrdeutigkeit)

Stand 2026-10-28. Nutzerauftrag: „plane und probiere zu lösen“ (nach der umgeklappten zweiten
`BackslashNut` in BG22 und dem verworfenen Dauer-Bündeln, ARCHITECTURE.md §2.1c). Nutzer: ein
Konstrukteur darf nicht 100 Teile auf Umklappen prüfen müssen - also an die Mehrdeutigkeit in
OndselSolver ran.

## Problem

- OndselSolver `FixedJoint`: drei Gleichungen `yI·xJ = zI·xJ = zI·yJ = 0`. Lösungen: deckungsgleich
  oder 180 Grad um X, Y bzw. Z (#32477). Newton nimmt die, die der Startlage am nächsten liegt.
- FreeCAD kennt zwei gewollte Lagen: deckungsgleich oder „umgekehrt“ (180 Grad um X,
  Knopf „Ausrichtung wechseln“ = `flipOnePart()`, `preSolve()` wählt beim Anlegen nach der
  Z-Richtung). Die Wahl steht nirgends im Gelenk, nur in der Lage der Teile.
- Ein Spiegel in einer einbauenden Baugruppe kennt diese Wahl nicht und kann bei ungünstiger
  Startlage in die andere Variante rutschen (BG22: Mutter um 180 Grad geklappt, gespeichert).

## Lösung (umgesetzt, Build-Baum; nicht installiert, nicht committet)

### OndselSolver (Submodul-Patch)
- `FixedJoint::initializeGlobally()`: statt der drei Richtungskosinusse
  `zI·xJ = 0` + **Winkel von J um die Z-Achse von I = 0** + **Winkel um die X-Achse = 0**
  (beide über atan2, stetig). Einzige Lösung: deckungsgleich. Bei kleiner Drehung sind die drei
  Gleichungen genau die drei Drehkomponenten, also unabhängig.
- Neue Klassen `AngleXIeqcJeqc` (Ableitung von `AngleZIeqcJeqc`, nur die Richtungskosinusse
  (1,1)/(2,1) statt (0,0)/(1,0)) und `AngleXConstraintIqcJqc` (von `AngleZConstraintIqcJqc`).
  Ableitungen unverändert übernommen - der vorhandene Baustein wird schon für Gelenkgrenzen
  benutzt.

### FreeCAD (AssemblyObject)
- Der Solver verlangt jetzt Deckung; die gewollte Variante wird in den Marker J eingerechnet
  (`handleOneSideOfJoint(..., jcsTurn)`), bestimmt beim Modellaufbau von
  `fixedJointVariant()`:
  1. Lage nahe einer der vier früheren Lösungen (< 1e-3): **genau diese** - was heute steht,
     bleibt stehen, auch frühere stille Umklappungen (keine Prüfpflicht für den Konstrukteur).
  2. Verschachteltes Gelenk weit daneben (neuer/veralteter Spiegel, z.B. kurz nach
     Starr -> Flexibel): Variante aus dem **eigenen Dokument** des Gelenks.
  3. Sonst weit daneben: deckungsgleich oder umgekehrt nach der Z-Richtung, wie `preSolve()`;
     180 Grad um Y/Z nie.
- Danach kann Newton die Variante nicht mehr wechseln (Lösen, Simulation, schlechte Startlagen).
- Hilfen `jcsInPartFrame()` (aus `handleOneSideOfJoint()` herausgezogen),
  `snapFixedJointRelation()`, `fixedJointRelationInOwnDocument()`.

## Prüfung (Build-Baum)

| Prüfung | Ergebnis |
|---|---|
| Testreihe (run-all-tests) | 38/38 |
| Gelenktyp-Matrix | 60/60 |
| BG22 mit umgeklappt gespeicherter Mutter | bleibt wie gespeichert, 0 Widersprüche |
| BG22 frisch (Stand vor der Mutter) + neues BG37 | Mutter richtig, 0 Widersprüche |
| BG22 Starr -> Flexibel (Kopie) | Mutter richtig, **0 Widersprüche** (Bündel-Versuch: 16/7) |
| #32477-Nachstellung | Ergebnis „umgekehrt“ - wie `preSolve()` es wählen würde (siehe Offen) |
| Ziehen-Rundumtest, repariertes BG22 | neu 24/24, alt 24/24 |
| Ziehen-Rundumtest, BG22 wie um 12:29 gespeichert | neu und alt **identisch** 15 Verletzungen an BG22#Joint004 (0,55 mm) - kommt vom kaputt gespeicherten Stand, nicht vom neuen Solver |
| gespeichertes BG22 (12:29) neu gelöst | Mutter richtig, bleibt 1 Widerspruch Joint022 (vom missglückten Umschalten) |
| `PROJ_CNC3018/..._repariert.FCStd` neu gelöst | Mutter richtig, 0 Widersprüche |

## Stand

- 2026-10-28: umgesetzt und im Build-Baum geprüft (Tabelle oben).
- Danach: Nutzer hat das reparierte BG22 übernommen; Freigabe -> installiert. Mit der
  Installation: Testreihe 38/38, aktuelles BG22 (Kopie) Mutter richtig, 0 Widersprüche.
  Committet, Rundum-Patch und OndselSolver-Submodul-Patch neu.
- Ausstehend: GUI-Abnahme durch den Nutzer (BG37/BG22, Starr an/aus, Ziehen, Simulation).
- Nächster Schritt: Dreh-/Zylinder-/Schiebegelenk.

## Nachtrag: Slider, Dreh-, Zylindergelenk (2026-10-28)

Gleiches Muster umgesetzt (ARCHITECTURE.md §2.1d): Slider wie Fixed, Dreh/Zylinder mit zwei
Kippwinkeln der Z-Achse; Winkelbaustein verallgemeinert (`AxisAngle*`). Motion-getriebene Gelenke
(Simulation, `EndFrameqct`) behalten die alten Bedingungen - der erste Versuch ohne diese
Ausnahme stürzte in `runKINEMATIC` ab (test_simulation_nested). Variantenwahl `jointVariant()`.
Prüfung: Testreihe 39/39, Matrix Ziehen + Simulation, MotionProfiles 6/6, BG22 Mutter/Starr-
Flexibel ohne Widerspruch und Grenzverletzung.

Außerdem: nach „Turn rigid/flexible“ löst die GUI sofort (aktive bzw. einbauende Baugruppe).

## Offen

- **#32477-Erwartung:** das Skript erwartet bei entgegengesetzten JCS-Z-Achsen „deckungsgleich“;
  FreeCADs `preSolve()` wählt dort „umgekehrt“. Vor einem Upstream-Beitrag mit den Maintainern
  klären, welche Semantik gilt; der Solver-Teil (eindeutige Bedingung) ist davon unabhängig.
