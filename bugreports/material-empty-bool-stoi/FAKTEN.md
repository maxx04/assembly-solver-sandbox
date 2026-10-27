# Faktenblatt: Materialdatei mit leerem Bool-Wert legt die Materialbibliothek lahm

Stand 2026-10-27. Zusammenfassung für einen Bugreport, den der Nutzer selbst schreibt
(FreeCAD-Regel: Issues/PR-Texte von Menschen, siehe #33672/#33673). Kein fertiger Text.

## Was passiert ist

- Im Material-Editor wurde beim Material „PET-Generic“ (eigenes Material im Benutzerprofil,
  Vorlage „PET“) versehentlich das Darstellungsmodell **„Render Workbench“** mit der
  Eigenschaft **`UseObjectColor`** hinzugefügt, ohne einen Wert zu setzen.
- FreeCAD hat die Datei so gespeichert: `<Profil>/Material/PET-Generic.FCMat`, Kopfzeile
  `# File created by FreeCAD 27.1.0 Revision: 49308 (Git)`, am Ende:

  ```yaml
  AppearanceModels:
    ...
    Render Workbench:
      UUID: "344008be-a837-43af-90bc-f795f277b309"
      UseObjectColor: ""
  ```

- Danach ließ sich in FreeCAD kein Dokument mehr öffnen, dessen Teil ein Material hat.

## Beobachtung

- Öffnen eines Dokuments mit Material (hier ein Profil mit `ShapeMaterial`):
  `Exception opening file: …/CNC3018_003_B_Profil_20x20x360.FCStd [stoi]`,
  in Python `OSError('Invalid project file …: stoi')`.
- Verweise auf dieses Dokument (App::Link aus einer Baugruppe): „Link not restored“ bzw. im
  Report-Fenster „…: Link broken!“, danach mehrfach „Material not found“.
- Schon `Materials.MaterialManager().getMaterial("<uuid>")` bricht ab. Die C++-Ausnahme kommt
  nicht als Python-Fehler an, das Skript endet mit „Unknown exception while processing file“
  (`minrepro.py`).
- Nur mit dieser Datei im Benutzerprofil reproduzierbar, mit frischem Profil alles normal.

## Gegenprobe

- `UseObjectColor: "0"` statt `""`: Material wird gefunden (209 Materialien), Dokumente öffnen
  sich, kein „Material not found“.
- Dezimalkommas (siehe unten) durch Punkte ersetzt, leeren Wert gelassen: Fehler bleibt. Ursache
  ist also der leere Bool-Wert.

## Versionen (geprüft)

- FreeCAD 27.1.0 Weekly 2026.10.21, offizielle AppImage (`27.1.0devR20261020`), Linux x86_64:
  Fehler tritt auf (Vanilla).
- Eigene Sandbox auf Upstream `4218d22c3a` (Material-Modul unverändert): gleiches Verhalten.

## Vermutete Ursache

- Beim Lesen wird der Bool-Wert per `std::stoi` umgewandelt. Ein leerer Text wirft
  `std::invalid_argument` („stoi“). Die Ausnahme wird nicht abgefangen und reißt das Laden der
  ganzen Materialbibliothek mit, statt nur dieses eine Material zu überspringen.
- Beim Speichern schreibt der Editor für eine hinzugefügte, aber nicht gesetzte Eigenschaft einen
  leeren Wert, den FreeCAD selbst nicht wieder lesen kann.

## Erwartet

- Nicht gesetzte Eigenschaft nicht schreiben oder beim Lesen als Standardwert behandeln.
- Ein fehlerhaftes Material höchstens dieses Material betreffen lassen, mit verständlicher
  Meldung (Datei, Eigenschaft) - nicht das Öffnen von Dokumenten.

## Nebenbeobachtung: Dezimalkomma

In derselben Datei hat der Editor Zahlen mit **Dezimalkomma** geschrieben (deutsches
Gebietsschema), das Original-Material verwendet Punkte:

| Eigenschaft | gespeichert | Original (PET) |
|---|---|---|
| ThermalConductivity | `"0,181 W/m/K"` | `"0.181 W/m/K"` |
| ShearModulus | `"38,5 MPa"` | `"0.0385 GPa"` |
| UltimateTensileStrength | `"38,8 MPa"` | `"38.8 MPa"` |
| YieldStrength | `"62,8 MPa"` | `"62.8 MPa"` |

Die Datei ließ sich mit Kommas lesen (nach dem Reparieren von `UseObjectColor`), sie hängt aber
vom Gebietsschema ab: Auf einem System mit englischem Gebietsschema könnten diese Werte falsch
oder gar nicht gelesen werden. Materialdateien sollten gebietsschema-unabhängig mit Punkt
gespeichert werden. Nicht gesondert geprüft.

## Anhänge

- `PET-Generic_kaputt.FCMat` - die Datei wie von FreeCAD gespeichert (mit leerem `UseObjectColor`)
- `minrepro.py` - Nachstellung: Datei nach `<Profil>/Material/` legen, Skript in FreeCAD ausführen

## Reproduktion (Kurzform)

1. `PET-Generic_kaputt.FCMat` in `<Benutzerprofil>/Material/` kopieren.
2. FreeCAD starten, in der Python-Konsole:
   `import Materials; Materials.MaterialManager().getMaterial("3ca74cf2-e188-4438-af08-aa9d6b5b955d")`
   -> Abbruch ("stoi") statt Material bzw. sauberer Fehlermeldung.
   Alternativ ein Dokument öffnen, dessen Teil ein Material zugewiesen hat.
