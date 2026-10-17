# Plan: Redundanz innerhalb einer Toleranz schlucken (2026-10-14)

## Anlass

BG22 mit eingebauter BG89 (Tisch): Ein Joint anzulegen dauert über 30 s, und es kommen sehr viele
Warnungen „ueberbestimmt“.

Messung auf einer Kopie:

| Fall | Dauer eines Solves |
|---|---|
| mit Redundanz | 2,1–3,3 s |
| ohne die redundanten Gelenke | 0,6 s |

Python-Anteil: etwa 0,1 s pro Solve.

gdb-Stichproben aus dem Solve:

| Anteil | Wo |
|---|---|
| etwa 80 % | OndselSolver `PosICNewtonRaphson::lookForRedundantConstraints()` (vollständige Zerlegung der ganzen Matrix) |
| etwa 12 % | unsere `recordRedundancyGroups()` |
| etwa 3 % | Baugruppen-Code |

Diese Suche läuft bei **jedem** Solve von vorn, obwohl sich die Menge der redundanten Bedingungen
zwischen zwei Solves fast nie ändert. Der Erstell-Dialog löst 5- bis 10-mal (Referenz, Typ, Wert,
OK, Wiederholung).

Die Redundanzen stammen aus normalen Konstruktionsabsichten: Halter parallel zum Rahmen ausrichten,
Spindel parallel zur Führung. Sie sind geometrisch stimmig.

## Nutzerentscheidung

> „Solver soll z.B. zwei Führungen schlucken, in bestimmtem Toleranzbereich.“

- Stimmige Redundanz ist erlaubt. Es gibt dann keine Warnung und kein „Over-constrained“.
- Nur ein echter Widerspruch über der Toleranz wird gemeldet.
- Die Toleranz ist einstellbar.

## Regel (neu: C7 in docs/CONSTRAINTS.md)

Redundante Bedingungen innerhalb der Toleranz gelten als erfüllt.
- **Standard-Toleranz:** 0,01 mm bzw. 0,01°.
- **Einstellung:** `User parameter:BaseApp/Preferences/Mod/Assembly`, Parameter
  `RedundancyToleranceLength` und `RedundancyToleranceAngle`. Es kommt keine neue Eigenschaft in
  die Datei (C4).
- **Über der Toleranz:** Konflikt. Die Warnung nennt Gelenk, Abweichung und Gruppe, die GUI zeigt
  „Over-constrained“.

## Umsetzung

### 1. OndselSolver (eigene Patchdatei, Submodul)

- `System::knownRedundantConstraints`: eine Menge (Gelenkname, Index der Bedingung im Gelenk).
  - Wird in `System::runPreDrag()` nach `postInput()` angewendet: Die Bedingung wird wie in
    `Joint::removeRedundantConstraints()` in eine `RedundantConstraint` gehüllt.
  - Bleibt das System singulär, sucht der Solver wie bisher weiter. Die Menge wächst dann nur.
- `System::redundantConstraintStates()`: Für jede herausgenommene Bedingung werden Gelenkname,
  Index, Restwert `aG` (nach `prePosIC()` und `postPosICIteration()` der inneren Bedingung) und
  die Art geliefert.
  - Winkel bei `DirectionCosine*`. Dort ist der Restwert ein Kosinus, die Abweichung beträgt
    asin(|aG|).
  - Sonst Länge in mm. Die MbD-Einheiten sind auf 1 gesetzt (`ASMTAssembly`).

### 2. AssemblyObject

**Cache je Baugruppe** (nur im Speicher):
- Signatur aller ASMT-Gelenke und -Antriebe: Klasse, Name, MarkerI, MarkerJ.
- Liste der herausgenommenen Bedingungen.
- Zuletzt ermittelte Gruppen.

**Gültigkeit des Caches:** Alle gemerkten Signaturen sind noch vorhanden.
- Kommen Gelenke hinzu, bleibt der Cache gültig. Eine abhängige Zeile bleibt abhängig, wenn
  weitere Zeilen dazukommen.
- Fehlt ein Gelenk oder hat es sich geändert (Typ, Marker, starr/flexibel, Erdung), wird neu
  gesucht.

**Nach dem Solve:** Jede herausgenommene Bedingung wird nachgemessen.
- **Alle innerhalb der Toleranz:** geschluckt. Nur ein Log-Eintrag, `lastHasRedundancies` bleibt
  false, die Gelenke stehen nur informativ in `lastRedundantJoints`.
- **Eine über der Toleranz und der Cache wurde benutzt:** Cache verwerfen und einmal frisch lösen.
  Das schützt davor, dass eine gemerkte Bedingung inzwischen gar nicht mehr redundant ist.
- **Frisch gelöst und immer noch über der Toleranz:** echter Konflikt.
  `lastConflictingJoints` und `lastHasConflict` werden gesetzt, und es kommt eine Warnung mit
  Abweichung und Gruppe.

**Weitere Änderungen:**
- Die Gruppen-Analyse (`recordRedundancyGroups`) läuft nur bei frischer Suche. Ihr Ergebnis wird
  mitgemerkt.
- Freiheitsgrade: Herausgenommene Bedingungen werden nicht mehr abgezogen. Bisher machte jede
  stimmige Redundanz die Freiheitsgrade negativ und damit „Over-constrained“.
- Die Wiederholung bei Redundanz („loese erneut“) gibt es nur noch bei Konflikt.
- Ziehen: In `postDrag()` werden die herausgenommenen Bedingungen nachgemessen. Bei Verletzung
  wird der Cache verworfen.

### 3. GUI (ViewProviderAssembly)

- „Over-constrained“ nur bei Konflikt (`getLastConflicting()`), nicht mehr bei stimmiger
  Redundanz.
- Python `getRedundancyStates()`: je herausgenommener Bedingung Gelenk, Abweichung, Einheit,
  geschluckt ja/nein (für Tests).

## Tests

- **Neu `test_redundancy_tolerance_flat`:**
  - Zwei parallele Gleiter (zwei Führungen), geometrisch stimmig: Solve rc 0, kein Konflikt,
    zweiter Solve nutzt den Cache und ist schneller (Log), Ergebnis identisch.
  - Zweite Führung um 1 mm versetzt: Konflikt gemeldet.
  - Gelenk gelöscht, das eine gemerkte Redundanz trug: Cache verworfen, Ergebnis wie frisch.
- Volle Matrix und Drag-Rundumtest.
- BG22-Kopie: Solve-Zeit vorher und nachher, Meldungen vorher und nachher.

## Risiken

- **Stellungsabhängige Redundanz** (nur in einer bestimmten Lage abhängig): Nachmessen nach jedem
  Solve und Drag deckt das ab, dann wird frisch gelöst.
- **Upstream weicht ab:** Wird in docs/ARCHITECTURE.md als bewusste Abweichung dokumentiert, wie
  bei den Gelenkgrenzen (§3.2c).

## Umsetzungsstand (2026-10-14)

**Umgesetzt wie geplant, mit einer Ergänzung:** Redundanzen werden nach **Rangfolge** bestimmt
(`PosICNewtonRaphson::redundantEqnsByPriority()`).

Grund für die Ergänzung: Der erste Test zeigte, dass OndselSolvers zufällige Auswahl im Fall zwei
Führungen die Drehbedingung des starren Gelenks herausnahm. Der 1 mm Versatz wurde dann als
4,5°-Verdrehung der zweiten Stange aufgelöst und war damit nie innerhalb der Toleranz.

Zwei weitere Funde:
- Bekannte Konflikte werden mitgemerkt. Nur ein **neuer** Konflikt führt zu einem frischen Solve.
- Die Schwelle liegt bei 1e-6 statt 1e-8: An BG22 blieb bei einer um 89° verletzten
  Parallel-Bedingung ein Rest von ~1e-7.

**Messung BG22 (Kopie):**

| | vorher | nachher |
|---|---|---|
| Dauer eines Solves | 2,1–3,3 s | 0,66 s |
| Meldung | 2–3 redundante Gelenke | 1 echter Konflikt |

Der Konflikt ist Joint015 (Parallel): Seine Achsen stehen tatsächlich 89,5° zueinander, gemessen
über `getJcsGlobalPlc`. Die Spindel parallel zur Führung (Joint014) wird geschluckt.

**BG11:** 75 stimmige Redundanzen werden geschluckt.

**Tests:** Matrix 35/35, Drag-Rundumtest PASS.

## Nachtrag (2026-10-14, GUI-Messung des Nutzers)

Das Ändern einer Referenz und das OK dauerten noch je etwa 8 s. Zwei Ursachen:

1. **Spurious singulär:** Auch mit Gedächtnis meldete der schnelle Löser jedes Mal „singulär“.
   Die Suche nach Rangfolge fand nichts, danach lief die teure volle Pivotsuche. Jetzt kommt
   vorher `GESpMatParPvPrecise`. Auslöser war ein um 89° verletztes Gelenk: Joint015 stand auf
   „Rechtwinklig“ zwischen zwei parallelen Kanten.
2. **Gedächtnis-Pendeln:** `preDrag()` löst mit gebündelten Fixed-Ketten, das ist ein anderes
   Modell als der normale Solve. Jetzt hat jedes der drei Modelle ein eigenes Gedächtnis.

**Nutzer danach:** Die Referenzänderung dauert noch etwa 5 s statt 8 s.

**BG22 mit Joint015 auf Parallel:** 0,73 s pro Solve, kein Konflikt. Das Drehen des Motors (Rotor
in BG73) um 180° funktioniert, je Schritt 0,04 s.
