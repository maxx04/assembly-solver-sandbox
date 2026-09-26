#!/usr/bin/env bash
#
# check-neon-sync.sh
#
# Prueft, ob unsere eigene, isolierte Qt6/PySide6/Shiboken6-Umgebung
# (neon-qt6-pyside6/, siehe Memory "reference-neon-qt6-pyside6-environment") noch dem
# entspricht, was FreeCADs eigenes CI-Skript (package/ubuntu/install-apt-packages.sh) JETZT aus
# dem KDE-Neon-Repo installieren wuerde. FreeCAD selbst pinnt dort KEINE Version - es fuegt das
# Repo hinzu und installiert "was gerade aktuell ist". "synchron mit FreeCAD" heisst deshalb
# konkret: synchron mit dem aktuellen Stand des Neon-"noble main"-User-Repos.
#
# Rein lesend (apt-get update + apt-cache policy gegen die schon vorhandene, sudo-freie
# apt-sandbox/-Konfiguration aus dem urspruenglichen Setup) - laedt/installiert NICHTS,
# veraendert nichts an neon-qt6-pyside6/root/. Bei Bedarf (neuere Version gefunden) zeigt es nur
# den naechsten manuellen Schritt (siehe Ausgabe am Ende), aktualisiert aber nicht automatisch -
# das war schon beim urspruenglichen Aufbau ein bewusster, mehrstufiger manueller Vorgang
# (apt-get download + dpkg-deb -x + zwei Handkorrekturen an PySide6Config.abi3.cmake).
#
# Aufruf: ./check-neon-sync.sh

set -euo pipefail

NEON_DIR="/home/maxx/Dokumente/FreeCAD-Development/neon-qt6-pyside6"
APT_SANDBOX="${NEON_DIR}/apt-sandbox"
ROOT="${NEON_DIR}/root"

if [[ ! -d "$APT_SANDBOX" ]]; then
    echo "FEHLER: ${APT_SANDBOX} nicht gefunden - urspruengliches Neon-Setup fehlt." >&2
    exit 1
fi

APT_OPTS=(
    -o "Dir::Etc::SourceList=${APT_SANDBOX}/sources.list"
    -o "Dir::Etc::SourceParts=${APT_SANDBOX}/sources.list.d"
    -o "Dir::State::Lists=${APT_SANDBOX}/lists"
    -o "Dir::Cache::Archives=${APT_SANDBOX}/archives"
    -o "Dir::Cache=${APT_SANDBOX}/cache"
    -o "Dir::Etc::Parts=/dev/null"
)

echo "Aktualisiere Paketindex (nur lesend, gegen bestehende sudo-freie apt-sandbox/-Konfiguration)..."
apt-get "${APT_OPTS[@]}" update -qq

# Ein Paket je "Schicht" reicht als Stichprobe - Qt6/PySide6/Shiboken6 werden im Neon-Repo als
# EIN zusammenhaengendes Release gebaut (gleiche Versionsnummer, unterschiedliche Build-Nummern
# je Paket), Drift zwischen ihnen ist praktisch ausgeschlossen.
declare -A PACKAGES=(
    [libqt6core6t64]="Qt6"
    [libpyside6-py3-6.11]="PySide6"
    [shiboken6]="Shiboken6"
)

CURRENT_VERSION="unbekannt"
if compgen -G "${APT_SANDBOX}/archives/shiboken6_*.deb" > /dev/null; then
    CURRENT_VERSION=$(basename "${APT_SANDBOX}"/archives/shiboken6_*.deb | sed -E 's/^shiboken6_(.+)_all\.deb$/\1/')
fi
echo ""
echo "Aktuell entpackte Version (aus dem urspruenglichen Download): ${CURRENT_VERSION}"
echo ""

DRIFT_FOUND=0
for pkg in "${!PACKAGES[@]}"; do
    label="${PACKAGES[$pkg]}"
    CANDIDATE=$(apt-cache "${APT_OPTS[@]}" policy "$pkg" 2>/dev/null | awk '/Installationskandidat:|Candidate:/ {print $2}')
    if [[ -z "$CANDIDATE" ]]; then
        echo "WARNUNG: ${label} (${pkg}) im Neon-Repo nicht gefunden - Repo-Struktur geaendert?"
        continue
    fi
    if [[ "$CANDIDATE" == "$CURRENT_VERSION" ]]; then
        echo "OK       ${label} (${pkg}): ${CANDIDATE} - aktuell, entspricht unserem Stand."
    else
        echo "NEUER    ${label} (${pkg}): Neon-Repo hat ${CANDIDATE}, wir haben ${CURRENT_VERSION}."
        DRIFT_FOUND=1
    fi
done

echo ""
if [[ "$DRIFT_FOUND" -eq 0 ]]; then
    echo "=== Alles synchron - kein Update noetig. ==="
else
    echo "=== Neuere Version im Neon-Repo verfuegbar. ==="
    echo "Manueller Aktualisierungsweg (siehe Memory reference-neon-qt6-pyside6-environment):"
    echo "  1. apt-get ${APT_OPTS[*]} install --simulate <Pakete> pruefen (Abhaengigkeitsaufloesung)"
    echo "  2. apt-get ${APT_OPTS[*]} download <alle betroffenen Pakete>"
    echo "  3. dpkg-deb -x <jedes .deb> ${ROOT}/"
    echo "  4. PySide6Config.abi3.cmake erneut auf PACKAGE_PREFIX_DIR pruefen (hartcodierte Pfade?)"
    echo "  5. Komplett neu bauen (rm -rf build/ + frisches cmake -S -B, kein einzelnes"
    echo "     _autogen-Loeschen - siehe Memory)."
fi
