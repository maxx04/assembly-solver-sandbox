# Plan: Punkt-auf-Kurve-Gelenk (Slider entlang einer Kurve)

Stand 2026-11-01. Nutzerwunsch vom 2026-10-11 (Slider entlang B-Spline), damals zurückgestellt;
am 2026-11-01 als Solver-Prototyp begonnen (Nutzer: „ja“ zum Vorschlag „erst nur im Solver, ohne
Oberfläche und ohne Dateiformat-Änderung“).

## Prototyp im Solver (erledigt, nicht committet)

Fußpunkt-Verfahren: In jeder Newton-Iteration wird zum aktuellen Ort des Punkts (Ursprung von
Marker J, in den Koordinaten von Marker I) der nächste Kurvenpunkt c mit Tangente t bestimmt.
Zwei Gleichungen verlangen, dass der Abstand senkrecht zur Tangente null ist:
`n_k · (A_I^T (rJ - rI)) - n_k · c = 0`, k = 0, 1, mit n0, n1 ⟂ t.

- `DispCompAlongCurveIeqcJeqc`: der Abstand entlang n_k als Linearkombination der drei vorhandenen
  Achs-Bausteine `DispCompIeqcJeqcKeqc` (K = I, Achsen x, y, z) - alle Ableitungen kommen von dort.
  n und c gelten innerhalb einer Iteration als fest: erste Ableitungen auf der Kurve exakt (dc/dq
  liegt entlang t ⟂ n), Krümmungsterme der zweiten Ableitungen fehlen (Quasi-Newton).
- `PointOnCurveConstraintIqcJqc` (eine Gleichung), `PointOnCurveJoint` (zwei, 4 Freiheitsgrade
  bleiben), `ASMTPointOnCurveJoint` (Kurve als Funktion `CurveFootFunction`, nicht in .asmt
  gespeichert).
- Test ohne FreeCAD: `standalone-check/solver-tests/point_on_curve_test.cpp`
  (`run_point_on_curve_test.sh`): Schraubenlinie, 5 Startlagen inkl. verschobener und gedrehter
  Grundplatte - Punkt danach ≤ 6e-15 mm auf der Kurve.

## Ohne neuen Gelenktyp: Abstand-Gelenk Punkt–Kurve (geprüft 2026-11-01)

Nutzer: „prüf bitte ohne neuen Typ“. Ergebnis: geht, über das **vorhandene Abstand-Gelenk**.

- `getDistanceType()` (AssemblyUtils.cpp) liefert für Punkt + Kante, die keine Gerade ist, schon
  heute `DistanceType::PointCurve`. Der Solver baut daraus nur `ASMTPointInPlaneJoint` („Punkt in
  der Ebene der Kurve“, Kommentar im Code: „Maybe it would be best ...“) - für 3D-Kurven falsch,
  bei Kreis/Bogen darf der Punkt überall in der Kreisebene liegen.
- Neu: `PointCurve` mit Abstand 0 → `ASMTPointOnCurveJoint` (Punkt genau auf der Kurve,
  4 Freiheitsgrade). Abstand ≠ 0 („Punkt im Schlauch um die Kurve“, eine Gleichung) zunächst
  unverändert, später als eigene Stufe.
- Dateiformat unverändert (kein neuer Typ, keine neue Eigenschaft, Regel C4 erfüllt). Vanilla
  öffnet die Datei und rechnet wie bisher näherungsweise „Punkt in Ebene“ - ein
  Interpretationsunterschied, keine Inkompatibilität.
- Oberfläche: Punkt + Kante wählt man im Abstand-Gelenk schon heute aus, nichts Neues nötig.
- Ein Schlitten, der einer gekrümmten Schiene folgt (1 Freiheitsgrad), entsteht ohne neuen Typ aus
  zwei Punkt–Kurve-Gelenken (zwei Punkte des Schlittens auf der Schiene) plus einer Bedingung gegen
  Drehen um die Schiene - wie bei einem echten Schlitten mit zwei Laufwagen.

## Kurven- und Linienketten

Eine Bahn besteht oft aus mehreren Kanten (Gerade – Bogen – Gerade, Skizzen-Kontur). Zwei Wege:

1. **Kette direkt (Vorschlag):** Zur gewählten Kante die zusammenhängende Kette im selben Objekt
   bestimmen (Kanten, die über gemeinsame Ecken anschließen, z.B. der Draht einer Skizze). Die
   Fußpunkt-Funktion sucht über alle Kanten der Kette den nächsten Punkt (OCC
   `BRepExtrema_DistShapeShape` bzw. je Kante `GeomAPI_ProjectPointOnCurve`) und nimmt die Tangente
   dieser Kante. Exakte Geometrie, keine Umwandlung nötig.
   - Tangential stetige Übergänge (Gerade → tangentialer Bogen): problemlos.
   - Scharfe Ecken: Tangente springt; der Fußpunkt kann an der Ecke hängen bleiben oder springen.
     Für echte Mechanismen sind solche Bahnen ohnehin verrundet - Hinweis an den Nutzer, ggf.
     Warnung bei Knick.
   - Offene Kette/Kurve: Über ein Ende hinaus ist der nächste Kurvenpunkt der Endpunkt; die zwei
     Gleichungen halten den Punkt dann auf der **Verlängerung der Tangente** fest, nicht am Ende
     (kein Anschlag). Ein echter Anschlag bräuchte eine Grenze auf dem Kurvenparameter (wie die
     Längengrenze beim Slider) - offen.
2. **Umwandeln in eine Spline (Alternative):** FreeCAD kann eine Kette zu einer Kurve machen, z.B.
   Draft „Wire to B-spline“ (interpoliert durch die Ecken, verändert die Form leicht) oder die
   Curves-Werkbank „Join curves“ (verbindet Kanten zu einer B-Spline). Dann reicht eine Kante.
   Nachteil: Ersatzgeometrie, bei Änderung der Kette neu erzeugen.

Empfehlung: 1 umsetzen (der Konstrukteur wählt einfach eine Kante der Bahn), 2 als Ausweg
dokumentieren.

## FreeCAD-Seite umgesetzt (2026-11-01)

- `AssemblyObject::makeMbdJointDistance()`, Fall `PointCurve` mit Abstand 0: `makeCurveFootFunction()`
  holt die Kante in den eigenen Koordinaten des referenzierten Objekts (dieselben, auf die sich
  Placement1/2 beziehen) und rechnet in das System des Markers um. Kette: liegt die Kante in genau
  einem Draht (Skizzen-Kontur, Part::Wire), gilt der ganze Draht; bei Körpern (jede Kante in zwei
  Flächenrändern) nur die Kante. Fußpunkt per `Extrema_ExtPC` (Toleranz 1e-10) plus Kantenenden.
- Regel: gewählt werden muss eine **gekrümmte** Kante der Kette. Ist die gewählte Kante eine Gerade,
  bleibt es „Punkt auf (unendlicher) Gerade“ wie bisher (`PointLine`) - bestehende Modelle mit
  Punkt–Gerade in Rechteck-Skizzen ändern sich so nicht. Offen: auch bei gewählter Geraden die
  Kette nehmen, wenn der Draht gekrümmte Kanten hat (Verhaltensänderung, mit Nutzer klären).
- Test `test_point_on_curve_flat`: 3D-B-Spline, schräger Kreisbogen, Kette Gerade–Bogen–Gerade
  (Start an jeder der drei Kanten), Bahn verschoben/gedreht, Placement1 schief - Punkt ≤ 2e-14 mm
  auf der Bahn. Weekly-Vanilla: 7-40 mm daneben (FAIL).
- Redundanzprüfung (Regel C7): die zwei Gleichungen sind Längen in mm, gegen die Längentoleranz.

## Nächste Schritte

1. (erledigt, siehe oben) FreeCAD-Seite (`AssemblyObject::makeMbdJointDistance`, Fall `PointCurve`, Abstand 0):
   Kante bzw. Kette aus der Referenz holen (in den Koordinaten des Objekts), in das System von
   Marker I umrechnen, Fußpunkt-Funktion mit OCC bauen, `ASMTPointOnCurveJoint` statt
   `ASMTPointInPlaneJoint`.
2. Tests: flach (Punkt auf B-Spline, Kreisbogen, Kette Gerade–Bogen–Gerade), Ziehen entlang der
   Bahn bis zum Kettenende, verschachtelt; Vanilla-Vergleich (dort „Punkt in Ebene“).
3. Später: Abstand ≠ 0 (Schlauch), zeitabhängige Variante, tangentiale Ausrichtung.
