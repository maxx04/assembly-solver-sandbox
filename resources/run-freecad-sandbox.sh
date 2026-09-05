#!/bin/bash
# Startet die ISOLIERTE Sandbox-Installation des Hintergrund-Agenten
# (/home/maxx/freecad/install-claude-sandbox) statt der normalen FreeCAD-Installation - dorthin
# deployt der Agent seine C++-Diagnose-/Fix-Builds (siehe project_fcproject_solver_data_fix_and_
# import_component-Memory, "Betriebs-Lektion" zum ABI-Vorfall vom 2026-08-28). Immer dieses
# Skript benutzen, wenn ein Live-Test speziell FUER den Agenten gebraucht wird - niemals die
# normale run-freecad-26.3.sh dafuer zweckentfremden, das war Ursache des ABI-Vorfalls.
#
# WICHTIG (2026-08-29, Nachtrag): dieses Skript (bzw. das gepinnte Desktop-Icon "FreeCAD Sandbox")
# hatte urspruenglich KEIN eigenes Nutzerprofil und teilte sich dadurch versehentlich
# ~/.config/FreeCAD/v26-3/user.cfg mit der echten, taeglich genutzten Installation - ein
# Testlauf darueber hat dabei einmal die komplette Fensteranordnung des Nutzers ueberschrieben.
# Deshalb jetzt per FREECAD_USER_HOME (App::Application::getCustomPaths(),
# src/App/Application.cpp) ein eigenes, dauerhaftes Profil-Verzeichnis erzwungen - Falle dabei:
# das Zielverzeichnis MUSS schon existieren, sonst wird der Wert stillschweigend verworfen und
# FreeCAD faellt auf den echten, geteilten Pfad zurueck (deshalb IMMER zuerst mkdir -p).
# WICHTIG (2026-08-29, zweiter Nachtrag - ROOT CAUSE der toten 3D-Ansicht gefunden): jede per
# `cp -a` erzeugte Kopie der echten Installation (wie diese Sandbox) traegt ein FEST
# EINPROGRAMMIERTES RUNPATH im FreeCAD-Binary, das UNABHAENGIG vom tatsaechlichen
# Installationsort weiterhin auf /home/maxx/freecad/install/lib zeigt (readelf -d bestaetigt).
# Ohne Gegenmassnahme laedt der dynamische Linker FreeCADApp.so/FreeCADGui.so/etc. von DORT statt
# vom eigenen lib-Ordner - die 3D-Ansicht bleibt dadurch komplett leer (Overlay/NaviCube rendert
# normal, Geometrie nicht), siehe patches/assembly-architecture-overview.md ("ROOT CAUSE
# GEFUNDEN UND BEHOBEN") fuer die volle Herleitung samt Screenshots. Fix: eigenen lib-Ordner per
# LD_LIBRARY_PATH voranstellen (hat Vorrang vor dem einprogrammierten RUNPATH).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISOLATED_PROFILE="${SCRIPT_DIR}/freecad-sandbox-profile"
mkdir -p "$ISOLATED_PROFILE"
export FREECAD_USER_HOME="$ISOLATED_PROFILE"

FC_BIN="/home/maxx/freecad/install-claude-sandbox/bin/FreeCAD"
FC_LIB_DIR="$(cd "$(dirname "$FC_BIN")/../lib" && pwd)"

FALLBACK_VENV_DIR="/home/maxx/Dokumente/FreeCAD-Development/.venv"
if VENV_DIR="$(cd "${SCRIPT_DIR}/../../.venv" 2>/dev/null && pwd)"; then
    :
else
    VENV_DIR="${FALLBACK_VENV_DIR}"
fi
source "${VENV_DIR}/bin/activate"
export VIRTUAL_ENV="${VENV_DIR}"

PYSIDE_QT="${VIRTUAL_ENV}/lib/python3.12/site-packages/PySide6/Qt"

export QT_PLUGIN_PATH="${PYSIDE_QT}/plugins"
export LD_LIBRARY_PATH="${FC_LIB_DIR}:${PYSIDE_QT}/lib:/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONNOUSERSITE=1
export PYTHONPATH="${VIRTUAL_ENV}/lib/python3.12/site-packages"
export QT_QPA_PLATFORM=xcb

LOG_DIR="$HOME/freecad/logs"
mkdir -p "$LOG_DIR"
TS="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="${LOG_DIR}/freecad-sandbox-${TS}.log"

echo "Starte FreeCAD-SANDBOX (Agent-Diagnose-Build) mit RUNPATH-Fix (eigener lib-Ordner: $FC_LIB_DIR), Logfile: $LOG_FILE"

exec "$FC_BIN" -l --log-file "$LOG_FILE" "$@"
