# Plan: neues Gelenk „Tangent“, erster Schritt: Vertex auf Spline

Stand: 2026-10-16. Umgesetzt (Solver getestet, FreeCAD-Seite noch nicht gebaut, siehe „Stand der
Umsetzung“). Bezug: docs/CONSTRAINTS.md (C2, C4).

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

## Entscheidungen (Nutzer, 2026-10-16)

1. **Ausrichtung:** Drehung bleibt frei, nur der Punkt liegt auf der Kurve.
2. **Offenes Kurvenende:** der Punkt bleibt am Endpunkt stehen.
3. **Simulation:** vorerst nur Solve und Ziehen; die Simulation lehnt Tangent mit Meldung ab.
4. **Kanten:** vorerst genau eine Kante.

## Stand der Umsetzung (2026-10-16)

Abweichungen vom Plan oben:
- Statt einer eigenen Constraint-Klasse mit neuen Gleichungen: ein **mitwandernder End-Frame**
  (`EndFrameqcCurve`, z = Tangente am Fußpunkt) plus die vorhandenen Translations-Bedingungen
  (x/y = 0). Mathematisch dasselbe (g1/g2 oben), aber ohne neue Ableitungen; die Aktualisierung
  pro Iteration sitzt in `DispCompIeqcJeqcKeqcCurve`.
- **Kurvenende** über zwei Translations-Limits im MbD-System (Ebene durch den Endpunkt senkrecht
  zur Endtangente), aktiv im Grenz-Durchgang wie Slider-Grenzen. Ohne Limit gleitet der Punkt
  sonst auf der Verlängerung der Endtangente.
- Der Kurvenmarker bleibt das normale JCS (Placement1); die Kurve wird in dessen Koordinaten
  übergeben, `findPlacement()` musste deshalb nicht geändert werden.
- Solver-Tests liegen unter `standalone-check/solver-tests/` (eigenständig, ohne FreeCAD).

Erledigt:
- [x] Schritt 1 Solver-Patch (`patches/2026.10.16-ondselsolver-point-on-curve.patch`), alle
  Solver-Tests grün.
- [x] Schritt 2–4 FreeCAD (`patches/2026.10.16-freecad-assembly-tangent-joint.patch`).
- [x] Schritt 6 Doku (ARCHITECTURE §1.2, JOURNAL), `PATCHES.txt`.
- [ ] FreeCAD lokal bauen und `test_tangent_vertex_spline_flat` laufen lassen (in der
  Cloud-Session ohne FreeCAD/OCC nicht möglich). Die übrigen Testfälle aus Schritt 5
  (3D-Spline, Bogen, verschachtelt) sind bisher nur im Solver-Test abgedeckt.
- [ ] Prüfen im GUI: Gelenk anlegen (Vertex + Kante), Ziehen, Ende, Speichern/Laden.

## Zweiter Schritt (2026-10-18): Vertex auf Fläche, Kante an Fläche, Kante an Kante

Nutzerauftrag 2026-10-18: „weitere Elemente hinzufügen: Kurve auf Kurve und Spline-Fläche auf
Vertex und Kurve“, selbstständig umgesetzt.

**Paarungen** (die höhere Geometrie wird Referenz 1 = Marker I, Fläche > Kante > Vertex, per
`swapJCS()`):

| Auswahl | Solver | Wirkung |
|---|---|---|
| Vertex + Kante | `ASMTPointOnCurveJoint` (Schritt 1) | Punkt gleitet auf der Kurve, Anschlag am offenen Ende |
| Vertex + Fläche | `ASMTTangentJoint` „PointOnSurface“ | Punkt gleitet auf der Fläche (1 Gleichung) |
| Kante + Fläche | `ASMTTangentJoint` „CurveOnSurface“ | Kurve berührt die Fläche einseitig, kann abrollen und gleiten (1 Gleichung) |
| Kante + Kante | `ASMTTangentJoint` „CurveOnCurve“ | Kurve berührt die andere Kurve einseitig (2 Gleichungen) |

Zwei Vertices oder zwei Flächen lehnt der Dialog ab; `makeMbdJointTangent()` warnt und baut kein
Gelenk.

**Einseitiger Kontakt statt Tangentialbedingung:** Der Kontaktpunkt auf Kurve J ist der Punkt mit
dem kleinsten *orientierten* Abstand zu I (Flächennormale bzw. Normale in der Ebene einer ebenen
Kurve I). Ist dieser Abstand null, berührt J die Geometrie I von einer Seite: Tangentialität und
„keine Durchdringung“ folgen aus der Wahl des Punktes, es braucht nur Translationsgleichungen.
Die Seite ist die, auf der beim ersten Auswerten der größte Teil von J liegt (Median). Verworfen:
zusätzliche Richtungskosinus-Bedingung „Tangenten parallel“. Mit eingefrorenem Fußpunkt
linearisiert, hätte sie das Abrollen blockiert. Zwei nicht ebene Raumkurven: nächstes Punktpaar
(sie treffen sich, Tangentialität nicht erzwungen).

**Fläche aus FreeCAD:** `getFaceBSplineInJcs()` wandelt die Fläche per
`GeomConvert::SurfaceToBSplineSurface` in eine B-Spline-Fläche, vorher auf die UV-Grenzen der
Fläche beschnitten (Ebenen und Zylinder sind sonst unendlich). Löcher und krumme Ränder
(getrimmte Flächen) werden nicht berücksichtigt; am Rand der UV-Fläche bleibt der Fußpunkt am
Rand stehen.

**Konvergenz (was nötig war, siehe JOURNAL 2026-10-18):**
1. *Hüllkurven-Korrektur der zweiten Ableitungen.* Der Kontaktpunkt ist ein Minimum über den
   Kurvenparameter t. Die eingefrorenen zweiten Ableitungen gehören zu einem festen Materialpunkt
   und machten das Drehen des Rades um die eigene Achse künstlich steif (Lösung kroch in
   0,003er-Schritten). Korrektur: `+ λ·(J_rᵀ H_D J_r − g gᵀ / f_tt)` mit H_D = Hesse-Matrix des
   Abstandsfeldes von I (zweite Fundamentalform, im Abstand d: κ/(1 − dκ)), g = Ableitung von
   df/dt nach den Euler-Parametern und f_tt exakt (nicht per Differenzenquotient, die Terme heben
   sich fast auf und werden mit großem λ multipliziert). Gewichtet mit exp(−(d/L)²), L = 5 % der
   Größe von J: exakt am Kontakt, weit weg wie vorher.
2. *Kontaktpunkt exakt:* Goldener Schnitt plus Newton-Schritte auf df/dt = 0.
3. *Aufsetzen vor dem Lagelösen* (`setDownOnContact()` in `TangentJoints.cpp`): Hängt ein Teil
   nur an diesem Kontakt, wird es vor der ersten Iteration entlang der Normalen um −d verschoben
   („Rad auf die Fläche setzen“). PosIC hält die Teile möglichst nah an der Startlage und
   gewichtet 1 mm Weg etwa wie 1 rad Drehung (1e6). Aus 20 mm Abstand kippte und drehte es die
   Kurve deshalb lieber, λ wuchs auf ~1e7, und die Iteration irrte umher (je nach Pivotwahl
   anders, daher FreeCAD ≠ Nachrechnung). Teile mit weiteren Gelenken bleiben, wo sie sind.

**Tests:**
- Solver: `standalone-check/solver-tests/test_tangent_contacts.cpp`. Flächenauswertung und
  Projektion, Punkt auf ebener/gewölbter Fläche (Solve + Ziehen), Rad über/unter Ebene und über
  der Beule (auch gekippt mit versetztem Marker), Rad an zusätzlicher senkrechter
  Zylinderführung (kein Aufsetzen, die Korrekturen allein), Kreis rollt um Kreis (Mittenabstand 25),
  Kreis auf Welle, ASMT-Speichern/Einlesen aller drei Arten. Gemessen wird unabhängig vom
  Gelenk (Abtastung + Verfeinerung).
- FreeCAD: `standalone-check/kinematic-tests/test_tangent_surface_curve_flat` (7 Fälle:
  Vertex/Beule, Rad über/unter Beule, Rad auf Quader-Deckfläche, Rad an Wellenkurve,
  Referenzen vertauscht, Speichern/Laden), gemessen per OCC `distToShape` und Seitenprüfung.

**Offen:**
- Simulation lehnt Tangent weiterhin ab (Krümmungsterme für Geschwindigkeit/Beschleunigung
  fehlen).
- Getrimmte Flächen (Löcher, Ränder) und Flächen aus mehreren Patches: nur die eine Fläche,
  UV-Rechteck.
- Fläche an Fläche (Nocken mit Flächenkontakt) und Kontakt an mehreren Stellen gleichzeitig.
- `PointOnSurface` nutzt die Korrekturen aus 1. noch nicht (bisher nicht nötig).
- GUI-Abnahme durch den Nutzer.
