# Plan: neues Gelenk „Tangent“ (wie Onshape Tangent Mate)

Stand: 2026-10-16. Nur Plan, noch keine Umsetzung. Bezug: docs/CONSTRAINTS.md (C2, C4).

## Ziel

Zwei Flächen (später auch Kanten) bleiben **tangential**, also berühren sich, ohne sich zu
durchdringen. Wie in Onshape: Auswahl zweier Geometrien, kein Abstandswert, die Seite der
Berührung (außen/innen) ist umschaltbar. Das Gelenk lässt alle Freiheitsgrade frei, die die
Berührung nicht verletzen (z.B. Zylinder rollt/gleitet auf Ebene).

## Bestandsaufnahme

- Gelenktypen sind doppelt definiert und müssen übereinstimmen:
  `JointTypes` in `JointObject.py:65` und `enum class JointType` in `App/AssemblyUtils.h:52`.
- Solver-Gelenke entstehen in `AssemblyObject::makeMbdJointOfType()` (`AssemblyObject.cpp:3507`).
  Der Typ **Distance** deckt fast alle Tangentenfälle schon ab: `makeMbdJointDistance()`
  (`AssemblyObject.cpp:3587`) rechnet bei Flächen den Radius in den Abstand ein. Mit Abstand 0
  ist z.B. Ebene–Zylinder (`ASMTLineInPlaneJoint`, offset = r) oder Zylinder–Zylinder
  (`ASMTRevCylJoint`, distanceIJ = r1 + r2) bereits eine Tangente (außen).
- Die Paarbestimmung macht `getDistanceType()` (`AssemblyUtils.cpp:164`), inkl. `swapJCS()`,
  damit z.B. die Ebene immer Referenz 1 ist.
- Marker: `findPlacement()` legt bei Zylinder auf die Achse, bei Kugel/Torus in den
  Mittelpunkt (`UtilsAssembly.py:1132ff.`). Für Distance wird `ignoreVertex=True` gesetzt
  (`JointObject.py:935`), das brauchen wir für Tangent genauso.
- Das Icon `Gui/Resources/icons/Assembly_CreateJointTangent.svg` liegt schon im `Assembly.qrc`,
  wird aber nirgends benutzt.
- Was Distance **nicht** kann:
  1. **Innen-Tangente** (Welle in Bohrung, Kugel in Pfanne): Abstand muss |r1 − r2| sein statt
     r1 + r2.
  2. **Seite bei Ebene–Zylinder/Kugel**: Der Offset geht in Richtung der Ebenen-Normalen. Liegt
     das Teil auf der anderen Seite, zieht der Solver es durch die Ebene. Heute löst das nur
     „Reverse“ von Hand.
  3. Kegel, Torus, Freiformkurven (Nocken) gehen ins `default` (Planar) und sind damit falsch.

## Umfang

### Phase 1 — Tangent auf Basis der vorhandenen MbD-Gelenke (kein Solver-Patch)

| Paar | MbD-Gelenk | Wert |
|---|---|---|
| Ebene – Zylinder | `ASMTLineInPlaneJoint` | offset = ±r |
| Ebene – Kugel | `ASMTPointInPlaneJoint` | offset = ±r |
| Zylinder – Zylinder | `ASMTRevCylJoint` | außen r1 + r2, innen \|r1 − r2\| |
| Zylinder – Kugel | `ASMTCylSphJoint` | außen r1 + r2, innen \|r1 − r2\| |
| Kugel – Kugel | `ASMTSphSphJoint` | außen r1 + r2, innen \|r1 − r2\| |
| Gerade (Kante) – Zylinder | `ASMTRevCylJoint` | r (Kante parallel zur Achse) |
| Kreis (Kante) – Ebene | `ASMTLineInPlaneJoint` auf Kreisachse? | prüfen, siehe offene Fragen |

Nicht unterstützte Paare (Kegel, Torus, B-Spline) erzeugen **kein** Gelenk und eine klare
Meldung im Report („Tangent: Kombination X–Y nicht unterstützt“), statt still ins Planar-Gelenk
zu fallen.

### Phase 2 — echte Kurven-/Flächen-Tangente (später, eigener Plan)

Kegel, Torus, Nocken/B-Spline brauchen ein neues Constraint in OndselSolver
(Kontaktpunkt als zusätzliche Parameter u,v auf beiden Flächen). Das wäre ein eigener Patch
`patches/…-ondselsolver-tangent.patch` und ist deutlich größer. Hier nur vorgemerkt.

## Umsetzung Phase 1

### Schritt 1 — Typ anlegen (C4: nur am Ende anhängen)
- `JointObject.py`: `"Tangent"` **ans Ende** von `JointTypes` und `TranslatedJointTypes`.
  Am Ende, damit die Enum-Indizes bestehender Dateien gleich bleiben.
- `AssemblyUtils.h`: `Tangent` ans Ende von `enum class JointType`.
- `AssemblyIdentityGraph.cpp:1150` `jointTypeName()`: Fall `Tangent`.
- `isJointTypeConnecting()` braucht keine Änderung (Tangent verbindet).

### Schritt 2 — Paarbestimmung wiederverwenden
- `getDistanceType()` unverändert nutzen. Kleine Ergänzung für Kante–Zylinder (heute nicht
  erkannt) als neuer `DistanceType::LineCylinder`-Zweig, falls er in Phase 1 bleibt.

### Schritt 3 — Solver-Gelenk `makeMbdJointTangent()`
- Neue Methode in `AssemblyObject` neben `makeMbdJointDistance()`, aufgerufen aus
  `makeMbdJointOfType()`. Gleiche Fallunterscheidung wie Distance, aber:
  - Abstand fest 0, `Distance`-Property wird nicht gelesen.
  - **Seite außen/innen** und **Vorzeichen bei Ebenen** werden aus der **aktuellen Lage der
    Solver-Körper** bestimmt (Startlage aus `getMbdPlacement()`, also Spiegel im Kontext, C2):
    - Kreisförmig: Achs-/Mittelpunktabstand d. Ist d < max(r1, r2) → innen, sonst außen.
    - Ebene: Vorzeichen des Mittelpunkt-/Achsabstands zur Ebene entlang der Normalen.
  - So bleibt die Seite beim Ziehen erhalten (der Solver startet immer vom aktuellen Stand),
    und es braucht **keine neue persistente Property** (C4).
  - „Reverse“ (vorhandene Property) erzwingt die jeweils andere Seite (Onshape „Flip“).
- Nicht unterstützte Paare → `nullptr` + Warnung (siehe Umfang).

### Schritt 4 — Oberfläche
- `CommandCreateJoint.py`: `CommandCreateJointTangent` (Icon `Assembly_CreateJointTangent.svg`,
  Text „Tangent Joint“), registrieren, in `InitGui.py` in Toolbar und Menü neben Distance.
- `JointObject.py`:
  - `findPlacement()`: `ignoreVertex` auch für `"Tangent"`.
  - `getIcon()` (`JointObject.py:1265ff.`): Tangent-Icon.
  - `JointUsingReverse` um `"Tangent"` ergänzen (Seite umschalten); kein Distance-Feld.
  - Auswahlprüfung im Dialog: nur Flächen/Kanten, keine Punkte; nicht unterstützte Paare schon
    beim Auswählen ablehnen (Hinweis im Dialog).
- Simulation: Tangent nicht als Antrieb anbieten (wie Distance).

### Schritt 5 — Tests (standalone-check/kinematic-tests)
Analytisch / Vanilla / gepatcht, wie in der README beschrieben. Vanilla kennt Tangent nicht,
dort gilt der Distance-Joint mit Abstand 0 als Referenz (nur Außen-Fälle).
- `test_tangent_plane_cylinder_flat`: Zylinder liegt auf Ebene, beim Verschieben/Drehen bleibt
  Achsabstand = r. Beide Seiten der Ebene.
- `test_tangent_cylinder_cylinder_flat`: außen (r1 + r2) und innen (|r1 − r2|, Welle in Bohrung).
- `test_tangent_sphere_flat`: Kugel–Ebene, Kugel–Kugel, Kugel in Pfanne.
- `test_tangent_reverse_flat`: Reverse wechselt die Seite.
- `test_tangent_nested_flex`: Tangent in flexibler Unter-BG, Original bleibt unverändert (C2).
- `test_tangent_unsupported_flat`: Kegel–Ebene → kein Gelenk, Warnung, kein Absturz.

### Schritt 6 — Doku und Patch
- `docs/ARCHITECTURE.md`: Abschnitt Gelenktypen ergänzen. `docs/JOURNAL.md`: Eintrag.
- Rundum-Patch in `patches/` neu exportieren (Disziplin aus `PATCHES.txt`).

## Offene Fragen an dich

1. **Seite außen/innen:** automatisch aus der aktuellen Lage + „Reverse“ zum Umschalten (mein
   Vorschlag, keine neue Property), oder lieber eine sichtbare Auswahl „außen/innen“ im Dialog?
   Letzteres braucht eine neue persistente Property (C4 → Rücksprache).
2. **Umfang Phase 1:** reichen Ebene/Zylinder/Kugel, oder sind Kegel/Torus/Nocken (Phase 2,
   Solver-Patch) gleich mit gewünscht?
3. **Zylinder–Zylinder mit schrägen Achsen:** Onshape erlaubt Tangente auch bei windschiefen
   Achsen (Punktberührung). Prüfen, ob `ASMTRevCylJoint` dann den kürzesten Abstand der Achsen
   nimmt oder Parallelität voraussetzt. Falls Parallelität: zusätzlich ein Parallel-Gelenk
   koppeln oder nur parallele Achsen zulassen?
4. **Kreis-Kante auf Ebene** (Scheibe rollt auf Tisch): in Phase 1 aufnehmen?
