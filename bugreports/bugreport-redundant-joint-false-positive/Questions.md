# Frage an Assembly-Solver-Kenner: Was genau zählt der Solver als "redundant"?

**Datum:** 2026-08-24
**Betroffene Version:** FreeCAD 26.3.0 (Git, selbst kompiliert, `Libs: 26.3.0devR48232`)
**Repro-Datei:** [motor-nema17x33-repro.zip](./motor-nema17x33-repro.zip) — Hauptdatei `CNC3018_068_A_NEMA.FCStd` + 19 verlinkte Unterelemente (alle im selben Ordner entpacken, dann die Hauptdatei öffnen)

## Beobachtung

Beim Aufbau einer neuen, bewusst "sauberen" Joint-Struktur für eine NEMA17x33-Motor-Baugruppe
(als Vergleich zu einer älteren, per Rigid Group aufgebauten Version) tauchte wiederholt folgende
Warnung auf:

```
Wrn: Assembly: Solve of 'CNC3018_068_A_NEMA#CNC3018_067_A_NEMA17x33_III_Y' finished with
     4 redundant joint(s): Joint003, Joint004, Joint011, Joint012.
```

`Joint003` und `Joint004` verbinden dasselbe Teile-Paar (`CNC3018_051_B...` und
`CNC3018_048_B...`), genau wie `Joint010`/`Joint011`/`Joint012` (`CNC3018_050_B...` und
`CNC3018_049_B...`).

**Aber:** Empirisch geprüft (Joint einzeln unterdrückt, neu berechnet, Teil in der 3D-Ansicht
beobachtet) ist **keiner der beiden als redundant markierten Joints tatsächlich überflüssig**:

- `Joint003` sperrt die Bewegung in **X**-Richtung
- `Joint004` sperrt die Bewegung in **Z**-Richtung

Lässt man einen der beiden weg, wird das Teil in genau dieser Richtung beweglich (das Solve
"funktioniert" dann zwar noch, aber das Teil hat einen ungewollten Freiheitsgrad). Beide Joints
sind also für eine vollständige Festlegung des Teils nötig - sie schränken unterschiedliche,
sich NICHT überschneidende Freiheitsgrade ein.

## Die eigentliche Frage

Was prüft der Solver konkret, um einen Joint als "redundant" zu klassifizieren?

1. Ist es eine echte numerische Rang-Analyse der Jacobi-/Constraint-Matrix (die erkennen würde,
   dass Joint003 und Joint004 tatsächlich unabhängige Zeilen beisteuern und daher NICHT redundant
   sind)?
2. Oder ist es (wie die Beobachtung nahelegt) eine einfachere Heuristik, die im Wesentlichen
   "verbindet ein weiterer Joint dasselbe Teile-Paar erneut?" prüft, unabhängig davon, welche
   konkreten Freiheitsgrade dieser Joint tatsächlich einschränkt?

Falls (2): gibt es einen empfohlenen Weg, ein Teil über zwei (oder mehr) unabhängige Joints mit
demselben Nachbarteil zu verbinden - je einen pro eingeschränktem Freiheitsgrad -, ohne die
"redundant"-Warnung als Fehlalarm in Kauf nehmen zu müssen? Oder ist die Warnung in diesem Fall
grundsätzlich ignorierbar (also: informativ, aber ohne Auswirkung auf die berechnete Lösung)?

## Zusammenhang mit einem zweiten, verwandten Befund

Bei der ALTEN Version derselben Baugruppe (Rigid Group statt Einzel-Joints, 18 Teile in einer
`RigidGroupJoint`) tritt statt der Warnung ein harter Fehler auf:

```
Err: <Assembly> AssemblyObject.cpp(260): Solve failed: To be implemented.
```

...ausgelöst nach vielen Wiederholungen von "MbD: Checking for redundant constraints." Die
Vermutung: dieser Absturz betrifft speziell den Redundanz-Auflösungs-Code-Pfad für
`RigidGroupJoint`-Objekte (mit vielen intern potenziell redundanten Bindungen), während einzelne,
gewöhnliche Joints (wie oben) bei Redundanz nur eine Warnung erzeugen und trotzdem sauber zu Ende
rechnen. Ist das zutreffend - und falls ja: ist der `RigidGroupJoint`-Redundanz-Code tatsächlich
(wie die Fehlermeldung wörtlich sagt) schlicht noch nicht implementiert, oder gibt es einen
bekannten Workaround (z.B. eine bestimmte Formulierung der Rigid-Group-Mitgliederliste), der den
Absturz vermeidet?

## Repro-Schritte (aus dem beigefügten ZIP)

1. Alle Dateien aus `motor-nema17x33-repro.zip` in denselben Ordner entpacken.
2. `CNC3018_068_A_NEMA.FCStd` öffnen, komplett neu berechnen.
3. Report View beobachten: "finished with 4 redundant joint(s): Joint003, Joint004, Joint011,
   Joint012."
4. `Joint003` per Rechtsklick unterdrücken ("Suppress"), neu berechnen. Beobachtung: Teil
   `CNC3018_051_B_NEMA17x33_III_Y` bewegt sich in X-Richtung frei (Solver akzeptiert die
   fehlende Bindung klaglos).
5. `Joint003` wieder aktivieren, stattdessen `Joint004` unterdrücken, neu berechnen. Beobachtung:
   dasselbe Teil bewegt sich stattdessen in Z-Richtung frei.
6. => Beide Joints sind einzeln notwendig, werden aber gemeinsam als "redundant" gemeldet.
