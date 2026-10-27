# Faktenblatt: Starre Gruppe (RigidGroupJoint) - Traceback beim Anlegen, Warnung bei jedem Öffnen

Stand 2026-10-27. Zusammenfassung für einen Bugreport, den der Nutzer selbst schreibt
(FreeCAD-Regel: Issues/PR-Texte von Menschen). Kein fertiger Text.

Datei: `src/Mod/Assembly/JointObject.py`, Klasse `RigidGroupJoint` (im Sandbox-Stand gegenüber
Upstream unverändert).

## 1. Traceback beim Anlegen einer starren Gruppe

```
Traceback (most recent call last):
  ...
    fp.setPropertyStatus("RigidPlacements", "-ReadOnly")
<class 'AttributeError'>: Property container has no property 'RigidPlacements'
Assembly: Rigid group positions updated.
```

- `RigidGroupJoint.__init__()` setzt `joint.ObjectsToRigidGroup = ...`, **bevor** die
  Eigenschaft `RigidPlacements` angelegt ist. Das Setzen löst `onChanged("ObjectsToRigidGroup")`
  -> `updateStoredPositions()` -> `_write_rigid_placements()` aus, das auf die noch fehlende
  Eigenschaft zugreift.
- Folgen: nur der Traceback im Report-Fenster. Der Konstruktor ruft danach
  `updateStoredPositions(joint, True)` noch einmal auf, die Lagen werden korrekt gespeichert.
- Naheliegende Behebung: `RigidPlacements` vor dem Zuweisen von `ObjectsToRigidGroup` anlegen.

## 2. Warnung „Rigid group sync broken, skipping restore.“ bei jedem Öffnen

- Tritt bei jedem Öffnen einer Datei mit starrer Gruppe auf, obwohl die gespeicherten Lagen
  vollständig sind (geprüft: nach dem Laden `len(RigidPlacements) == len(ObjectsToRigidGroup)`;
  in der Datei liegt die Binärdatei `RigidPlacements` mit allen Einträgen).
- Ursache: `onChanged()` reagiert auf `Suppressed` und ruft `restoreFromPlacements()` auf -
  auch während das Dokument eingelesen wird. FreeCAD liest zuerst die XML-Eigenschaften
  (darunter `Suppressed`), die `PropertyPlacementList` `RigidPlacements` aber erst danach aus
  ihrer Binärdatei. Beim Einlesen von `Suppressed` ist die Liste noch leer -> Längen ungleich
  -> Warnung.
- Inhaltlicher Punkt: `restoreFromPlacements()` soll beim **Aufheben** der Unterdrückung die
  Mitglieder an ihre gespeicherten relativen Lagen setzen. Beim Einlesen eines Dokuments ist
  das nicht gewollt (es würde Mitglieder beim Öffnen versetzen) und wird derzeit nur zufällig
  durch die noch leere Liste verhindert.
- Naheliegende Behebung: in `onChanged()` während des Einlesens nichts tun
  (z.B. `if "Restore" in fp.State: return`).

## Versionen (geprüft)

- FreeCAD 27.1.0 Weekly 2026.10.21, offizielle AppImage (`27.1.0devR20261020`), Linux x86_64:
  beide Punkte nachgestellt (Vanilla).
- Eigene Sandbox auf Upstream `4218d22c3a`: gleiches Verhalten (auch in echten Projektdateien
  mit 6 und 21 Mitgliedern).

## Reproduktion

`rg_repro.py` in FreeCAD ausführen (Makro oder Python-Konsole, `exec(open(...).read())`):
legt eine Baugruppe mit zwei Würfeln und einer starren Gruppe an (-> Punkt 1), speichert,
öffnet neu (-> Punkt 2) und gibt die Anzahl der gespeicherten Lagen vor und nach dem Laden aus
(jeweils 2). Die Datei landet im Benutzerdatenordner (`rg_repro.FCStd`).
