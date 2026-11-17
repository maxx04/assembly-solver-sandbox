#!/usr/bin/env bash
#
# fcstd-diff.sh
#
# Git-textconv-Helfer fuer .FCStd-Dateien: FreeCAD speichert IMMER als ZIP-Container (siehe
# App::Document::saveToFile(), zipios::ZipOutputStream - auch bei CompressionLevel=0 bleibt der
# ZIP-Rahmen bestehen), deshalb zeigt "git diff" normalerweise nur "Bin X -> Y bytes" statt
# lesbarer Zeilen. Dieses Skript entpackt nur die eigentliche Modelldatei (Document.xml) und
# gibt sie auf stdout aus - "git diff"/"git log -p" zeigen dann einen normalen Text-Diff.
#
# Aktivierung (git liest textconv-Befehle aus Sicherheitsgruenden NIE aus einer getrackten
# Datei, nur aus lokaler/globaler Konfiguration - deshalb pro Klon einmalig noetig):
#   git config diff.fcstd.textconv "$(pwd)/tools/fcstd-diff.sh"
# (.gitattributes im Repo-Root ordnet *.FCStd bereits dem Diff-Treiber "fcstd" zu.)
#
# Aendert NICHTS an der gespeicherten Datei selbst - reine Anzeige-Hilfe fuer git diff/log/show.

set -euo pipefail
unzip -p -- "$1" Document.xml
