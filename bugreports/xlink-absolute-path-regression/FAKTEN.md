# Faktenblatt: Seit 24ea8589aa speichert FreeCAD Dateiverweise (XLink) mit absolutem Pfad

Stand 2026-11-01. Unterlage für den Nutzer (FreeCAD-Regel: Issue-/PR-Texte schreibt ein Mensch).
Kein fertiger Text.

## 1. Befund

FreeCAD main ab Commit `24ea8589aa` („fix errored XLink's file property“, zu Issue #33468, Autor
Nicolas Mure, 2026-10-22). Repro `xlink_probe.py`: Teil.FCStd speichern, Bg.FCStd mit App::Link auf
den Quader speichern, `Document.xml` von Bg.FCStd lesen.

| Build | gespeicherter Verweis |
|---|---|
| Weekly 2026.10.28 (enthält den Commit noch nicht) | `<XLink file="Teil.FCStd" …>` (relativ) |
| main mit 24ea8589aa (Sandbox-Build, Pixi) | `<XLink file="/tmp/xlinkprobe…/Teil.FCStd" …>` (absolut) |

## 2. Ursache

`PropertyXLink::Save()` (src/App/PropertyLinks.cpp) nimmt seit dem Commit
`getDocumentPath()` statt `filePath`. `getDocumentPath()` liefert
`docInfo ? docInfo->filePath() : filePath.c_str()` - bei geladenem Zieldokument also den
**absoluten** Pfad aus DocInfo. Der Commit-Text erwartet dort den relativen Pfad.

## 3. Folgen

- Jede gespeicherte Baugruppe verweist mit festem Pfad auf ihre Teile: Projektordner lässt sich
  nicht mehr verschieben, auf einem anderen Rechner/Benutzer finden sich die Teile nicht.
- Nach „Speichern unter“ eines verlinkten Teils (neuer Name) schreibt eine geöffnete Baugruppe beim
  nächsten Speichern den **alten** absoluten Pfad - DocInfo wird dabei offenbar nicht nachgeführt
  (FCProject-Test `fc_docobserver_rename_check.py`, Fall „A geöffnet“).

## 4. Vorschlag

Den ursprünglichen Fehler aus #33468 (falscher Pfad durch das Arbeitsverzeichnis) anders beheben,
z.B. den relativen Pfad aus `docInfo->filePath()` relativ zum Ordner des speichernden Dokuments
bilden (`DocInfo::getDocPath(..., relative=true)` bzw. wie beim Export-Zweig darunter), statt den
absoluten Pfad zu schreiben. Die Sandbox nimmt die Zeile bis dahin per Build-Patch zurück
(`patches/2026.11.01-freecad-xlink-relative-path-revert.patch`).

## 5. Zu prüfen vor dem Melden

- Gegen die nächste Weekly, die 24ea8589aa enthält (erwartet: absolut).
- Issue #33468 lesen: welchen Fall der Commit beheben sollte, damit der Vorschlag ihn nicht wieder
  aufbricht.
