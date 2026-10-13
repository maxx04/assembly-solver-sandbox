#!/usr/bin/env bash
#
# rebuild-fresh.sh - kompletter Neubau des FreeCAD-Build-Baums aus einem LEEREN Build-Ordner,
# danach Installation nach /home/maxx/freecad-sandbox/install. Teil des woechentlichen
# Standard-Updates (Nutzerauftrag 2026-10-10): nach update-sandbox.sh (Vanilla + Neon) immer
# frisch bauen, nie inkrementell ueber einen Toolchain-Wechsel hinweg (stale moc/_autogen, siehe
# Memory reference-neon-qt6-pyside6-environment).
#
# Aufgerufen vom CMake-Ziel rebuild-fresh-and-deploy (standalone-check/CMakeLists.txt) - Build und
# Installation laufen damit wie gewohnt ueber CMake.
#
# Die Konfiguration bleibt exakt erhalten: vor dem Loeschen werden alle Projekt-Optionen
# (BUILD_*, FREECAD_*, ENABLE_*, CMAKE_BUILD_TYPE, CMAKE_INSTALL_PREFIX, CMAKE_PREFIX_PATH,
# Python3_EXECUTABLE) aus dem bisherigen CMakeCache.txt in eine Initial-Cache-Datei geschrieben
# (freecad-build-options.cmake neben dem Build-Baum) und beim Neukonfigurieren per -C geladen.
# Fehlt der Build-Ordner, wird die zuletzt gespeicherte Datei verwendet.
#
# Aufruf: rebuild-fresh.sh <freecad-source> <build-dir> <neon-lib-dir> <jobs>

set -euo pipefail

SRC="$(realpath -m "${1:?freecad-source fehlt}")"
BUILD="$(realpath -m "${2:?build-dir fehlt}")"
NEON_LIB="${3:?neon-lib-dir fehlt}"
JOBS="${4:?jobs fehlt}"
OPTIONS="$(dirname "$SRC")/freecad-build-options.cmake"

export LD_LIBRARY_PATH="${NEON_LIB}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

log() { printf '\n=== %s -- %s ===\n\n' "$(date +%H:%M:%S)" "$*"; }

log "1/4: Build-Optionen sichern"
if [[ -f "${BUILD}/CMakeCache.txt" ]]; then
    grep -E '^(BUILD_[A-Z0-9_]+|FREECAD_[A-Z0-9_]+|ENABLE_[A-Z0-9_]+|CMAKE_BUILD_TYPE|CMAKE_INSTALL_PREFIX|CMAKE_PREFIX_PATH|Python3_EXECUTABLE):' \
        "${BUILD}/CMakeCache.txt" \
        | grep -v ':INTERNAL=' \
        | sed -E 's/^([^:]+):([A-Z]+)=(.*)$/set(\1 "\3" CACHE \2 "")/' \
        | sed -E 's/ CACHE UNINITIALIZED / CACHE STRING /' \
        > "${OPTIONS}.tmp"
    mv "${OPTIONS}.tmp" "$OPTIONS"
    echo "$(wc -l < "$OPTIONS") Optionen nach ${OPTIONS} geschrieben."
elif [[ -f "$OPTIONS" ]]; then
    echo "Kein CMakeCache.txt - verwende gespeicherte ${OPTIONS}."
else
    echo "FEHLER: weder ${BUILD}/CMakeCache.txt noch ${OPTIONS} vorhanden." >&2
    exit 1
fi

log "2/4: Build-Ordner leeren und neu konfigurieren"
rm -rf "$BUILD"
cmake -S "$SRC" -B "$BUILD" -C "$OPTIONS" > "$(dirname "$SRC")/rebuild-fresh-configure.log" 2>&1 || {
    echo "FEHLER beim Konfigurieren - siehe $(dirname "$SRC")/rebuild-fresh-configure.log" >&2
    tail -20 "$(dirname "$SRC")/rebuild-fresh-configure.log" >&2
    exit 1
}
echo "Konfiguriert."

log "3/4: bauen (${JOBS} Jobs)"
start=$(date +%s)
cmake --build "$BUILD" --parallel "$JOBS"
echo "Gebaut in $(( ($(date +%s) - start) / 60 )) min."

log "4/4: installieren"
# Eine laufende Sandbox-FreeCAD wuerde beim Ueberschreiben ihrer Bibliotheken abstuerzen.
if pgrep -af "^/home/maxx/freecad-sandbox/install/bin/FreeCAD" | grep -qv "kinematic-test"; then
    echo "FEHLER: die Sandbox-FreeCAD laeuft noch - bitte schliessen und danach nur installieren:" >&2
    echo "  LD_LIBRARY_PATH=${NEON_LIB} cmake --install ${BUILD}" >&2
    exit 1
fi
cmake --install "$BUILD" > "$(dirname "$SRC")/rebuild-fresh-install.log" 2>&1
echo "Installiert. Naechster Schritt: Kinematik-Matrix"
echo "  standalone-check/kinematic-tests/common/run-all-tests.sh /home/maxx/freecad-sandbox/install"
