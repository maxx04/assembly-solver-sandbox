#!/usr/bin/env bash
#
# install-clean.sh - installiert den Build-Baum in einen LEEREN Installationsordner (2026-11-01).
# Gemeinsamer Installationsschritt aller cmake-Ziele (build-and-deploy, build-all-and-deploy,
# rebuild-fresh-and-deploy).
#
# Warum leer: `cmake --install` ueberschreibt nur, loescht aber nichts, was ein neuer Build nicht
# mehr installiert. Beim Wechsel Neon -> Pixi blieb so das alte, mitgebaute SMESH (VTK 9.1) in
# install/lib liegen, ueberdeckte das SMESH der Pixi-Umgebung, FEM/MeshPart fanden
# libvtkCommonCore-9.1 nicht und FreeCAD beendete sich beim Start.
#
# Der vorige Stand bleibt als <install>.prev liegen (genau einer). Bricht ab, solange die
# Sandbox-FreeCAD laeuft (Testlaeufe unter Xvfb ausgenommen).
#
# Aufruf: install-clean.sh <freecad-source> <build-dir> <install-dir>
set -euo pipefail
SRC="$(realpath -m "${1:?freecad-source fehlt}")"
BUILD="$(realpath -m "${2:?build-dir fehlt}")"
INSTALL="$(realpath -m "${3:?install-dir fehlt}")"
PIXI="${PIXI:-/home/maxx/.pixi/bin/pixi}"

# Erst die Liste holen, dann pruefen: "pgrep | grep -q" scheitert unter pipefail (grep -q beendet
# sich beim ersten Treffer, pgrep bekommt SIGPIPE, die Pipeline gilt als fehlgeschlagen) - so lief
# die Installation am 2026-11-01 trotz laufender FreeCAD durch.
running="$(pgrep -af "^${INSTALL}/bin/FreeCAD" | grep -v "kinematic-test" || true)"
if [[ -n "$running" ]]; then
    echo "FEHLER: die Sandbox-FreeCAD laeuft noch - bitte schliessen und erneut installieren:" >&2
    echo "$running" >&2
    exit 1
fi
rm -rf "${INSTALL}.prev"
if [[ -d "$INSTALL" ]]; then
    mv "$INSTALL" "${INSTALL}.prev"
fi
cd "$SRC"
"$PIXI" run --manifest-path "${SRC}/pixi.toml" --frozen cmake --install "$BUILD" \
    > "$(dirname "$SRC")/install.log" 2>&1 || {
    echo "FEHLER beim Installieren - siehe $(dirname "$SRC")/install.log; voriger Stand: ${INSTALL}.prev" >&2
    exit 1
}
echo "Installiert nach ${INSTALL} (voriger Stand: ${INSTALL}.prev)."
