# Plan: Addon „Bewegungsprofile“ für die Assembly-Simulation

Stand 2026-10-28. Nutzeridee: Gelenke in der Simulation über 2D-Kurven ansteuern, mit Auswahl
Weg / Geschwindigkeit / Beschleunigung, wie der Servomotor in PTC Creo. Die Formeln im heutigen
Motion-Dialog sind nicht offensichtlich.

Entscheidungen (Nutzer 2026-10-28): **eigenes Addon-Repo**, erste Version **Profile + Vorschau**
(Auswertung nach der Simulation später).

## Grundidee

Kein Eingriff in Solver oder Assembly-Modul. Das Addon übersetzt ein Profil in eine normale
`Motion.Formula` (Positionsverlauf über `time`). Die Formelsprache von OndselSolver
(`SymbolicParser.cpp`) kann dafür genug:

- Operatoren `+ - * / ^`, Klammern, `pi`, `sin`, `cos`, `arctan`, `asin`, `abs`
- `spline(time, Grad, t0, w0, t1, w1, ...)` – Interpolation durch Tabellenpunkte (Grad 1 oder 3)
- `piecewise(...)`, `rampstep(...)`, `integral(...)`

Die Formel ist Standard → Dateien bleiben in Vanilla-FreeCAD lesbar und simulierbar, auch ohne
Addon (nur das Profil bearbeiten geht dann nicht). Keine Änderung an bestehenden Eigenschaften.

## Bedienung

Neues Aufgabenfenster „Bewegungsprofil“ (Befehl in der Assembly-Werkbank bzw. eigene Toolbar),
aufgerufen auf einer vorhandenen Motion oder einem Gelenk:

1. **Gelenk** (Dreh-/Schiebe-/Zylinder-Gelenk), Art **Drehung/Verschiebung** aus dem Gelenktyp.
2. **Größe** (Dropdown): Weg | Geschwindigkeit | Beschleunigung.
3. **Profil** (Dropdown) mit passenden Feldern:

| Profil | Formel f(t) der gewählten Größe | Felder |
|---|---|---|
| Konstant | A | A |
| Rampe | A + B·t | A, B |
| Cosinus | A·cos(2π·t/T + φ) + B | A, T, φ, B |
| Zykloidisch | L·(t/T − sin(2π·t/T)/(2π)) | L, T |
| Parabolisch | A·t + ½·B·t² | A, B |
| Polynom | a + b·t + c·t² + d·t³ | a, b, c, d |
| SCCA | Sinus-Konstant-Cosinus-Beschleunigung | A, B, H, T |
| Tabelle | Punkte (t, Wert), linear oder kubisch | Tabellen-Editor |

4. **Anfangswerte**: Weg relativ zur aktuellen Stellung (`initialValue`), bei Beschleunigung
   zusätzlich Anfangsgeschwindigkeit.
5. **Einheiten**: Grad bzw. mm im Dialog, intern rad bzw. mm.
6. **Vorschau**: drei Diagramme Weg / Geschwindigkeit / Beschleunigung über der Zeit
   (Matplotlib, in FreeCAD enthalten), aktualisiert bei jeder Eingabe; Zeitbereich aus der
   Simulation (Start/Ende).

## Umrechnung in die Motion-Formel

- **Analytische Profile**: geschlossen integriert (Geschwindigkeit → 1×, Beschleunigung → 2×),
  Ergebnis als exakter Ausdruck in `time`, z.B. Geschwindigkeit „Rampe“ →
  `initialValue + A*time + 0.5*B*time^2`. Exakt, Ableitungen stimmen (der Solver leitet
  symbolisch ab).
- **Tabelle**: Weg direkt als `spline(time, 3, ...)`; bei Geschwindigkeit/Beschleunigung in
  Python numerisch integriert (fein abgetastet), dann als Weg-`spline(...)`.
- Die Profildefinition wird in eigenen Eigenschaften an der Motion abgelegt (Gruppe
  „Bewegungsprofil“, z.B. `ProfileQuantity`, `ProfileType`, `ProfileParameters`,
  `ProfileTable`), damit das Aufgabenfenster sie wieder öffnen kann. Rein additiv.

## Aufbau des Addons

```
MotionProfiles/
  package.xml, README.md, LICENSE
  InitGui.py                 Befehl registrieren (Assembly-Werkbank-Toolbar/Menue)
  motionprofiles/
    profiles.py              Profile: Werte, Ableitungen, Integrale, Formel-Erzeugung (rein Python, ohne FreeCAD)
    formula.py               Formel-Text, spline()-Erzeugung, Einheiten
    taskpanel.py             Aufgabenfenster (Qt), Vorschau (Matplotlib)
    motion_link.py           Eigenschaften an der Motion lesen/schreiben
  tests/
    test_profiles.py         reine Mathematik (pytest, ohne FreeCAD)
    test_simulation.py       headless in FreeCAD: Profil -> Motion -> generateSimulation ->
                             gemessener Gelenkwert gegen Profil (Vanilla-Weekly und Sandbox)
```

## Phasen

1. **Kern** `profiles.py` + `formula.py` mit Tests: jedes Profil × jede Größe, Formel-Text wird von
   OndselSolver akzeptiert, Weg/Geschw./Beschl. stimmen numerisch.
2. **Simulationstest headless**: Drehgelenk-Fixtur (aus jointtype-matrix), Profil erzeugen,
   `generateSimulation()`, gemessene Stellung je Bild gegen Sollkurve (Vanilla + Sandbox).
3. **Aufgabenfenster + Vorschau**.
4. **Addon-Gerüst** (package.xml, Installation über den Addon-Manager aus eigenem Repo).
5. Später: Auswertung (Kurven beliebiger Gelenke nach der Simulation), Export CSV.

## Offen / zu klären

- Name und Ort des Repos (Vorschlag: `MotionProfiles`, lokal neben der Sandbox, GitHub-Repo nur
  nach Rückfrage).
- Mehrere Abschnitte hintereinander (z.B. Rampe hoch – konstant – Rampe runter) in Version 1 nur
  über die Tabelle; echte Segmentkette ggf. Version 2.
- Simulation beachtet Gelenkgrenzen nicht (bekannter Punkt) – im Dialog als Hinweis anzeigen.

## Stand 2026-10-28: Phase 1 + 2 erledigt

Repo `/home/maxx/Dokumente/FreeCAD-Development/MotionProfiles` (Nutzer: Name/Ort passt,
GitHub ja). Befunde vorab in echter Simulation (Sandbox + Vanilla identisch):
- `spline(...)` in Motion-Formeln: generateSimulation scheitert (rc=-1) -> nicht verwendet.
- `piecewise(time, functions(..), transitions(..))`, `^`, sin/cos: exakt.
- Bild 0 = Ausgangslage, Bild k = Zeit Start + (k-1)*Schritt.

Darstellung: stueckweise Polynome + sin/cos (`segments.py`), exakte Integration je Abschnitt,
Tabellen als natuerlicher kubischer bzw. linearer Spline in Python. SCCA mit A (Flanken),
B = 1/2 - A. 9 reine Python-Tests; `fc_simulation_check.py`: 6 Faelle (Dreh/Schiebe, alle drei
Groessen) in Sandbox und Vanilla max. 6e-12.
