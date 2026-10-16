# Plan: neues Gelenk „Tangent“, erster Schritt: Vertex auf Spline

Stand: 2026-10-16. Nur Plan, noch keine Umsetzung. Bezug: docs/CONSTRAINTS.md (C2, C4).

## Ziel

Neuer Gelenktyp **Tangent** wie das Tangent-Mate in Onshape. Nutzerentscheidung 2026-10-16:
**zuerst reicht Vertex auf Spline.** Ein Punkt (Vertex) von Teil A bleibt auf einer Kurve
(B-Spline, auch Bogen/Ellipse) von Teil B und kann entlang der Kurve gleiten. Das Gelenk nimmt
2 Freiheitsgrade weg; die Drehung von A bleibt frei. Weitere Paare (Ebene/Zylinder/Kugel,
Fläche–Fläche) kommen später, siehe „Spätere Schritte“.

## Bestandsaufnahme

- **Heute falsch:** Distance mit Vertex + Kurve ergibt `DistanceType::PointCurve`
  (`AssemblyUtils.cpp:365ff.`) und daraus ein `ASMTPointInPlaneJoint` (`AssemblyObject.cpp`,
  `makeMbdJointDistance()`). Der Punkt bleibt nur in der Ebene der Kurve und kann darin frei
  wandern. Der Kommentar dort nennt das selbst als offene Baustelle.
- **OndselSolver kennt keine Kurven-Geometrie.** `ASMTRefCurve` ist ein leerer Platzhalter
  (`parseASMT()`/`storeOnLevel()` werfen „To be implemented“). `GeneralSpline` ist eine
  1D-Funktion für Motion-Formeln, keine Raumkurve. Es braucht also einen **Solver-Patch**.
- **Vorlage im Solver:** `PointInLineJoint` (Punkt auf der z-Achse von Marker J) besteht aus zwei
  `TranslationConstraintIJ` entlang der x- und y-Achse von J. Genau diese Struktur brauchen wir,
  nur dass die „Gerade“ die **Tangente der Kurve am Fußpunkt** ist und mitwandert.
- Gelenktypen sind doppelt definiert und müssen übereinstimmen: `JointTypes` in
  `JointObject.py:65` und `enum class JointType` in `App/AssemblyUtils.h:52`.
- Das Icon `Gui/Resources/icons/Assembly_CreateJointTangent.svg` liegt schon im `Assembly.qrc`,
  wird aber nirgends benutzt.

## Ansatz: Punkt-auf-Kurve als mitwandernde Punkt-auf-Gerade-Bedingung

Für den Punkt p (Marker I an Teil A) und die Kurve c(u) (fest an Teil B, in Koordinaten von
Marker J):

1. In **jeder Newton-Iteration** den Fußpunkt u* bestimmen: nächster Punkt der Kurve zu p
   (Newton auf (c(u) − p) · c′(u) = 0, Startwert aus der letzten Iteration bzw. aus einer
   groben Abtastung beim ersten Mal).
2. Am Fußpunkt Tangente t = c′(u*)/|c′(u*)| und zwei Normalen n1, n2 ⟂ t bilden.
3. Zwei Bedingungen: g1 = (p − c(u*)) · n1 = 0 und g2 = (p − c(u*)) · n2 = 0.

Weil der Fußpunkt sich in erster Näherung nur entlang t verschiebt, ist die Ableitung von
g1/g2 nach p genau n1/n2. Die Jacobi-Struktur ist also dieselbe wie bei `PointInLineJoint`,
nur mit Richtungen, die pro Iteration neu gesetzt werden. **Keine zusätzliche Unbekannte**
(der Kurvenparameter u wird nicht vom Solver gelöst). Das fügt sich ohne Umbau in OndselSolvers
Gleichungssystem ein und wirkt gleich in Solve, Drag und PreDrag.

Verworfene Alternative: Fußpunkt nur einmal pro Solve in FreeCAD bestimmen und ein normales
`ASMTPointInLineJoint` an die Tangente setzen, dann wiederholen. Kein Solver-Patch, aber
mehrere Solves pro Klick, und im Drag müsste nach jedem Schritt ein Marker im laufenden Modell
umgesetzt werden. Langsamer und ungenau bei starker Krümmung.

## Umsetzung

### Schritt 1 — Kurve im Solver (OndselSolver-Patch)
- Neue Klasse `NurbsCurve` (Grad, Knoten, Gewichte, Pole in J-Koordinaten): Auswerten von c(u),
  c′(u), c″(u) (de Boor), Fußpunkt-Suche mit Abtastung + Newton, periodische und offene Kurven.
  Keine OCC-Abhängigkeit im Solver.
- Neue Constraint-Klasse `PointOnCurveConstraintIJ` (nach Vorbild `TranslationConstraintIJ`):
  zwei Gleichungen g1/g2, Richtungen n1/n2 werden in `prePosIC()`/`postPosICIteration()` und im
  Drag-Pfad pro Iteration neu berechnet. Der Fußpunkt-Parameter wird zwischen Iterationen
  gemerkt (stabiler, schneller).
- Neues Gelenk `PointOnCurveJoint` + ASMT-Hülle `ASMTPointOnCurveJoint` (mit `With()`,
  `mbdClassNew()`), Kurvendaten als Member.
- ASMT-Ein-/Ausgabe (`storeOnLevel()`/`parseASMT()`), damit „Export ASMT“ und der Simulations-
  Export weiter funktionieren.
- **Simulation (Kinematik):** braucht Geschwindigkeit/Beschleunigung (zweite Ableitungen der
  Bedingung, Krümmung c″). Für den ersten Schritt nur Lage (Solve, Drag). In der Simulation
  wird ein Tangent-Gelenk zunächst mit Meldung abgelehnt, siehe offene Frage 3.
- Als eigener Patch `patches/2026.10.xx-ondselsolver-point-on-curve.patch`, Eintrag in
  `PATCHES.txt`. Kandidat für Upstream (OndselSolver).

### Schritt 2 — Gelenktyp in FreeCAD (C4: nur am Ende anhängen)
- `JointObject.py`: `"Tangent"` **ans Ende** von `JointTypes` und `TranslatedJointTypes`, damit
  die Enum-Indizes bestehender Dateien gleich bleiben.
- `AssemblyUtils.h`: `Tangent` ans Ende von `enum class JointType`.
- `AssemblyIdentityGraph.cpp` `jointTypeName()`: Fall `Tangent`.
- `isJointTypeConnecting()` braucht keine Änderung (Tangent verbindet).

### Schritt 3 — Solver-Gelenk bauen (`AssemblyObject.cpp`)
- `makeMbdJointOfType()`: Fall `Tangent` → neue Methode `makeMbdJointTangent()`.
- Vertex und Kante zuordnen (Kante immer Referenz 1 = Teil B, wie `getDistanceType()` es für
  PointCurve schon per `swapJCS()` macht).
- Kante aus OCC holen (`BRepAdaptor_Curve`), mit `GeomConvert::CurveToBSplineCurve` in eine
  B-Spline wandeln (deckt Spline, Bogen, Ellipse, auch Gerade ab), Pole in die Koordinaten von
  Marker J umrechnen (gleiche Placement-Kette wie `handleOneSideOfJoint()`, inkl. Spiegel im
  Kontext, C2) und ans `ASMTPointOnCurveJoint` geben.
- Andere Kombinationen als Vertex + Kante: kein Gelenk, Warnung im Report
  („Tangent: bisher nur Vertex auf Kurve unterstützt“).

### Schritt 4 — Oberfläche
- `CommandCreateJoint.py`: `CommandCreateJointTangent` (Icon `Assembly_CreateJointTangent.svg`,
  Text „Tangent Joint“), registrieren, in `InitGui.py` in Toolbar und Menü neben Distance.
- `JointObject.py`:
  - `getIcon()`: Tangent-Icon.
  - Kein Distance-Feld, kein Offset, kein Reverse.
  - Auswahl im Dialog: genau ein Vertex und eine Kante; andere Auswahl mit Hinweis ablehnen.
  - `findPlacement()`: Marker der Kante so legen, dass er unabhängig vom Klickpunkt ist
    (z.B. Ursprung des Teils), weil die Kurve vollständig im Solver liegt.
- Simulation: Tangent nicht als Antrieb anbieten.

### Schritt 5 — Tests (standalone-check/kinematic-tests)
Analytisch / Vanilla / gepatcht wie in der README. Vanilla kennt Tangent nicht; der
Vergleich läuft dort analytisch (Abstand Punkt–Kurve).
- `test_tangent_vertex_spline_flat`: Punkt auf ebener B-Spline. Nach Solve und nach mehreren
  Drag-Schritten ist der Abstand Punkt–Kurve < 1e-6 (geprüft mit OCC `distToShape`), Teil A
  ist entlang der Kurve gewandert.
- `test_tangent_vertex_3d_spline_flat`: nicht ebene (räumliche) Spline.
- `test_tangent_vertex_arc_flat`: Kreisbogen; Vergleich mit analytischer Kreisbahn.
- `test_tangent_vertex_spline_end_flat`: offenes Kurvenende. Zieht man über das Ende hinaus,
  bleibt der Punkt am Endpunkt bzw. auf der Verlängerung (Verhalten festlegen, offene Frage 2).
- `test_tangent_nested_flex`: Gelenk in flexibler Unter-BG, Original bleibt unverändert (C2).
- `test_tangent_unsupported_flat`: Fläche + Fläche → kein Gelenk, Warnung, kein Absturz.
- Unit-Tests im Solver für `NurbsCurve` (Auswertung, Fußpunkt, periodisch).

### Schritt 6 — Doku und Patch
- `docs/ARCHITECTURE.md`: Gelenktypen ergänzen. `docs/JOURNAL.md`: Eintrag.
- Rundum-Patch in `patches/` neu exportieren (Disziplin aus `PATCHES.txt`).

## Spätere Schritte (vorgemerkt, nicht Teil dieses Plans)

- Tangent für Ebene/Zylinder/Kugel (außen und innen) auf Basis der vorhandenen MbD-Gelenke
  (`ASMTLineInPlaneJoint`, `ASMTRevCylJoint`, `ASMTCylSphJoint`, `ASMTSphSphJoint`), Seite aus
  der aktuellen Lage, „Reverse“ zum Umschalten.
- Distance mit Vertex + Kurve (`PointCurve`) auf das neue Solver-Gelenk umstellen (mit
  Abstand ≠ 0 als Rohr um die Kurve). Ändert das Verhalten bestehender Dateien → Rücksprache.
- Kurve–Fläche, Fläche–Fläche (Nocken).

## Offene Fragen an dich

1. **Ausrichtung:** Soll Teil A sich beim Gleiten auch an der Kurve ausrichten (z.B. x-Achse
   des Vertex-Markers entlang der Tangente, wie ein Wagen auf der Schiene)? Oder bleibt die
   Drehung frei (nur Punkt auf Kurve, mein Vorschlag für Schritt 1)?
2. **Offenes Kurvenende:** Was passiert, wenn man über das Ende zieht? Vorschlag: Der Punkt
   bleibt am Endpunkt stehen (Fußpunkt wird auf [u0, u1] begrenzt).
3. **Simulation:** Reicht es für den ersten Schritt, wenn Tangent nur bei Solve und Drag
   wirkt und die Simulation mit einer Meldung ablehnt? Geschwindigkeit/Beschleunigung
   kämen dann als zweiter Schritt.
4. **Mehrere Kanten:** Soll man eine Kette verbundener Kanten (z.B. Spline + Bogen +
   Gerade als eine Bahn) auswählen können, oder vorerst genau eine Kante?
