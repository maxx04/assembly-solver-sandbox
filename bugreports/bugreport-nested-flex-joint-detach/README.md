# FreeCAD-Kernbug: Joint in einer zweifach eingebetteten flexiblen Sub-Baugruppe wird beim
# interaktiven Ziehen komplett ignoriert

**Status: sauber isoliert, minimales Repro-Paket vorhanden, in unveränderter FreeCAD-Weekly-
AppImage bestätigt (echter Upstream-Bug, nicht durch eigene Patches verursacht) - noch nicht
upstream eingereicht.**

## Bestätigung in unveränderter Vanilla-AppImage (2026-08-25)

Dasselbe Repro-Paket (`minimal-repro.zip`) in `FreeCAD_weekly-2026.08.05-Linux-x86_64.AppImage`
geöffnet (keine eigenen Patches, keine eigene Kompilierung) - **identisches Verhalten**: BoxB
lässt sich zwei Ebenen tief frei ziehen und bleibt auch nach komplettem Recompute an der
zufälligen Stelle stehen. Damit ausgeschlossen, dass einer unserer eigenen (teils bereits wieder
deaktivierten) Assembly-Patches diesen Bug verursacht - reiner FreeCAD-Kernbug.

## Symptom

Wird eine flexible `Assembly::AssemblyLink` selbst NOCHMAL als flexible AssemblyLink in ein
drittes Dokument eingebettet (zwei Verschachtelungsebenen statt einer), verlieren Joints, die
INNERHALB der tiefsten Sub-Baugruppe definiert sind, beim interaktiven Ziehen im 3D-Fenster
jegliche Wirkung - das betroffene Teil lässt sich in JEDE Richtung frei bewegen (nicht nur
entlang der vom Joint erlaubten Freiheitsgrade), und bleibt auch nach einem **kompletten
Dokument-Recompute dauerhaft** an der falschen Stelle stehen.

Das ist die Ursache hinter den ganzen "Baugruppe zerschossen"-Vorfällen dieser Woche mit
Motor67/68 in `CNC3018_037_A_TraegerBaugruppe_Z.FCStd` (siehe
[[project_fcproject_motor68_traegerbg_integration]]).

## Repro-Paket (`minimal-repro.zip`)

Drei Dokumente, ineinander verschachtelt:

```
MinimalReproGrandTop.FCStd
  └─ Assembly001 (flexible AssemblyLink -> MinimalReproTop#Assembly)
       ├─ BoxC, BoxD (Top-Level-Teile von MinimalReproTop, gespiegelt)
       │    Joints: Gleitverbindung (BoxC-BoxD), StarrerVerbund (BoxA-BoxD)
       └─ unterAssembly (flexible AssemblyLink -> MinimalReproSub#Assembly, gespiegelt)
            ├─ BoxA, BoxB
            └─ Joints: Gleitverbindung/Joint003 (BoxA-BoxB, Slider)
```

- `MinimalReproSub.FCStd`: BoxA (geerdet) + BoxB, verbunden über einen Slider-Joint.
- `MinimalReproTop.FCStd`: bettet Sub als flexible AssemblyLink ein (Spiegel von BoxA geerdet),
  plus zwei EIGENE Top-Level-Teile BoxC (geerdet)/BoxD, verbunden per Slider (BoxC-BoxD) und
  Fixed-Joint (BoxA-BoxD, ueber die Grenze Top-Level<->eingebettete Sub-Baugruppe hinweg).
- `MinimalReproGrandTop.FCStd`: bettet Top NOCHMAL als flexible AssemblyLink ein (Spiegel von
  BoxC geerdet) - **zwei Ebenen tief**.

## Kontrollmatrix (2026-08-25, live durchgetestet)

| Test | Verschachtelungstiefe | Ergebnis |
|---|---|---|
| Slider BoxA-BoxB komplett INNERHALB einer eingebetteten Sub-Baugruppe (1 Ebene) | 1 | ✅ Sauber - Solve konvergiert, Slider haelt, kein Redundanz-Fehler |
| Fixed-Joint BoxA(gespiegelt)-BoxD ueber Top-Level<->eingebettete-Grenze hinweg (1 Ebene) | 1 | ✅ Sauber - Solve konvergiert (Convergence ~1e-8), Fixed haelt starr |
| Slider BoxA-BoxB, aber Sub jetzt selbst nochmal in GrandTop eingebettet | **2** | ❌ **BoxB laesst sich in JEDE Richtung frei ziehen, bleibt nach komplettem Recompute dauerhaft an der (beliebigen) gezogenen Stelle stehen** |
| BoxA (Teil der Grounding-Kette, an jeder Ebene neu geerdet) ziehen, dann Recompute | 2 | ✅ Springt korrekt zurueck - die Grounding-Kette selbst funktioniert auch 2 Ebenen tief |

**Wichtig, ausdruecklich ausgeschlossen:** kein Slider-Distance-Artefakt (siehe das verwandte,
separate Problem in `bugreport-ensureIdentityPlacements/`) - BoxB bleibt nicht etwa nur
"irgendwo entlang der erlaubten Achse" stehen, sondern buchstaeblich exakt an der gezogenen
Stelle, AUCH abseits der Achse (verdreht/seitlich versetzt). Der Joint wird also nicht bloss
unterbestimmt gelassen, sondern komplett ignoriert.

## Vermuteter Mechanismus (aus dem Live-Log rekonstruiert)

Waehrend des interaktiven Ziehens (`preDrag()`) loest **ausschliesslich** die AEUSSERSTE
Assembly (`MinimalReproGrandTop`) wiederholt neu - im Log erscheint bei jedem Zieh-Frame nur
`Solving 'MinimalReproGrandTop#Assembly'`, kein einziges `Solving 'MinimalReproTop#Assembly'`
oder `Solving 'MinimalReproSub#Assembly'` dazwischen. Der Slider-Joint, der BoxB haelt, lebt
aber INNERHALB von `MinimalReproSub`s eigenem `AssemblyObject` - dessen `solve()` wird waehrend
des Ziehens schlicht nie aufgerufen, BoxB hat in diesem Moment also niemanden, der es haelt.

Bei einem KOMPLETTEN Dokument-Recompute (nicht nur Loslassen) lösen dagegen alle drei Ebenen
brav nacheinander (Sub -> Top -> GrandTop, bestaetigt im Log: alle drei "finished successfully",
inklusive korrekt erkanntem Slider-Joint in Sub) - trotzdem bleibt BoxB falsch stehen. Das
deutet darauf hin, dass der abschliessende volle Solve zwar "sauber durchlaeuft", die zuvor
waehrend des Ziehens im Umgehung des inneren Solvers geschriebene (ungueltige) Placement aber
nicht als Fehler erkennt/korrigiert - vermutlich weil ein AssemblyLink beim Spiegeln seiner
Kinder deren Placement direkt uebernimmt, bevor/ohne dass das eigene, innere `solve()` sie noch
einmal validiert.

## Root Cause gefunden (2026-08-25, experimentell verifiziert, Fix wieder zurueckgerollt)

Ueber ein testweises Umschalten von `assembly->getJoints(false, false)` auf
`getJoints(false, true)` in `AssemblyLink::synchronizeJoints()` (`AssemblyLink.cpp`)
bestaetigt: der zweite Parameter (`subJoints`) steuert, ob beim Hochspiegeln von Joints in
eine Elternbaugruppe auch die Joints VERSCHACHTELTER Enkel-Baugruppen mitgenommen werden -
Vanilla-Code uebergibt hier fest `false`. Deshalb erreicht ein zwei Ebenen tief liegender Joint
(BoxA-BoxB) die Joint-Liste der aeussersten Baugruppe (GrandTop) nie - nicht gefiltert,
strukturell nie kopiert.

Mit `subJoints=true` erscheint der Joint tatsaechlich eine Ebene hoeher (als lokal gespiegeltes
`Joint001`) - wird dann aber vom Solver selbst wieder verworfen:
```
Assembly: Ignoring joint (...Joint001) because its parts are connected by a fixed
joint bundle. This joint is a conflicting or redundant constraint.
```
...aus `AssemblyObject::isMbDJointValid()`.

**Warum:** `isMbDJointValid()` vergleicht `getMbDPart(part1) == getMbDPart(part2)`, wobei
`part1`/`part2` ueber `getMovingPartFromRef()` (in `AssemblyUtils.cpp`) ermittelt werden - diese
Funktion liefert NUR das direkt referenzierte Top-Level-Objekt einer `PropertyXLinkSub`, OHNE
deren Sub-Element-Pfad zu beruecksichtigen. Beim Spiegeln des tiefen Joints musste
`handleJointReference()` fuer BEIDE Enden (BoxA und BoxB, beides Enkelkinder) auf
`findLocalAncestor()` zurueckfallen - und beide klettern zum SELBEN naechsten bekannten
Vorfahren hoch: dem Wrapper-Objekt der verschachtelten Unter-Baugruppe. Reference1/Reference2
zeigen danach zwar mit unterschiedlichen Sub-Pfaden auf dieses Wrapper-Objekt, aber
`getMovingPartFromRef()` ignoriert den Sub-Pfad komplett - der Joint sieht dadurch aus wie ein
Teil, das sich selbst referenziert (`part1 == part2`), obwohl beide Enden tatsaechlich
unterschiedliche, echte Teile sind.

**Ein vollstaendiger Fix braeuchte vermutlich zwei zusammenspielende Aenderungen:** (a)
rekursives Joint-Spiegeln (`subJoints=true` o.ae.) UND (b) einen sub-pfad-bewussten
Selbstreferenz-Check in `isMbDJointValid()`/`getMovingPartFromRef()` statt des reinen
Top-Level-Objekt-Vergleichs. Der experimentelle Fix (nur Teil (a)) wurde NICHT dauerhaft
uebernommen - gleiche gefaehrliche Codezone wie der bereits einmal abgestuerzte
`synchronizeGroundedAndRigidJoints()`-Versuch (siehe patches/README.md), und ohne Teil (b)
ohnehin nicht ausreichend. Vollstaendig als Kommentar im GitHub-Issue dokumentiert:
https://github.com/FreeCAD/FreeCAD/issues/32171#issuecomment-5414455149

**Update: Teil (b) alleine waere KEIN sicherer Fix.** `isMbDJointValid()`s Pruefung ist nur ein
Symptom-Wächter, nicht die eigentliche Grenze - die dahinterliegende `objectPartMap`/
`getMbDData()`-Zuordnung (welcher starre MbD-Koerper zu welchem Teil gehoert) ist rein ueber
`App::DocumentObject*` indiziert, komplett ohne Sub-Pfad-Bewusstsein. Selbst wenn die Pruefung
selbst sub-pfad-bewusst gemacht wuerde und den Joint durchliesse, wuerden BoxA und BoxB
darunter weiterhin demselben starren Koerper zugeordnet (da beide auf dasselbe Top-Level-Objekt
aufloesen) - man wuerde also nur die Warnung stummschalten, die genau davor schuetzen soll,
dem Solver einen echt entarteten/selbstreferenzierenden Joint zu geben (siehe Code-Kommentar:
"The solver crash when fed such a bad joint"). Ein echter Fix braeuchte `objectPartMap` selbst
sub-pfad-bewusst (Schluessel `(Objekt, Sub-Pfad)` statt nur `Objekt`) - eine groessere,
invasivere Aenderung an der MbD-Koerper-Verwaltung, keine lokale Anpassung des einen Vergleichs.
Deshalb NICHT versucht. Ebenfalls als Kommentar dokumentiert:
https://github.com/FreeCAD/FreeCAD/issues/32171#issuecomment-5414646210

## Naechste Schritte

- [x] Minimal-Repro als GitHub-Issue bei FreeCAD/FreeCAD eingereicht (2026-08-25):
  https://github.com/FreeCAD/FreeCAD/issues/32171
- [ ] Root Cause im C++-Code lokalisieren (`AssemblyLink::updateContents()` /
  `ViewProviderAssembly::preDrag()` sind die naheliegendsten Ansatzpunkte, siehe
  `patches/freecad-assembly-grounded-joint-nested-flex.patch` fuer bereits vorhandene, verwandte
  Untersuchungen an `AssemblyLink.cpp`).
- [ ] Bis dahin: PlacementGuard-Workaround (siehe `python/PlacementGuardFeature.py`) bleibt die
  einzige zuverlaessige Methode fuer mehrfach verschachtelte flexible Baugruppen wie
  TraegerBaugruppe_Z/Motor67/68.
