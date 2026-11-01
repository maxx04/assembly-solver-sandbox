#!/usr/bin/env bash
#
# rebuild-fresh.sh - kompletter Neubau des FreeCAD-Build-Baums aus einem LEEREN Build-Ordner,
# danach Installation nach /home/maxx/freecad-sandbox/install. Teil des woechentlichen
# Standard-Updates (Nutzerauftrag 2026-10-10): nach update-sandbox.sh immer frisch bauen, nie
# inkrementell ueber einen Toolchain-Wechsel hinweg (stale moc/_autogen).
#
# Seit 2026-11-01 (Ubuntu 26.04, Nutzerauftrag "aendere auf pixi, weiterhin auf cmake basiert"):
# Umgebung wie FreeCADs eigener Pixi-Build (conda-forge, Python 3.13, Qt 6.11, clang + mold) - die
# Weekly-AppImage (unsere Vanilla-Referenz) wird genauso gebaut. Vorher eigene KDE-Neon-Umgebung
# (nur fuer Ubuntu 24.04 verfuegbar).
#   1. `pixi install --locked` im Quellordner (Umgebung exakt nach FreeCADs pixi.lock,
#      <freecad-source>/.pixi/envs/default).
#   2. Build-Ordner leeren, mit FreeCADs Preset conda-linux-rel-with-deb-info konfigurieren - aber in
#      UNSEREN Build-Ordner und mit Ziel /home/maxx/freecad-sandbox/install statt in die
#      Pixi-Umgebung. RPATH enthaelt die Pixi-Bibliotheken, damit die Installation sie auch ohne
#      aktivierte Umgebung findet.
#   3. bauen, 4. installieren (bricht ab, wenn die Sandbox-FreeCAD laeuft).
# Alle Schritte laufen ueber `pixi run`, also mit Compiler, cmake und Bibliotheken der Umgebung.
#
# Aufgerufen vom CMake-Ziel rebuild-fresh-and-deploy (standalone-check/CMakeLists.txt).
#
# Aufruf: rebuild-fresh.sh <freecad-source> <build-dir> <install-dir> <jobs>

set -euo pipefail

SRC="$(realpath -m "${1:?freecad-source fehlt}")"
BUILD="$(realpath -m "${2:?build-dir fehlt}")"
INSTALL="$(realpath -m "${3:?install-dir fehlt}")"
JOBS="${4:?jobs fehlt}"
PIXI="${PIXI:-/home/maxx/.pixi/bin/pixi}"
ENV_DIR="${SRC}/.pixi/envs/default"
LOGDIR="$(dirname "$SRC")"

pixi_run() {
    # wie FreeCADs Pixi-Tasks: CFLAGS & Co. leer, sonst setzt die conda-Aktivierung eigene Flags
    "$PIXI" run --manifest-path "${SRC}/pixi.toml" --frozen \
        env CFLAGS= CXXFLAGS= DEBUG_CFLAGS= DEBUG_CXXFLAGS= "$@"
}

log() { printf '\n=== %s -- %s ===\n\n' "$(date +%H:%M:%S)" "$*"; }

log "1/4: Pixi-Umgebung nach pixi.lock"
(cd "$SRC" && "$PIXI" install --locked)

log "2/4: Build-Ordner leeren und neu konfigurieren"
rm -rf "$BUILD"
(cd "$SRC" && pixi_run cmake --preset conda-linux-rel-with-deb-info -B "$BUILD" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL" \
    -DCMAKE_INSTALL_RPATH="${INSTALL}/lib;${ENV_DIR}/lib" \
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON) > "${LOGDIR}/rebuild-fresh-configure.log" 2>&1 || {
    echo "FEHLER beim Konfigurieren - siehe ${LOGDIR}/rebuild-fresh-configure.log" >&2
    tail -20 "${LOGDIR}/rebuild-fresh-configure.log" >&2
    exit 1
}
echo "Konfiguriert."

log "3/4: bauen (${JOBS} Jobs)"
start=$(date +%s)
(cd "$SRC" && pixi_run cmake --build "$BUILD" --parallel "$JOBS")
echo "Gebaut in $(( ($(date +%s) - start) / 60 )) min."

log "4/4: installieren (in einen leeren Ordner, siehe install-clean.sh)"
bash "$(dirname "${BASH_SOURCE[0]}")/install-clean.sh" "$SRC" "$BUILD" "$INSTALL"
echo "Naechster Schritt: Kinematik-Matrix"
echo "  standalone-check/kinematic-tests/common/run-all-tests.sh ${INSTALL}"
