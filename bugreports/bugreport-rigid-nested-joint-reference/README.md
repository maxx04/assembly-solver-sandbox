# FreeCAD-Kernbug: Joint-Referenz in doppelt verschachtelte flexible Unterbaugruppe bricht beim Spiegeln

**Status: Ursache gefunden, gepatcht und verifiziert (2026-08-19).** Patch:
`patches/freecad-assembly-link-delete-hang.patch` (2. Fix darin, siehe README.md dort).
Live vom Nutzer an einer echten, dreifach verschachtelten Baugruppe getestet - Fix
funktioniert (Referenzen sind lokal, Solve läuft durch, Teile bewegen sich korrekt mit).
Der end-to-end-Verifikationsversuch per `freecadcmd` scheiterte an derselben
Cold-Load-Unzuverlässigkeit wie beim 0-Joints-Problem (selbst der Rigid=True-Baseline-
Fall spiegelte in `freecadcmd` nicht zuverlässig) - deshalb live in der GUI verifiziert
statt headless.

Vorsicht: ein separater Fixversuch am selben Tag für die verwandte Rigid-Group-Variante
(`ObjectsToRigidGroup` statt `Reference1`/`Reference2`) hat eine Regression verursacht
(siehe [[project_fcproject_redundant_fixed_joint_rigidgroup_fix]]) - der hier gepatchte
Fix betrifft NUR normale Joint-Referenzen, nicht Rigid Groups.

## Symptom

`CNC3018_022_A_CNC3018_GesamtBaugruppe.FCStd` (`~/Dokumente/CAD_Workspace/PROJ_CNC3018/`)
bindet zwei baugleiche Unterbaugruppen ein: `CNC3018_025_A_FuerungsBaugruppe330` (mehrfach)
und `CNC3018_032_A_FuehrungsBaugruppe400`. Beide enthalten selbst wieder eine verlinkte
`Halterbaugruppe` (`Assembly::AssemblyLink`, zweite Verschachtelungsebene) sowie einen
Joint, der ein Teil *innerhalb* dieser Halterbaugruppe mit einem Teil *außerhalb*
verbindet (Führungsschiene). Bei 330 funktioniert die gespiegelte Verbindung in der
Gesamtbaugruppe korrekt - bei 400 nicht: die Halterbaugruppen erscheinen komplett
unverbunden/losgelöst von der Führung, obwohl der Joint strukturell existiert.

## Diagnose

Direkter XML-Vergleich (`unzip -p ... Document.xml`) der beiden gespiegelten
Verbindungs-Joints in der Gesamtbaugruppe:

```
330 (funktioniert):  Reference1: file=""                                   name="Halterbaugruppe"  Sub="Halter.Edge55"
400 (kaputt):        Reference1: file="FuehrungsBaugruppe400.FCStd"        name="Halter002"        Sub="Edge34"
```

Bei 400 zeigt die Referenz **direkt und mit vollem externem Dateipfad** aufs Enkelkind
(`Halter002`, das eigentliche Teil innerhalb der Halterbaugruppe) - bei 330 zeigt sie
**kompakt** auf die direkte Kind-Baugruppe (`Halterbaugruppe`), mit dem Enkelkind nur als
Teil des zusammengesetzten `Sub`-Strings. Diese Kodierung ist schon in der jeweiligen
**Quelldatei selbst** unterschiedlich, nicht erst nach dem Spiegeln entstanden.

Root Cause, zwei zusammenwirkende Stellen:

**1. `UtilsAssembly.getComponentReference()`** (`src/Mod/Assembly/UtilsAssembly.py:1233`) -
entscheidet beim Anlegen eines Joints anhand der Flächenauswahl, wo der Referenzpfad
"stoppt":

```python
if obj.isDerivedFrom("Assembly::AssemblyLink"):
    if hasattr(obj, "Rigid") and not obj.Rigid:
        continue  # ueberspringt die Unterbaugruppe, laeuft bis zum Enkelkind durch
```

Ist die verschachtelte `Halterbaugruppe` als `Rigid=True` eingefügt, stoppt die Funktion
dort (kompakte Referenz). Ist sie `Rigid=False` (flexibel - nötig, damit ihre eigenen
internen Joints/Rigid Groups überhaupt live mitgerechnet werden, siehe
[[project_fcproject_redundant_fixed_joint_rigidgroup_fix]]), überspringt sie die
Unterbaugruppe und läuft bis zum eigentlichen Teil durch (flache Referenz).

**2. `AssemblyLink::synchronizeComponents()`** (`src/Mod/Assembly/App/AssemblyLink.cpp`) -
das `objLinkMap`-Dictionary, über das `handleJointReference()` beim Spiegeln nach außen
externe Objekte auf lokale Kopien umbiegt, wird nur für die **direkten** Top-Level-
Komponenten der gespiegelten Baugruppe gefüllt - nie für Enkelkinder.

Zusammen: bei `Rigid=True` zeigt die Referenz von Anfang an nur auf ein direktes Kind
→ `objLinkMap`-Lookup trifft → Spiegeln funktioniert. Bei `Rigid=False` zeigt die
Referenz auf ein Enkelkind → `objLinkMap`-Lookup trifft nie → Referenz bleibt auf die
externe Quelldatei zeigen, der gespiegelte Joint wirkt nicht.

**Wichtig:** Beides sind offiziell gleichwertige, unterstützte Optionen beim Einfügen
einer Unterbaugruppe (Checkbox "als starre Baugruppe importieren"). Dass nur eine davon
bei weiterer Verschachtelung funktioniert, ist ein Bug, kein Bedienfehler.

## Minimal-Repro (`repro.py`)

Baut zwei winzige, eigenständige Dokument-Paare (Unterbaugruppe mit einem Teil,
eingebunden in eine Außenbaugruppe) - einmal mit `Rigid=True`, einmal mit `Rigid=False` -
und ruft `UtilsAssembly.getComponentReference()` mit identischer Auswahl-Situation
(`"SubLink.InnerPart.Face1"`) auf:

```
=== Rigid=True ===
  getComponentReference(...) -> component = SubLink, new_sub = 'InnerPart.Face1'
    -> KOMPAKT: zeigt auf SubLink selbst - Spiegeln nach aussen funktioniert.

=== Rigid=False ===
  getComponentReference(...) -> component = InnerPart, new_sub = 'Face1'
    -> FLACH: zeigt direkt auf InnerPart (Enkelkind) - objLinkMap beim Spiegeln kennt
       das nicht -> BUG.
```

Ausführen: `freecadcmd patches/bugreport-rigid-nested-joint-reference/repro.py`
(erzeugt temporäre `.FCStd`-Dateien unter `repro_docs/` im selben Ordner, nicht Teil des
Repros selbst - können nach dem Lauf gelöscht werden).

## Fix - umgesetzt (Variante a)

`handleJointReference()` mehrstufig gemacht: schlägt `objLinkMap.find(externalComponent)`
fehl, läuft die neue Methode `AssemblyLink::findLocalAncestor()` die Struktur-Eltern-Kette
von `externalComponent` hoch (über die `Group`-Property der `getInList()`-Kandidaten -
nicht blind `getInList().front()`, das kann auch Joints statt des echten Containers
treffen), bis ein in `objLinkMap` bekannter Vorfahre gefunden wird, und liefert den
durchlaufenen Pfad als `Sub`-Präfix zurück - analog zu `getComponentReference()`s eigener
Kodierung beim Joint-Anlegen. `maxDepth`-Begrenzung (32) als Zyklen-Bremse, gleiches
Muster wie `getJointContextName()` in `AssemblyObject.cpp`.

Patch: `patches/freecad-assembly-link-delete-hang.patch` (2. Eintrag). Live an einer
echten, dreifach verschachtelten Baugruppe verifiziert (siehe Status oben) - nicht per
`freecadcmd`, das scheiterte selbst am Rigid=True-Baseline-Fall (siehe
[[project_fcproject_freecadcmd_zero_joints_cold_load]]).

Variante (b) - `objLinkMap` von vornherein rekursiv befüllen - wurde nicht umgesetzt,
Variante (a) war zielgerichteter und ausreichend.

## Offene Fragen / mögliche Nacharbeit

- Nicht bei FreeCAD upstream eingereicht (kein GitHub-Issue/PR bisher) - `repro.py` ist
  dafür vorbereitet.
- Nicht geprüft, ob der Fix auch bei 3+ Verschachtelungsebenen (Enkelkind hat selbst
  wieder ein Kind, das referenziert wird) korrekt durchläuft - `findLocalAncestor()`s
  Schleife sollte das grundsätzlich abdecken (läuft beliebig viele Ebenen hoch bis
  `maxDepth`), aber noch nicht konkret getestet.
- Die verwandte Rigid-Group-Variante (`ObjectsToRigidGroup` statt `Reference1`/
  `Reference2`) ist weiterhin ungefixt, siehe
  [[project_fcproject_redundant_fixed_joint_rigidgroup_fix]].
