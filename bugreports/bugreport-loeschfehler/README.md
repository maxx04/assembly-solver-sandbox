# FreeCAD-Kernbug: Hänger beim Löschen eines Teils in einer mehrfach verlinkten Unterbaugruppe

**Status: Ursache gefunden, gepatcht und verifiziert (2026-08-16).** Patch:
`patches/freecad-assembly-link-delete-hang.patch`.

## Symptom

FreeCAD friert beim Löschen eines Bauteils ein, das Teil einer Unterbaugruppe
(`Assembly::AssemblyLink`) ist, die mehr als einmal in die übergeordnete Baugruppe
eingebunden ist. Kein Absturz, kein Fehlerdialog - der Hauptthread läuft auf ~100% CPU
weiter, es kommt aber keine weitere Ausgabe mehr in der Report View/im Logfile. Von außen
nicht von einem normalen, nur sehr langsamen Vorgang zu unterscheiden; per `ps`/`/proc`
über mehrere Sekunden beobachtet zeigt sich aber echte, kontinuierliche CPU-Last auf exakt
einem Thread (dem Hauptthread) - kein Deadlock (der würde 0% CPU zeigen), sondern eine
tatsächliche Endlosschleife.

Reproduziert mit `CNC3018_022_A_CNC3018_GesamtBaugruppe-loeschfehler.FCStd`
(`~/Dokumente/CAD_Workspace/PROJ_CNC3018/`) - die Baugruppe bindet
`CNC3018_025_A_FuerungsBaugruppe330` zweimal ein (als `...330` und `...331`). Löschen
eines Teils innerhalb einer dieser beiden Instanzen löst den Hänger zuverlässig aus.

## Diagnose

Kein FCProject-Code beteiligt - die komplette Aufrufkette liegt in FreeCADs eigenem
Assembly-Modul (`AssemblyApp.so`/`AssemblyGui.so`) plus `libFreeCADApp.so`/
`libFreeCADGui.so`. Per `gdb -p <pid> -batch -ex "thread apply all bt"` bei laufendem
Hänger bestätigt (`ptrace_scope` musste dafür per `sudo sysctl
kernel.yama.ptrace_scope=0` temporär gesenkt werden, war auf diesem System
standardmäßig auf `1`).

Kernstelle im Stacktrace (`gdb-backtrace-vor-fix-run1.txt`, Thread 1 = Hauptthread):

```
StdCmdDelete::activated
 -> AssemblyGui::ViewProviderAssemblyLink::onDelete
  -> App::GroupExtension::removeObjectsFromDocument
   -> App::Document::_removeObject
    -> App::PropertyLinkBase::breakLinks
     -> App::PropertyLinkList::setValues -> Property::hasSetValue
      -> Assembly::AssemblyLink::onChanged(&Group)
       -> Assembly::AssemblyLink::updateContents()
        -> Assembly::AssemblyLink::synchronizeComponents()
         -> Document::addObject(...)                       [legt fehlendes Spiegel-Objekt an]
          -> ... Property::hasSetValue (Group hat sich geändert)
           -> Assembly::AssemblyLink::onChanged(&Group)     [erneut - ggf. andere Instanz!]
            -> Assembly::AssemblyLink::updateContents()
             -> Assembly::AssemblyLink::synchronizeComponents()
              -> Document::addObject(...)                   [wieder ein fehlendes Spiegel-Objekt]
               -> ... (wiederholt sich, nie stabil)
```

`AssemblyLink::synchronizeComponents()` spiegelt die Top-Level-Komponenten der
verlinkten Quell-Baugruppe (`getLinkedAssembly()->Group`) in die eigene `Group` -
für jede Komponente ohne passendes Spiegel-Objekt wird per `doc->addObject(...)` eines
angelegt. Das Anlegen ändert die eigene `Group`-Property, was **synchron** (nicht
zurückgestellt bis nach dem aktuellen Aufruf) `onChanged(&Group)` auslöst. Dessen
`Group`-Zweig iteriert `getInList()` (alle Objekte, die auf `this` verweisen) und ruft
`updateContents()` auf **jeder** dort gefundenen `AssemblyLink`-Instanz auf - nicht nur
auf sich selbst. Damit kann die Kaskade zwischen mehreren `AssemblyLink`-Instanzen
hin- und herspringen (Instanz A triggert Instanz B, B trifft evtl. wieder A o.ä.), nicht
nur sich selbst rekursiv aufrufen.

Wird eine Komponente der Quell-Baugruppe gerade per `Document::_removeObject` entfernt,
während `synchronizeComponents()` mitten in seinem Abgleich läuft, findet der
Identitätsvergleich (`linkedObj == obj`, um Zeile 430 in `AssemblyLink.cpp`) nie ein
stabiles Match für das verschwindende Objekt - jeder Wiedereintritt hält es weiterhin
für fehlend und versucht erneut, ein Spiegel-Objekt anzulegen, was wieder `Group` ändert,
was wieder `onChanged`/`updateContents`/`synchronizeComponents` auslöst. Endlos, ohne
jede Log-Ausgabe (keiner der beteiligten Aufrufe schreibt ins Report View).

Der Code kennt das Problem in eine Richtung bereits (Kommentar in
`synchronizeComponents()`, Zeile ~496): das *Entfernen* überzähliger Spiegel-Objekte wird
bewusst übersprungen, wenn die Quelle gerade gelöscht wird ("the link is then in error, and
so AssemblyLink::execute() does not get called"). Für die *Hinzufügen*-Richtung fehlte
aber ein entsprechender Schutz - genau die hängt sich auf.

### Erster Fixversuch war unzureichend

Ein reiner Wiedereintritts-Schutz **pro Instanz** (`bool updatingContents` als
Member-Variable) hat den Hänger nicht behoben - siehe
`gdb-backtrace-nach-fix1-noch-haengend.txt`: der Stacktrace zeigt exakt zwei
`synchronizeComponents -> updateContents -> onChanged`-Ebenen wie vorher, weil die
zweite Ebene zu einer **anderen** `AssemblyLink`-Instanz gehört (über `getInList()`
gefunden), deren eigener Guard naturgemäß noch `false` war. Erst ein **globaler**
(`static`, über alle Instanzen geteilter) Guard hat das Problem behoben.

## Fix

`static bool AssemblyLink::updatingContents`, geprüft am Anfang von
`updateContents()`. Verschachtelte Aufrufe (gleiche oder andere Instanz) werden zum
No-Op; der äußerste Aufruf läuft normal bis zum Ende durch. Ein durch das Überspringen
ausgelassener Sync ist unkritisch - er holt sich beim nächsten regulären
`execute()`/Recompute nach, genau wie das bereits bestehende Verhalten für die
Entfernen-Richtung.

Details, Begründung und der volle Kommentartext stehen im Patch selbst
(`patches/freecad-assembly-link-delete-hang.patch`) sowie in
`src/Mod/Assembly/App/AssemblyLink.h` neben der `updatingContents`-Deklaration.

## Verifikation

Mit dem Fix gebaut, `AssemblyApp.so` installiert, `loeschfehler.FCStd` neu geöffnet,
denselben Löschvorgang wiederholt: Recompute läuft normal durch (`Convergence ≈
9.995e-17`, Recompute-Zeiten im Millisekundenbereich statt Hänger), Hauptthread fällt
danach auf ~0% CPU zurück. Vom Nutzer im laufenden Betrieb bestätigt ("hat
funktioniert").

## Offene Fragen / mögliche Nacharbeit

- Nicht bei FreeCAD upstream eingereicht (kein GitHub-Issue/PR bisher).
- Nicht mit einem synthetischen Minimal-Repro (2 Objekte, 1 mehrfach verlinkte
  Unterbaugruppe) nachgebaut - nur mit der echten `loeschfehler.FCStd`. Ein
  Minimal-Repro wäre für ein Upstream-Issue hilfreich, war für die lokale
  Fehlerbehebung aber nicht nötig.
- Nicht untersucht: ob derselbe Mechanismus auch bei anderen `Group`-ändernden
  Operationen (nicht nur Löschen) auftreten kann.
