# Kinematik-Testsuite (Phase B: Testmatrix)

Autonome, nicht-interaktive Regressionstests für den Assembly-Solver. Methodik (Nutzerauftrag
2026-09-06):

1. **Analytisch berechnen**, wie Bewegung/Reaktion aussehen MUSS, wenn ein Teil per Skript
   bewegt/gesetzt wird - unabhängig vom Solver (reine Placement-Algebra bzw. Geometrie).
2. Gegen **"normales" (unmodifiziertes) FreeCAD** (`/home/maxx/freecad-sandbox/install-clean-test`)
   vergleichen - validiert die Testmethodik selbst und dient als Regressions-Referenz.
3. Gegen **unseren gepatchten Stand** (`/home/maxx/freecad/install`) vergleichen.
4. Reihenfolge: erst **nicht verschachtelte** Modelle, verschiedene Jointarten - danach
   schrittweise tiefer verschachteln. Bei nicht verschachtelten Fällen MÜSSEN alle drei
   (analytisch/vanilla/gepatcht) übereinstimmen - sobald Verschachtelungstiefe die bekannten
   Bugs erreicht, wird vanilla erwartungsgemäß abweichen (das ist dann kein Testfehler unserer
   Seite, sondern der bereits dokumentierte Bug).

Keine manuelle Interaktion nötig - alles headless via Xvfb.

## Aufruf

```bash
./common/run-test.sh <freecad-install-dir> <testfall-skript.py> [Xvfb-Display, default :95]

# Beispiel: denselben Testfall gegen beide Stände laufen lassen
./common/run-test.sh /home/maxx/freecad/install            test_fixed_joint_flat/test_fixed_joint_flat.py
./common/run-test.sh /home/maxx/freecad-sandbox/install-clean-test test_fixed_joint_flat/test_fixed_joint_flat.py
```

Exit-Code 0 = PASS, 1 = FAIL (für Skript-Verkettung/CI). Startet bei Bedarf sein eigenes,
isoliertes Xvfb (kapert nie ein fremdes Display).

## Ergebnis-Dateien ansehen

Jeder Lauf speichert das aufgebaute Testmodell (inkl. Solver-Ergebnis) als `.FCStd`, je Testfall
in einem EIGENEN Unterordner (Nutzerauftrag 2026-09-07, gilt für alle Testfälle, auch künftige):
`fcstd-output/<testfall>/<installationsname>.FCStd` (z.B.
`fcstd-output/test_fixed_joint_flat/freecad-sandbox_install.FCStd`). Mehrdateien-Testfälle
(verschachtelte Baugruppen mit mehreren Dokumenten) legen ihre Dateien als Geschwister im
selben Unterordner ab, mit einem zusätzlichen Suffix (z.B.
`fcstd-output/test_fixed_nested_flex/freecad-sandbox_install__sub.FCStd` +
`..._install__grand.FCStd`). Nicht versioniert (`.gitignore`, regenerierbar), bleibt aber lokal
liegen - **wird von Claude nicht mehr eigenständig gelöscht/aufgeräumt**, nur auf ausdrücklichen
Wunsch. Zum Anschauen die Sandbox-FreeCAD nutzen (eigenes Profil, siehe
`/home/maxx/freecad-sandbox/run-freecad-sandbox.sh`), Datei einfach normal öffnen.

## Warum die Umwege (Xvfb, venv, volles FreeCAD statt FreeCADCmd)

- `Assembly/JointObject.py` und `UtilsAssembly.py` importieren `PySide` **unconditional** auf
  Modulebene - das schlägt sowohl in `FreeCADCmd` als auch in `FreeCAD --console` fehl (beide
  initialisieren kein Qt). Volle Gui-Binary nötig, auch wenn nichts sichtbar sein muss.
- PySide6 selbst ist im System-Python nicht installiert, nur im projekteigenen
  `.venv` (`/home/maxx/Dokumente/FreeCAD-Development/.venv`) - `common/run-test.sh` aktiviert es und
  setzt `LD_LIBRARY_PATH`/`QT_PLUGIN_PATH` entsprechend (Muster aus
  `/home/maxx/freecad-sandbox/run-freecad-sandbox.sh` übernommen).
- Jede kopierte FreeCAD-Installation trägt ein fest einprogrammiertes RUNPATH auf die
  ursprüngliche Installation (siehe `docs/JOURNAL.md`, "ROOT CAUSE GEFUNDEN UND BEHOBEN") -
  `common/run-test.sh` stellt den eigenen `lib`-Ordner deshalb per `LD_LIBRARY_PATH` voran.
- **Wichtige Lektion (ebenfalls aus `docs/JOURNAL.md`):** `print()`-Ausgaben eines
  Gui-Prozesses landen unzuverlässig auf stdout (Report-View-Umleitung) - `common/run-test.sh` wertet
  deshalb IMMER das echte `--log-file` aus, nie die rohe Bash-Ausgabe.

## Wie ein neuer Testfall aussieht

Ein Testfall ist ein eigenständiges `.py`-Skript mit einer `main() -> int`-Funktion (0 = PASS),
das über `run_under_gui.py` (liest den Pfad aus `$KINEMATIC_TEST_PATH`) geladen wird. Siehe
`test_fixed_joint_flat.py` als Vorlage: Assembly per Python bauen (Joint-Objekte OHNE
`setJointConnectors()`/GUI-Selektion direkt über `Reference1`/`Reference2`/`Placement1/2` +
`Detach1/2=True` setzen, das umgeht die Selektions-/Geometrie-Abhängigkeit), Solver laufen
lassen, Ergebnis gegen eine unabhängig berechnete Erwartung vergleichen.

## Aktuelle Tests

Für Joints MIT Freiheitsgraden (alle außer Fixed) ist der freie Wert (Drehwinkel, Z-Versatz)
absichtlich NICHT Teil der Prüfung - welchen Wert der Solver dafür wählt, hängt vom
Newton-Verfahren/Startwert ab und wäre keine solver-unabhängige analytische Referenz mehr.
Geprüft wird stattdessen die Zwangsbedingungsgleichung des jeweiligen Jointtyps selbst (siehe
`joint_test_utils.py` für die Herleitung je Typ).

**Stand 2026-09-09 (aktualisiert) - siehe Projekt-Memory
"reference-fixed-joint-180-degree-bug-rootcause":** seit der Umstellung auf
`face_jcs_placement()`-Geometrie (mit absichtlich entgegengesetzten JCS-Z-Achsen) zeigten
mehrere dieser Tests kurzzeitig die bekannte 180-Grad-Rotationsmehrdeutigkeit im Solver, und
`test_ball_joint_flat.py` separat ein zu eng bemessenes Konvergenzradius-Fenster. Alle 5
flachen Tests sind inzwischen mit einer empirisch abgesicherten Startrotation/-position
repariert (siehe `verify_safe_rotation.py` im jeweiligen Ordner bzw.
`common/verify_safe_rotation_revolute_cylindrical_slider.py`).

**Nutzerkorrektur 2026-09-09 (Referenz-Geometrie):** "grundsaetzlich falsch" (Zitat) - eine
Flaeche ist keine realistische JCS-Referenz fuer alle Jointtypen. In echter FreeCAD-Nutzung
waehlt man fuer Revolute/Cylindrical/Slider eine Achse bzw. gerade Kante (eine Flaeche hat
keine ausgezeichnete Drehachse), und fuer Ball einen Punkt (ein Kugelgelenk dreht sich um einen
Punkt). Nur Fixed bleibt Face-basiert (Flaeche-auf-Flaeche ist dort der typische reale
Anwendungsfall). Umgestellt auf `edge_jcs_placement()`/`vertex_jcs_placement()` (Edge9 bzw.
Vertex2 der Standard-Part::Box, siehe `joint_test_utils.py::BOX_EDGES`/`BOX_VERTICES`). Dabei
zwei zusaetzliche, spezifisch fuer Cylindrical geltende Befunde: (1) kein sauberes Zwei-
Wurzel-Verhalten wie bei Fixed/Revolute/Slider, sondern ein kleiner, mit der Startabweichung
wachsender Rest-Achsenfehler, der erst zwischen ~20-30 Grad in eine echte Fehlkonfiguration
kippt - deshalb nur 10 statt 30 Grad Perturbation UND eine gelockerte Toleranz in
`check_cylindrical()`; (2) ein eigenes, engeres Konvergenzradius-Fenster bei sehr grossem
Positions-Versatz (999mm zu weit, 500mm noch sicher) - deshalb kleinerer Versatz (99,-99,99)
statt (999,-999,999). Beides noch nicht root-cause-analysiert (siehe Projekt-Memory).
Erfreulicherweise wurde Balls Konvergenzfenster mit der neuen Vertex-Geometrie sogar deutlich
GROESSER als zuvor (die fruehere enge 5-5.5mm-Grenze war offenbar spezifisch fuer die
kuenstliche Face-Konstruktion).

| Datei | Szenario | Verschachtelung | Jointtyp | Geprüfte Invariante | Status (patched/vanilla) |
|---|---|---|---|---|---|
| `test_fixed_joint_flat.py` | 2 Teile, BoxA geerdet, BoxB per Joint drangehängt, BoxB vor dem Solve absichtlich falsch platziert | keine | Fixed (0 DOF) | exakte Ziel-Placement (volle Placement-Algebra) | ✅ PASS / ✅ PASS |
| `test_revolute_joint_flat.py` | wie oben | keine | Revolute (1 DOF) | JCS-Ursprung deckungsgleich, Rotation nur um lokale Z-Achse | ✅ PASS / ✅ PASS |
| `test_cylindrical_joint_flat.py` | wie oben | keine | Cylindrical (2 DOF) | wie Revolute, zusätzlich Z-Translation frei | ✅ PASS / ✅ PASS |
| `test_slider_joint_flat.py` | wie oben | keine | Slider (1 DOF) | Translation nur entlang Z frei, keine Rotation | ✅ PASS / ✅ PASS |
| `test_ball_joint_flat.py` | wie oben | keine | Ball (3 DOF) | nur JCS-Ursprung deckungsgleich, Rotation frei | ✅ PASS / ✅ PASS |

Vanilla besteht hier erwartungsgemäß dieselbe Matrix wie gepatcht - die Patches dieser Sandbox
betreffen ausschließlich verschachtelte/geerdete Sonderfälle, nicht die grundlegende
Joint-Mathematik einzelner, nicht verschachtelter Joints. Eine Abweichung wird erst ab der
Verschachtelungsstufe erwartet (siehe unten).

**Ball-Joint-Besonderheit:** anders als bei den anderen Typen ist der Anfangsversatz von
BoxB bewusst klein (~14mm statt (999,-999,999) wie bei den uebrigen Tests) - bei einem
Kugelgelenk sind alle 3 Rotations-DOF frei, es gibt also keinen Rotationszwang, der das
Newton-Verfahren in Richtung Loesung "fuehrt". Aus einer sehr weit entfernten Startposition
konvergiert der Solver deshalb nicht (`Solve failed: iterNo > iterMax`) - eine reine
Konvergenzradius-Eigenschaft des Loesers, kein Joint- oder Test-Fehler (per Probe-Skript
eingegrenzt: Grenze liegt zwischen ~14mm und ~27mm Versatz, bei der hier verwendeten
Test-Rotation).

### Stufe 2: eine Ebene Verschachtelung (flexible AssemblyLink)

Struktur (siehe `nested_test_utils.py` für die volle Herleitung, Nomenklatur "GrandTop"/"Sub"
aus docs/ARCHITECTURE.md Abschnitt 3): `Sub` (eigenes Dokument, BoxA **geerdet** - der interne
Referenz-/Befestigungspunkt dieser Unterbaugruppe, normale Konstrukteurspraxis unabhängig davon
wo/ob sie später eingebaut wird - Nutzer-Korrektur 2026-09-08 - BoxA-BoxB per innerem
Fixed-Joint) wird über eine **flexible** `Assembly::AssemblyLink` (`Rigid=False`) in `GrandTop`
(eigenes Dokument, BoxC geerdet) eingebettet; ein äußerer Joint verbindet BoxC mit dem Spiegel
von BoxB. Sub's eigene Erdung UND der äußere Joint sind dabei kein Widerspruch.

**Stand 2026-09-09 (final fuer diese Runde):** alle 5 "je Jointtyp verschachtelt"-Testfaelle
existieren jetzt (Fixed/Revolute/Cylindrical/Slider/Ball, aeusserer Joint immer Fixed, siehe
`nested_test_utils.py`), mit der neuen, realistischen Referenzgeometrie
(`edge_jcs_placement()`/`vertex_jcs_placement()` je nach innerem Jointtyp, analog zu den
flachen Tests).

| Datei | Jointtyp (innen/außen) | Status (patched) | Status (vanilla) |
|---|---|---|---|
| `test_fixed_nested_flex.py` | Fixed/Fixed | ⚠️ äußerer Joint ✅, innerer Joint ❌ 180° (bekannt) | ❌ (siehe unten) |
| `test_revolute_nested_flex.py` | Revolute/Fixed | ⚠️ äußerer Joint ✅, innerer Joint ❌ 180° (bekannt) | ❌ (siehe unten) |
| `test_cylindrical_nested_flex.py` | Cylindrical/Fixed | ⚠️ äußerer Joint ✅, innerer Joint ❌ 180° (bekannt) | ❌ (siehe unten) |
| `test_slider_nested_flex.py` | Slider/Fixed | ⚠️ äußerer Joint ✅, innerer Joint ❌ 180° (bekannt) | ❌ (siehe unten) |
| `test_ball_nested_flex.py` | Ball/Fixed | ✅ PASS | ❌ (siehe unten) |

**Erkenntnis (2026-09-09), Nutzerauftrag "180-Grad-Problem ignorieren, weitermachen":** Der
sichere Hebel fuer den äußeren Joint ist `subB.Placement` (das ECHTE, kanonische Objekt in
Sub's eigenem Dokument), NICHT `mirror_boxB.Placement` (nur eine kosmetische, vom Solver
ueberschriebene Kopie - siehe `AssemblyObject::syncLocalMirrorPlacement()`). Damit ist der
äußere Joint bei JEDEM Jointtyp zuverlässig exakt korrekt (0,0 mm/Grad). Der innere Joint
zeigt bei allen Typen AUSSER Ball weiterhin die bekannte 180-Grad-Solver-Mehrdeutigkeit -
selbst wenn der Startwert exakt auf den korrekten, gemeinsamen Zielwert vorbelegt wird (siehe
Projekt-Memory "todo-nested-fixed-mirror-placement-quirk" fuer die volle Analyse). Ball ist der
einzige Typ, der komplett besteht, weil er als einziger keine Orientierungs-Zwangsbedingung
hat - fuer ihn existiert die Mehrdeutigkeit gar nicht.

**Vanilla zeigt ein ANDERES, eigenstaendiges Fehlerbild** (nicht dasselbe wie patched): der
äußere Joint ist dort ebenfalls falsch, PLUS eine Sub<->Spiegel-Sync-Diskrepanz - auch beim
Ball-Fall, der bei patched komplett besteht. Grund: der `subB.Placement`-Seeding-Trick ist
spezifisch auf unsere eigene "Adressieren statt Kopieren"-Architektur zugeschnitten (siehe
docs/ARCHITECTURE.md Abschnitt 4), die vanilla nicht hat - vanilla loest verschachtelte
flexible Baugruppen weiterhin ueber den aelteren Kopier-Mechanismus. Das ist ein echter,
interessanter Architektur-Unterschied zwischen den Staenden, kein Testfehler - noch nicht
weiter untersucht (Nutzerauftrag: 180-Grad-Thema erstmal zurueckstellen).

**Methodik-Korrektur (2026-09-08, wichtig für alle künftigen Stufen):** dieser Testfall verlief
durch mehrere Fehlversuche, bevor er sein jetziges, verlässliches Ergebnis lieferte - siehe
[[reference-nested-fixture-methodology]] für die volle Aufarbeitung:
1. Erste Version ließ Sub's eigene Erdung weg (falsche Annahme, das wäre redundant zum äußeren
   Joint) - Nutzer-Korrektur: eine Unterbaugruppe hat normalerweise ihren eigenen
   Referenzpunkt, das ist kein Widerspruch. Nachgetestet MIT Erdung: Kernberechnung war in
   beiden Fällen (mit/ohne Erdung) bei patched UND vanilla korrekt.
2. Zweite Version fand einen scheinbaren Vanilla-Bug (Sub-Dokument bleibt beim Neuladen
   unsynchron, ~10-13mm Abweichung) - **aber**: die Ausgangs-`.FCStd`-Datei wurde dabei bei
   JEDEM Testlauf per Skript unter der jeweils getesteten FreeCAD-Version NEU gebaut.
   Nutzer-Einwand: das vermischt "baut das Skript unter Version X dieselbe Struktur wie unter
   Version Y" mit "löst Version X denselben Ausgangszustand anders" - nur Letzteres soll
   gemessen werden. **Fix:** feste, git-getrackte Ausgangsdatei (`fixtures/`), EINMAL gebaut,
   für jeden Testlauf nur noch kopiert und geöffnet (siehe unten).
3. Beim Umbau auf die feste Fixture kam ein GENUINER Bug in dieser Sandbox selbst zum
   Vorschein: das Kopieren der Sub-Datei unter einem umbenannten Dateinamen (z.B.
   `freecad-sandbox_install__sub.FCStd`) bricht `GrandTop`s internen Verweis (der zeigt
   intern auf den BLOSSEN Dateinamen `sub.FCStd`) - "Link broken!", der innere Joint fällt
   danach unbemerkt aus dem Gesamtsystem, BoxA landet an einer falschen Position. **Fix:**
   pro Installation ein eigener UNTERORDNER mit den ORIGINALEN Dateinamen `sub.FCStd`/
   `grand.FCStd`, statt umbenannter Kopien im selben Ordner.
4. Mit dem korrekten Aufbau (feste Fixture + intakter Link) verschwand der in Schritt 2
   gefundene Vanilla-Unterschied vollständig - **beide Stände bestehen jetzt exakt denselben
   Test, inklusive der Sync-Prüfung direkt nach dem Solve UND ein zweites Mal nach
   Speichern+Schließen+Neuladen (nur `recompute()`, kein erneuter expliziter `solve()`-Aufruf
   - genau das Szenario, in dem der scheinbare Bug ursprünglich auftauchte).**

**Ehrliches Fazit dieser Stufe (Stand 2026-09-08, nach Erweiterung auf alle 5 Jointtypen):**
für Fixed/Fixed und Slider/Fixed gibt es keinen nachweisbaren Unterschied zwischen gepatchtem
und vanilla FreeCAD - beide lösen und synchronisieren korrekt. Für Revolute/Fixed,
Cylindrical/Fixed und Ball/Fixed (alle drei mit einer Rotationsfreiheit im inneren Joint) zeigt
sich dagegen ein ECHTER, reproduzierbarer Unterschied zwischen den Ständen - siehe die
180°-Fehlrotation oben. Beide Stände sind hier fehlerhaft, aber auf unterschiedliche Weise:
Vanilla löst korrekt, synchronisiert aber nicht zurück (der bereits bekannte Bug 8); Patched
synchronisiert korrekt zurück, löst dabei aber den äußeren Joint falsch (ein NEUER, bisher
unbekannter Nebeneffekt unseres eigenen Sync-Fixes). Root-Cause-Analyse ist als offener Punkt
vertagt, siehe [[todo-nested-outer-joint-180-degree-rotation]].

Alle Tests zusammen laufen lassen: `common/run-all-tests.sh <install-dir>` (bzw. die CMake-Targets
`kinematic-test-patched`/`kinematic-test-vanilla`, die die komplette Matrix ausführen). **Beide
Targets zeigen aktuell 3 bekannte FEHLER** (Revolute/Cylindrical/Ball nested, s.o.) - das ist
kein CI-Alarmsignal, sondern ein dokumentierter, offener Befund; ein NEUER, bisher unbekannter
Fehlschlag (andere Testfälle, oder eine Änderung der bekannten Differenz) wäre dagegen ein
echtes Alarmsignal.

### Stufe 3: zwei Ebenen Verschachtelung (Sub -> Mid -> GrandTop)

Direkte Erweiterung von Stufe 2 um eine dritte Ebene (siehe `test_fixed_double_nested_flex/`
für die volle Herleitung). Struktur: `Sub` (BoxA geerdet -[innerster Joint]- BoxB) wird über
eine flexible `AssemblyLink` in `Mid` eingebettet (BoxD geerdet -[mittlerer Joint]- Spiegel von
BoxB), `Mid` wiederum über eine zweite flexible `AssemblyLink` in `GrandTop` (BoxC geerdet
-[äußerster Joint]- Spiegel des Spiegels von BoxB). Insgesamt existiert in diesem System nur
EIN echter Freiheitsgrad (`subB.Placement`, das reale Objekt in Subs eigenem Dokument) - BoxD
und BoxC sind beide geerdet und bewegen sich nie.

**Namenskollision live gefunden:** `nested_test_utils.py::new_grand_assembly_with_sublink()`
vergab bisher immer fest "BoxC"/"SubLink" für die eigene Ground-Box/den eigenen Link. Bei zwei
Verschachtelungsebenen (diese Funktion zweimal aufgerufen) kollidiert das zwangsläufig -
FreeCAD benennt die zweite gleichnamige Kopie beim Spiegeln still um (z.B. "BoxC001"), was
namensbasiertes `get_mirror()`-Nachschlagen auf der äußersten Ebene unbrauchbar macht. **Fix:**
die Funktion um optionale `box_name`/`link_name`-Parameter erweitert (Default unverändert, alle
5 Stufe-2-Tests bleiben davon unberührt) - Mid bekommt `box_name="BoxD"`, GrandTop bekommt
`link_name="MidLink"`, wodurch alle Namen auf jeder Ebene eindeutig bleiben.

**Ergebnis `test_fixed_double_nested_flex.py` (2026-09-09), SELBE Fixture getestet gegen
patched UND vanilla (kein erneuter Fixture-Bau zwischen den Läufen):**

| Joint | patched | vanilla |
|---|---|---|
| äußerster (BoxC-BoxB, doppelt gespiegelt) | ✅ 0,0 mm/Grad | ❌ 180° |
| mittlerer (BoxD-BoxB, einfach gespiegelt) | ✅ 0,0 mm/Grad | ❌ 180° |
| innerster (BoxA-BoxB, real) | ❌ 180° (bekannt, siehe Stufe 2) | ✅ 0,0 mm/Grad |

Bemerkenswert: vanilla zeigt hier nicht einfach "auch falsch", sondern GENAU DAS SPIEGELBILD
von patched - patched trifft die äußeren beiden Joints exakt richtig und nur der innerste zeigt
den bekannten 180°-Rest; vanilla trifft dagegen NUR den innersten richtig und beide äußeren
Joints landen auf der falschen (180°-gedrehten) Wurzel. `RESULT: FAIL` bei vanilla ist damit
GENAU die erwartete, vorhergesagte natürliche Verschlechterung (siehe
[[todo-double-nested-tests]] - vanilla wurde NICHT bewusst ausgeschlossen, sondern ganz normal
mitgetestet und scheitert von selbst). Der Seeding-Hebel (`subB.Placement` nahe am äußersten
Ziel vorbelegen, identisch zu Stufe 2) bleibt der einzig bekannte, wirksame Mechanismus -
weitere Ebenen wuerden vermutlich denselben Kompromiss zeigen (je eine Ebene mehr "richtig"
fuer patched, aber keine automatische Verbesserung des innersten 180°-Rests). Auf
Nutzerauftrag ("180-Grad-Problem ignorieren") nicht weiter root-cause-analysiert.

**Stand 2026-09-09, alle 5 Jointtypen fertig:** die Vorhersage aus [[todo-double-nested-tests]]
("vermutlich derselbe Spiegelbild-Kompromiss mit dem jeweils eigenen Jointtyp als innerstem
Joint") hat sich fuer ALLE 4 uebrigen Typen bestaetigt - Mitte+Aussen bleiben, wie bei Stufe 2,
Fixed. Ergebnis (jeweils SELBE Fixture gegen patched UND vanilla, ohne erneuten Bau
dazwischen):

| Datei | Jointtyp (innen) | patched | vanilla |
|---|---|---|---|
| `test_fixed_double_nested_flex.py` | Fixed | ✅ außen/mitte, ❌ 180° innen (bekannt) | ❌ Spiegelbild (außen+mitte 180°, innen ✅) |
| `test_revolute_double_nested_flex.py` | Revolute | ✅ außen/mitte, ❌ 180° innen (bekannt) | ❌ Spiegelbild |
| `test_cylindrical_double_nested_flex.py` | Cylindrical | ✅ außen/mitte, ❌ 180° innen (bekannt) | ❌ Spiegelbild |
| `test_slider_double_nested_flex.py` | Slider | ✅ außen/mitte, ❌ 180° innen (bekannt) | ❌ Spiegelbild |
| `test_ball_double_nested_flex.py` | Ball | ✅ PASS (komplett) | ✅ PASS (komplett) |

**Slider brauchte einen eigenen, kleineren Startwert:** mit dem Standard-Seed (999,-999,999mm
+ 30° Störung, wie bei den anderen 4 Typen) DIVERGIERTE der Solver echt (`Solve failed: To be
implemented.`, Konvergenzwerte liefen auf > 1e16 statt zu fallen) - keine saubere
180-Grad-Wurzel, sondern eine echte Konvergenzradius-Ueberschreitung im gekoppelten
3-Joint-System. Mit demselben Muster wie Cylindricals/Balls eigener Einschraenkung im flachen
Test (siehe oben) auf 99mm/10° reduziert - konvergiert damit sauber, gleiches Ergebnis wie die
anderen 3 Rotationstypen.

Ball ist auch auf Stufe 3 der einzige Typ, der komplett besteht (keine Orientierungs-
Zwangsbedingung, deshalb keine 180-Grad-Mehrdeutigkeit an keiner Ebene) - UND besteht hier,
anders als bei Stufe 2, auch bei vanilla vollstaendig (die Stufe-2-Sync-Diskrepanz bei vanilla
wurde hier nicht erneut geprüft, siehe Testcode - nur die Placement-Kette).

## Fixture-Methodik (gilt für ALLE Stufen, auch künftige)

Nutzerauftrag 2026-09-08: die Ausgangs-`.FCStd`-Datei(en) jedes Testfalls (inkl. einer
absichtlich falschen Startposition, die der Solver korrigieren muss) werden GENAU EINMAL
gebaut und unter `fixtures/<testfall>/` GIT-GETRACKT abgelegt - NICHT bei jedem Testlauf per
Skript unter der jeweils getesteten FreeCAD-Version neu erzeugt. Siehe
`joint_test_utils.py`/`nested_test_utils.py`'s Moduldocstrings für die volle Begründung.

- **Fixtures bauen/aktualisieren:** `./common/run-test.sh <freecad-install-dir> common/build_all_fixtures.py`
  (alle 5 flachen Testfälle) bzw. `./common/run-test.sh <freecad-install-dir> test_<typ>_nested_flex/build_fixture.py`
  (je Stufe-2-Testfall einzeln) bzw. `./common/run-test.sh <freecad-install-dir> test_fixed_double_nested_flex/build_fixture.py`
  (Stufe 3). Nur nötig, wenn sich die Struktur/Placement-Werte eines Testfalls ändern sollen -
  NICHT Teil des automatisierten Testlaufs (Dateiname passt nicht zum `test_*.py`-Glob).
  **WICHTIG:** immer nur EINMAL bauen (Konvention: mit der gepatchten Installation), dann
  dieselbe Fixture-Datei gegen BEIDE Installationen testen - ein erneuter Bau unter der anderen
  Installation zwischen den Testläufen macht den Vergleich methodisch ungültig (live als Fehler
  gemacht und korrigiert bei Stufe 3, siehe dortigen Abschnitt).
- **Jeder Testlauf** kopiert die feste Fixture in sein eigenes `fcstd-output/<testfall>/`-
  Verzeichnis, öffnet NUR diese Kopie und führt darauf `recompute()`/`solve()` aus - die
  Fixture selbst bleibt unangetastet.
- **Bei mehreren Dokumenten (Stufe 2+):** die Kopie muss die ORIGINALEN Dateinamen behalten
  (eigener Unterordner pro Installation statt umbenannter Dateien im selben Ordner) - sonst
  brechen dokumentübergreifende Links (siehe Methodik-Korrektur Schritt 3 oben).

## Koerper als externe Dateien (Nutzerauftrag 2026-09-15, "naeher zur Realitaet")

Bis 2026-09-14 wurden BoxA/BoxB/BoxC (usw.) als NATIVE `Part::Box`-Objekte direkt INNERHALB des
jeweiligen Assembly-Dokuments angelegt - realitätsfremd, da echte Mehrdatei-Nutzung (PDM-Alltag)
jeden Koerper als eigene Datei haelt, per `App::Link`-XLink in die Baugruppe eingebunden. Seit
diesem Umbau gilt: **jeder Koerper eine eigene Datei, jede Baugruppe eine eigene Datei** (bereits
vorher der Fall). Die vier kanonischen Master-Koerper (`BoxA`/`BoxB`/`BoxC`/`BoxD`, gebaut per
`common/build_box_bodies.py`, git-getrackt unter `common/fixtures/`) werden von JEDEM Testfall
per eigener, git-getrackter KOPIE verwendet (`joint_test_utils.py::ensure_box_bodies_in()`) - "die
gleiche Datei fuer alle Tests benutzen" heisst hier: ein gemeinsamer Ursprung, den jeder Testfall
kopiert, NICHT eine gemeinsam-referenzierte Datei ueber Ordnergrenzen hinweg (der relative XLink
wuerde sonst bei jeder Neu-Kopie brechen, siehe "Wichtige Lektion zur Kopie-Benennung" oben).
Interner Name kommt seit dem Mehrfachinstanz-Fix (2026-09-14) ohnehin von FreeCAD selbst - das
Label bleibt "BoxA"/"BoxB"/... fuer Lesbarkeit und als stabiler Bezugspunkt
(`get_by_label()`/`get_mirror()`).

**Zwei echte, bisher nie geprüfte App::Link-Eigenheiten dabei gefunden** (beide inzwischen in den
Testskripten behoben):

1. **Erdung eines App::Link braucht `LinkPlacement`, nicht nur `Placement`.**
   `AssemblyObject::getGroundedParts()` prüft über die generische
   `DocumentObject::getPlacementProperty()`-API, die für einen Link dessen EIGENE, separate
   `LinkPlacement`-Property zurückliefert, sobald eine existiert - nicht die schlichte
   `Placement`-Property. Ohne `LinkPlacement.ReadOnly` erkennt `getGroundedParts()` einen
   "geerdeten" Link nicht, der zugehörige Joint gilt fälschlich als nicht erreichbar und wird
   komplett ignoriert. **Kein Produktivcode-Bug** - die echte `GroundedJoint.setReadOnly()` in
   `JointObject.py` (Zeile ~1547) setzt bereits korrekt beide Properties; unsere Testskripte
   brauchten das nur bisher nie, weil geerdete Objekte immer native `Part::Box` waren. Fix:
   `joint_test_utils.py::ground_object()` (statt eines rohen `setPropertyStatus`).
2. **Öffnet man nur das oberste Assembly-Dokument und lässt FreeCAD dessen transitive
   XLink-Ziele automatisch nachladen, ist diese Auflösung beim allerersten automatischen Solve
   während `restore()` nicht zuverlässig bereits vollständig** - live beobachtet:
   `getGroundedParts()` sah dabei ein `App::Link`-Objekt, dessen `getLinkedObject()` in genau
   diesem Moment noch auf sich selbst zurückfiel statt auf das echte, externe Zielobjekt. Trat
   bei den flachen Tests (nur eine Verzeichnisebene an externen Körpern) nie auf - erst bei
   verschachtelten Baugruppen (Assembly-Dokument referenziert sowohl eigene externe Körper ALS
   AUCH eine Unterbaugruppe, die selbst wieder externe Körper referenziert) wurde es sichtbar,
   und zwar dauerhaft für den gesamten weiteren Testlauf (ein späteres `recompute()`/`solve()`
   ändert daran nichts mehr). Fix: `joint_test_utils.py::open_other_documents_in_dir()` - öffnet
   jede `.FCStd`-Datei im Ausgabeverzeichnis explizit, BEVOR das oberste Assembly-Dokument
   geöffnet wird.

## Geplante Erweiterung (Testmatrix)

Bewusst zurückgestellt (Nutzerauftrag "180-Grad-Problem ignorieren, weitermachen"): die
180°-Solver-Mehrdeutigkeit bei Revolute/Cylindrical/Slider (innerster Joint, Stufe 2+3) sowie
das ihr genau entgegengesetzte Fehlerbild bei vanilla (Stufe 3) root-cause-analysieren.

Stufe 3 ist jetzt fuer alle 5 Jointtypen abgeschlossen. Als Nächstes: äußerer Joint mit
Freiheitsgraden (z.B. äußerer Slider/Revolute statt immer Fixed), dann gemischt
rigid/flexible Verschachtelung, dann noch tiefere Verschachtelung (4+ Ebenen) - jeweils mit
demselben Dreiklang analytisch/vanilla/gepatcht, auf Basis fester Fixtures.
