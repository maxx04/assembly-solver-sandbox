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
# WICHTIG (2026-09-24, "wie FreeCAD selbst arbeiten"): FreeCAD wird jetzt gegen eine EIGENE,
# in sich konsistente Qt6/PySide6/Shiboken6-Umgebung gebaut (neon-qt6-pyside6/root/, aus dem
# offiziellen KDE-Neon-Repo als .deb-Pakete heruntergeladen und in einen reinen Projektordner
# entpackt - GENAU der Weg, den FreeCADs eigenes CI-Skript package/ubuntu/install-apt-packages.sh
# geht, nur ohne das System zu veraendern). Grund: System-Qt (Ubuntu 24.04, 6.4.2) und
# pip-PySide6 (aus .venv) hatten unterschiedliche, inkompatible Qt-ABI-Erwartungen - das fuehrte
# zu "Cannot call meta function ... Base::Quantity cannot be converted" im Simulation-Dialog UND
# verhinderte FREECAD_USE_SHIBOKEN=ON komplett (Linker-Fehler gegen System-Qt). Mit dieser
# einheitlichen Umgebung (alles Qt 6.11.1) funktioniert Shiboken sauber. Das alte .venv wird
# hierfuer nicht mehr gebraucht (nur noch als beschreibbares Ziel fuer FreeCADs eigenes kleines
# "freecad"-Python-Paket, das System-Python-dist-packages verlangt - PySide6/Shiboken6 selbst
# liegen dort als Symlinks auf dieses Neon-PySide6, siehe weiter unten).
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

NEON_ROOT="/home/maxx/Dokumente/FreeCAD-Development/neon-qt6-pyside6/root/usr"
NEON_LIB="${NEON_ROOT}/lib/x86_64-linux-gnu"

# FCPROJECT-PATCH (2026-09-24): FreeCADs eigener Interpreter.cpp::initInterpreter() ignoriert
# PYTHONPATH (isolierter Python-Start), liest aber explizit VIRTUAL_ENV aus und haengt
# "$VIRTUAL_ENV/lib/pythonX.Y/site-packages" an sys.path an - das ist der einzige Weg, wie
# unser aus dem Neon-Repo entpacktes PySide6/Shiboken6 6.11.1 gefunden wird. Unser eigenes
# .venv leitet dorthin per .pth-Datei um (venv-eigenes PySide6 6.6.3 wurde deinstalliert).
export VIRTUAL_ENV="/home/maxx/Dokumente/FreeCAD-Development/.venv"

export QT_PLUGIN_PATH="${NEON_LIB}/qt6/plugins"
export LD_LIBRARY_PATH="${FC_LIB_DIR}:${NEON_LIB}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONNOUSERSITE=1
export QT_QPA_PLATFORM=xcb

LOG_DIR="${SCRIPT_DIR}/logs"
mkdir -p "$LOG_DIR"
TS="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="${LOG_DIR}/freecad-sandbox-${TS}.log"

echo "Starte FreeCAD-Sandbox (/home/maxx/freecad-sandbox/install), Logfile: $LOG_FILE"

# WICHTIG (2026-09-15, nach echtem SIGSEGV-Absturz ohne Backtrace): per Default ist die
# Core-Dump-Groesse fuer diese Shell (und damit fuer den exec'ten FreeCAD-Prozess) auf 0
# begrenzt. /proc/sys/kernel/core_pattern leitet Abstuerze zwar bereits an apport weiter
# (systemweit, per systemctl aktiv) - apport ignoriert einen Crash aber komplett ("does not
# belong to a package, ignoring"), wenn der crashende Prozess selbst ein Core-Limit von 0 hat,
# UNABHAENGIG davon, dass die Binary hier nicht aus einem System-Paket stammt. ulimit -c
# unlimited hier hebt das NUR fuer diesen einen Prozess auf (kein systemweiter Eingriff, keine
# Root-Rechte noetig) - danach sollte ein Absturz unter /var/crash/*.crash (apport) bzw. per
# `coredumpctl list` (falls systemd-coredump zusaetzlich aktiv ist) einen echten Kern-Dump samt
# Backtrace liefern.
ulimit -c unlimited

exec "$FC_BIN" -l --log-file "$LOG_FILE" "$@"
