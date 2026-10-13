# Assembly-Modul: verbindliche Regeln (Constraints)

Feste Entwurfsregeln für die Sandbox, vom Nutzer entschieden. Jede Änderung am Assembly-Code muss
sie einhalten. Weicht eine Lösung davon ab, wird vorher mit dem Nutzer gesprochen. Hintergrund und
Herleitung stehen in [ARCHITECTURE.md](ARCHITECTURE.md) und in `plans/`.

## C1. Starre und flexible Unterbaugruppen (2026-10-09)

**Starr ist ein Klotz.**
- Eine starre Unterbaugruppe ist für den Solver genau ein Körper.
- Sie zeigt den Stand, den ihr verlinktes Objekt zeigt, also ihr eigenes Dokument bzw. die
  Instanz, auf die sie verweist („zeigt ja, ändert nein“). Ihre inneren Joints werden im äußeren
  Kontext nicht gelöst.
- Das gilt für alles unterhalb einer starren Unterbaugruppe, auch für darin enthaltene flexible
  Unterbaugruppen. Unter einem starren Vorfahren löst niemand, also werden deren Teile ebenfalls
  nur nachgeführt.

**Flexibel heißt: der äußere Kontext löst.**
- Jedes Teil ist ein eigener Körper. Die inneren Joints werden im äußeren Kontext mitgelöst.
- Die Lage der Teile ist Zustand der Instanz und steht in den Spiegeln der aktiven Baugruppe.
  Originale werden nie verändert (siehe C2).

**Wer entscheidet starr oder flexibel: nur das einbauende Dokument.**
- Ob eine Unterbaugruppe starr oder flexibel ist, legt allein das Dokument fest, in dem sie
  eingebaut ist. Beispiel: BG43 in BG37 → BG37 entscheidet.
- Tiefer verschachtelte Spiegel übernehmen diesen Wert immer automatisch von ihrer Quelle.
  Beispiel: der BG43-Spiegel unter BG37 in BG22. Ein abweichender eigener Wert in einem äußeren
  Kontext ist nicht vorgesehen.
- Umstellen geschieht nur im einbauenden Dokument und wirkt dann in allen Kontexten, die es
  einbinden.

**Warum:** Ein Mischzustand, starr in BG22 und flexibel in BG37, erzeugte Joints über eine Grenze,
die nur im äußeren Kontext starr war. Derselbe Motor wurde dadurch zwei Solver-Körper, und BG43
fiel in BG22 auseinander. Mit einem einzigen Entscheider gibt es diesen Zustand nicht.

**Wofür starr:** Teile, die sich im äußeren Kontext nie gegeneinander bewegen, z.B. Winkel mit
Schrauben, Halter, Führungsbaugruppen, Kaufteile. Das bringt weniger Körper (schnellere Solves und
Drags) und hält innere Redundanzen und Mehrdeutigkeiten aus dem äußeren Solve heraus.

**Wofür flexibel:** alles, was im äußeren Kontext beweglich sein muss, z.B. Treiber (Rotor dreht)
oder Träger (fährt).

## C2. Originale werden von außen nie verändert (2026-10-03)

- Im Kontext einer Baugruppe A ist jeder Solver-Körper genau ein Objekt im Dokument von A: ein
  lokales Teil oder A's eigener Spiegel.
- Startlage, Fixierung, Rückschreiben und Ziehen betreffen nur diese Instanz.
- Ein Original wird nur gelöst und geschrieben, wenn sein eigenes Dokument die aktive Baugruppe
  ist.
- Ein neu angelegter Spiegel startet einmalig in der Lage seiner Quelle. Danach ist seine
  Placement Zustand der Instanz.
- **Änderungen im Original** (2026-10-13, Nutzerentscheidung): Wird im Original ein Teil
  verschoben, zieht das keine eingebaute flexible Instanz nach. Ihre Lage bleibt Zustand der
  Instanz. Ausnahme sind **Gelenkgrenzen**. Beim nächsten Solve wird die Instanz in die neue
  Grenze gezogen. Klappt das nicht, etwa weil äußere Gelenke das Teil festhalten, bleibt die
  Instanz stehen, und es kommt eine Warnung „Gelenkgrenze ... nicht einhaltbar“ mit Gelenk,
  Wert und erlaubtem Bereich (`AssemblyObject::reportUnsatisfiedLimits()`, Python
  `getLimitViolations()`, Test `test_nested_limit_change_in_original`).

Plan: `plans/2026-10-03-solver-nur-auf-spiegeln.md`.

## C3. Erdung ist eine Kontext-Eigenschaft (2026-10-03)

- Welche Teile in einer Baugruppe geerdet sind, ergibt sich rein aus deren Kontext.
  `getGroundedParts()` ist seiteneffektfrei.
- Eine Unterbaugruppe darf ihren eigenen Referenzpunkt behalten.
- Ist eine eingebaute Unterbaugruppe über ein Gelenk mit der äußeren Baugruppe verbunden, ist
  ihre interne Erdung in diesem Kontext deaktiviert. Ihre Lage bestimmen dann die Gelenke.
  Die Deaktivierung gilt nur beim Lösen der äußeren Baugruppe. Die Datei der Unterbaugruppe
  und ihr `GroundedJoint` bleiben unverändert (siehe C2), allein geöffnet wirkt die Erdung wie
  bisher.
- Das Lösen einer äußeren Baugruppe verändert nie die Erdung eines anderen Dokuments.

Plan: `plans/2026-10-03-erdung-als-kontext-eigenschaft.md`.

## C4. Dateikompatibilität

- Jede Änderung erhält die Kompatibilität der `.FCStd`-Dateien.
- Keine neuen persistenten Properties ohne Rücksprache.

## C5. Eingefügte Unterbaugruppe bleibt an ihrer Ablage-Position (2026-10-13)

Nutzerentscheidung: Ein eingefügtes Teil darf nie undefiniert herumschwimmen. Bis es angebunden
ist, verhält es sich wie in seiner eigenen Datei und bleibt dort, wo es abgelegt wurde.

1. **Innenleben wie im Original.** Gleiter, Drehgelenke usw. stehen nach dem Einfügen so wie in
   der eigenen Datei der Unterbaugruppe (Startlage der Spiegel aus dem Gegenstück).
2. **Fest an der Ablage-Position.** Eine flexible Unterbaugruppe, die noch mit keinem Gelenk an
   die restliche Baugruppe angebunden ist, wird von ihrer eigenen Erdung gehalten - an der Stelle,
   an der sie in diesem Kontext liegt, nicht an ihrer Lage im eigenen Dokument. Innen wird sie
   normal gelöst. Hat sie entgegen C6 keine eigene Erdung, hält sie ersatzweise ihr erstes Teil.
3. **Beim ersten Gelenk fällt der Halt weg.** Sobald ein Gelenk sie mit der restlichen Baugruppe
   verbindet, bestimmen die Gelenke ihre Lage, auch wenn sie danach noch beweglich ist. Der
   Solver verschiebt so wenig wie möglich; bewegliche Teile behalten ihre Stellung, soweit das
   neue Gelenk es zulässt.
4. **Als Ganzes verschieben.** Eine noch lose Unterbaugruppe wird beim Ziehen als Ganzes bewegt;
   das setzt ihre Ablage-Position neu.
5. **Instanzen sind unabhängig.** Danach ist die Lage Zustand dieser Instanz (siehe C2).
6. **Keine neue Eigenschaft in der Datei.** Die Ablage-Position ist die gespeicherte Lage der
   Spiegel (C4).

Umsetzung: `AssemblyObject::getGroundedParts()` - verschachtelte Erdungen werden je Einbau-Instanz
aufgelöst (`findInstanceUnder()`, nötig für Unterbaugruppen, die über mehrere Eltern im Baum
stecken), Anker für Unterbaugruppen ohne eigene Erdung; die bestehende Gruppierung nach
Verbindungen verwirft den Halt, sobald die Unterbaugruppe angebunden ist. Tests:
`test_nested_cross_parent_grounding`, `test_nested_loose_subassembly_without_ground`.

## C6. Jede Baugruppe hat eine Erdung (Pflicht, 2026-10-13)

Nutzerentscheidung: Jede Baugruppe muss mindestens ein geerdetes Teil (`GroundedJoint`) haben.
Ohne Erdung kann sie in ihrer eigenen Datei nicht berechnet werden; `solve()` bricht dann ab
(Rückgabe -6, so auch in Vanilla-FreeCAD).

- Die Erdung gehört zur Baugruppe selbst und wird in ihrer eigenen Datei angelegt.
- Eingebaut und angebunden ist sie im Kontext der äußeren Baugruppe deaktiviert (C3).
- Eingebaut und noch lose hält sie die Unterbaugruppe an ihrer Ablage-Position (C5).
- Fehlt sie trotzdem, ist das ein Modellfehler. Der Ersatz-Anker aus C5 Punkt 2 ist nur ein
  Notbehelf, damit nichts herumschwimmt.
