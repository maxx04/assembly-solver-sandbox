#!/usr/bin/env bash
#
# update-neon-env.sh - bringt die eigene Qt6/PySide6/Shiboken6-Umgebung (neon-qt6-pyside6/root/)
# auf den aktuellen Stand des KDE-Neon-Repos. FreeCADs eigenes CI installiert ebenfalls immer "was
# gerade aktuell ist" aus diesem Repo (package/ubuntu/install-apt-packages.sh pinnt keine Version) -
# "gleicher Stand wie FreeCAD" heisst deshalb: aktueller Neon-Stand (Nutzerauftrag 2026-10-10:
# woechentliches Update immer komplett, inklusive Neon, als Standardvorgang).
#
# Kein Systemeingriff: dieselbe sudo-freie apt-sandbox/-Konfiguration wie beim urspruenglichen
# Aufbau (siehe Memory reference-neon-qt6-pyside6-environment), nur "apt-get download" +
# "dpkg-deb -x", nie "install".
#
# Ablauf:
#   1. Paketindex aktualisieren (apt-sandbox/).
#   2. Paketliste: packages.txt (beim ersten Lauf aus den bisherigen archives/*.deb erzeugt),
#      ergaenzt um alles, was der apt-Loeser fuer diese Liste gegen das echte System zusaetzlich
#      verlangt (neue Abhaengigkeiten).
#   3. Aktuelle Versionen in einen neuen Download-Ordner laden. Pakete, die nicht ladbar sind
#      (z.B. Ubuntu-Pro/ESM-pflichtig), werden gemeldet und uebersprungen - wie beim Aufbau.
#   4. Alles in ein frisches root.new/ entpacken (kein Ueberlagern alter Dateien), die bekannten
#      Handanpassungen anwenden (PySide6Config.abi3.cmake relativ statt /usr, Symlinks im
#      PySide6-Paket), dann root/ <-> root.new/ tauschen. Der vorige Stand bleibt als root.prev/
#      liegen (genau einer, aelterer wird ersetzt) - Rueckweg: Ordner zuruecktauschen.
#   5. venv-Symlinks (PySide6*, shiboken6*) neu setzen, damit sie auch nach einem
#      Versionssprung (anderer egg-info-Name) stimmen.
#
# Danach ist ein kompletter Neubau noetig (neue Qt-Builds betreffen praktisch alles):
#   cmake --build standalone-check/build --target rebuild-fresh-and-deploy
#
# Aufruf:  ./standalone-check/update-neon-env.sh
# Exit 0 = aktualisiert (oder bereits aktuell), sonst Fehler; der alte Stand bleibt dann aktiv.

set -euo pipefail

DEV_DIR="/home/maxx/Dokumente/FreeCAD-Development"
NEON_DIR="${DEV_DIR}/neon-qt6-pyside6"
APT_SANDBOX="${NEON_DIR}/apt-sandbox"
ROOT="${NEON_DIR}/root"
ROOT_NEW="${NEON_DIR}/root.new"
ROOT_PREV="${NEON_DIR}/root.prev"
ARCHIVES="${APT_SANDBOX}/archives"
DOWNLOAD="${APT_SANDBOX}/archives.new"
ARCHIVES_PREV="${APT_SANDBOX}/archives.prev"
MANIFEST="${NEON_DIR}/packages.txt"
VENV_SITE="${DEV_DIR}/.venv/lib/python3.12/site-packages"

APT_OPTS=(
    -o "Dir::Etc::SourceList=${APT_SANDBOX}/sources.list"
    -o "Dir::Etc::SourceParts=${APT_SANDBOX}/sources.list.d"
    -o "Dir::State::Lists=${APT_SANDBOX}/lists"
    -o "Dir::Cache::Archives=${ARCHIVES}"
    -o "Dir::Cache=${APT_SANDBOX}/cache"
    -o "Dir::Etc::Parts=/dev/null"
)

log() { printf '\n=== %s -- %s ===\n\n' "$(date +%H:%M:%S)" "$*"; }

if [[ ! -d "$APT_SANDBOX" || ! -d "$ROOT" ]]; then
    echo "FEHLER: ${APT_SANDBOX} oder ${ROOT} fehlt - urspruengliches Neon-Setup nicht vorhanden." >&2
    exit 1
fi

log "Neon 1/5: Paketindex aktualisieren"
apt-get "${APT_OPTS[@]}" update -qq

log "Neon 2/5: Paketliste bestimmen"
if [[ ! -f "$MANIFEST" ]]; then
    for deb in "$ARCHIVES"/*.deb; do
        dpkg-deb -f "$deb" Package
    done | sort -u > "$MANIFEST"
    echo "packages.txt aus den bisherigen Downloads erzeugt ($(wc -l < "$MANIFEST") Pakete)."
fi
mapfile -t WANTED < "$MANIFEST"
EXTRA=()
if SIM=$(apt-get "${APT_OPTS[@]}" -s install "${WANTED[@]}" 2>/dev/null); then
    while read -r pkg; do
        if ! grep -qxF "$pkg" "$MANIFEST"; then
            EXTRA+=("$pkg")
        fi
    done < <(awk '/^Inst / {print $2}' <<< "$SIM")
else
    echo "WARNUNG: Abhaengigkeitsaufloesung fehlgeschlagen - lade nur die bekannte Paketliste."
fi
if [[ ${#EXTRA[@]} -gt 0 ]]; then
    echo "Neue Abhaengigkeiten: ${EXTRA[*]}"
fi
ALL=("${WANTED[@]}" "${EXTRA[@]}")
echo "${#ALL[@]} Pakete."

log "Neon 3/5: aktuelle Versionen laden"
rm -rf "$DOWNLOAD"
mkdir -p "$DOWNLOAD"
FAILED=()
(
    cd "$DOWNLOAD"
    for pkg in "${ALL[@]}"; do
        apt-get "${APT_OPTS[@]}" download -qq "$pkg" >/dev/null 2>&1 || echo "$pkg" >> "${DOWNLOAD}/.failed"
    done
)
if [[ -f "${DOWNLOAD}/.failed" ]]; then
    mapfile -t FAILED < "${DOWNLOAD}/.failed"
    rm -f "${DOWNLOAD}/.failed"
    echo "Nicht ladbar (uebersprungen, z.B. Ubuntu-Pro/ESM): ${FAILED[*]}"
fi
NEW_COUNT=$(find "$DOWNLOAD" -maxdepth 1 -name '*.deb' | wc -l)
if [[ "$NEW_COUNT" -eq 0 ]]; then
    echo "FEHLER: nichts geladen - alter Stand bleibt aktiv." >&2
    rm -rf "$DOWNLOAD"
    exit 1
fi

CHANGED=$(comm -3 <(cd "$ARCHIVES" && ls -1 ./*.deb 2>/dev/null | sort) \
                  <(cd "$DOWNLOAD" && ls -1 ./*.deb | sort) | sed 's/^\s*//' || true)
if [[ -z "$CHANGED" ]]; then
    echo "Alle ${NEW_COUNT} Pakete unveraendert - Neon-Umgebung ist bereits aktuell."
    rm -rf "$DOWNLOAD"
    exit 0
fi
echo "Geaenderte Pakete (alt/neu):"
echo "$CHANGED" | sed 's/^/  /'

log "Neon 4/5: frisch entpacken, Handanpassungen anwenden, tauschen"
rm -rf "$ROOT_NEW"
mkdir -p "$ROOT_NEW"
for deb in "$DOWNLOAD"/*.deb; do
    dpkg-deb -x "$deb" "$ROOT_NEW"
done

# (a) PySide6Config.abi3.cmake: die Neon-Pakete tragen feste /usr/...-Pfade, alle anderen
#     Eintraege dort rechnen relativ zu PACKAGE_PREFIX_DIR - ohne diese Korrektur findet CMake
#     PySide6 nur im System-/usr, nicht in unserer Kopie.
PYSIDE_CFG="${ROOT_NEW}/usr/lib/x86_64-linux-gnu/cmake/PySide6/PySide6Config.abi3.cmake"
if [[ -f "$PYSIDE_CFG" ]]; then
    sed -i \
        -e 's#"/usr/lib/python3/dist-packages/PySide6"#"${PACKAGE_PREFIX_DIR}/python3/dist-packages/PySide6"#' \
        -e 's#"/usr/share/PySide6/typesystems"#"${PACKAGE_PREFIX_DIR}/../share/PySide6/typesystems"#' \
        -e 's#"/usr/share/PySide6/glue"#"${PACKAGE_PREFIX_DIR}/../share/PySide6/glue"#' \
        "$PYSIDE_CFG"
    if grep -qE 'set_and_check\(PYSIDE_[A-Z]+ "/usr/' "$PYSIDE_CFG"; then
        echo "FEHLER: ${PYSIDE_CFG} enthaelt weiterhin feste /usr-Pfade (Paketformat geaendert?)." >&2
        echo "        root.new/ bleibt zur Pruefung liegen, alter Stand ist weiter aktiv." >&2
        exit 1
    fi
else
    echo "FEHLER: ${PYSIDE_CFG} fehlt im neuen Stand - root.new/ bleibt zur Pruefung liegen." >&2
    exit 1
fi

# (b) Symlinks im PySide6-Paket auf Header und Bibliotheken (FreeCADs Shiboken-Bindungen suchen
#     sie dort). Ziel ist der ENDGUELTIGE root/-Pfad, nicht root.new/.
PYSIDE_PKG="${ROOT_NEW}/usr/lib/python3/dist-packages/PySide6"
ln -sfn "${ROOT}/usr/include/PySide6" "${PYSIDE_PKG}/include"
for lib in "${ROOT_NEW}"/usr/lib/x86_64-linux-gnu/libpyside6*.abi3.so.*; do
    name=$(basename "$lib")
    [[ "$name" =~ \.so\.[0-9]+\.[0-9]+$ ]] || continue
    ln -sfn "${ROOT}/usr/lib/x86_64-linux-gnu/${name}" "${PYSIDE_PKG}/${name}"
done

rm -rf "$ROOT_PREV" "$ARCHIVES_PREV"
mv "$ROOT" "$ROOT_PREV"
mv "$ROOT_NEW" "$ROOT"
mv "$ARCHIVES" "$ARCHIVES_PREV"
mv "$DOWNLOAD" "$ARCHIVES"
printf '%s\n' "${ALL[@]}" | sort -u > "$MANIFEST"
echo "Neuer Stand aktiv. Vorheriger Stand: ${ROOT_PREV} (Archive: ${ARCHIVES_PREV})."

log "Neon 5/5: venv-Symlinks neu setzen"
for old in "$VENV_SITE"/PySide6* "$VENV_SITE"/shiboken6*; do
    [[ -L "$old" ]] && rm -f "$old"
done
for target in "$ROOT"/usr/lib/python3/dist-packages/PySide6* "$ROOT"/usr/lib/python3/dist-packages/shiboken6*; do
    [[ -e "$target" ]] || continue
    ln -sfn "$target" "${VENV_SITE}/$(basename "$target")"
done
ls -1 "$VENV_SITE" | grep -iE '^(pyside6|shiboken6)' | sed 's/^/  /'

echo ""
echo "Fertig. Jetzt komplett neu bauen:"
echo "  cmake --build standalone-check/build --target rebuild-fresh-and-deploy"
