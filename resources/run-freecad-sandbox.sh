#!/bin/bash
# Startet die ISOLIERTE Sandbox-Installation (/home/maxx/freecad-sandbox/install - eigener,
# vollstaendiger FreeCAD-Klon, dessen src/Mod/Assembly + src/3rdParty/OndselSolver Symlinks
# auf assembly-solver-sandbox sind, siehe SANDBOX_NOTES.md) statt der normalen, taeglich
# genutzten FreeCAD-Installation (/home/maxx/freecad/install). Immer dieses Skript benutzen,
# wenn ein Live-Test speziell FUER Sandbox-Aenderungen gebraucht wird - niemals die normale
# run-freecad-26.3.sh dafuer zweckentfremden (war 2026-08-28 Ursache eines ABI-Vorfalls).
#
# WICHTIG (2026-09-07, konsolidiert): bis hierhin lief das Skript gegen
# /home/maxx/freecad/install-claude-sandbox (eine per `cp -a` kopierte Installation) - seit es
# /home/maxx/freecad-sandbox/install gibt (direktes `cmake --install`-Ziel des eigenstaendigen
# Klons, KEINE `cp -a`-Kopie), ist das nicht mehr noetig: FC_BIN zeigt jetzt direkt dorthin.
# install-claude-sandbox ist damit abgeloest (kann geloescht werden). Frueherer RUNPATH-Fix
# (LD_LIBRARY_PATH voranstellen) bleibt trotzdem als Vorsichtsmassnahme bestehen - schadet
# nicht, auch wenn er fuer ein direktes `cmake --install`-Ziel nicht mehr zwingend noetig ist
# (siehe ../Dokumente/FreeCAD-Development/assembly-solver-sandbox/docs/JOURNAL.md, "ROOT CAUSE
# GEFUNDEN UND BEHOBEN" fuer die urspruengliche Herleitung).
#
# WICHTIG (2026-08-29): FREECAD_USER_HOME erzwingt ein eigenes, dauerhaftes Profil-Verzeichnis
# (App::Application::getCustomPaths(), src/App/Application.cpp), damit dieses Skript sich nicht
# versehentlich ~/.config/FreeCAD/v26-3/user.cfg mit der echten Installation teilt - Falle
# dabei: das Zielverzeichnis MUSS schon existieren, sonst wird der Wert stillschweigend
# verworfen (deshalb IMMER zuerst mkdir -p).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ISOLATED_PROFILE="${SCRIPT_DIR}/freecad-sandbox-profile"
mkdir -p "$ISOLATED_PROFILE"
export FREECAD_USER_HOME="$ISOLATED_PROFILE"

FC_BIN="/home/maxx/freecad-sandbox/install/bin/FreeCAD"
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

LOG_DIR="${SCRIPT_DIR}/logs"
mkdir -p "$LOG_DIR"
TS="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="${LOG_DIR}/freecad-sandbox-${TS}.log"

echo "Starte FreeCAD-Sandbox (/home/maxx/freecad-sandbox/install), Logfile: $LOG_FILE"

exec "$FC_BIN" -l --log-file "$LOG_FILE" "$@"
