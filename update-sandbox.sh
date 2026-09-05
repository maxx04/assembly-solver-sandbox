#!/usr/bin/env bash
#
# update-sandbox.sh
#
# Synchronisiert diese Sandbox (Sparse-Checkout von src/Mod/Assembly +
# src/3rdParty/OndselSolver) mit einem neueren Commit von
# github.com/FreeCAD/FreeCAD - siehe SANDBOX_NOTES.md, Abschnitt
# "Sync mit Upstream". Analog zu FCProject/patches/update-and-rebuild-freecad.sh,
# aber OHNE Build-Schritte: diese Sandbox ist bewusst kein eigenstaendig
# baubares FreeCAD (siehe SANDBOX_NOTES.md, Abschnitt "Zweck") - reiner
# Editier-/Diff-Sandkasten. Fuer einen echten Compile-Check der
# Sandbox-Dateien danach: standalone-check/ (Ctrl+Shift+B / Task "sandboxfreecad").
#
# Ablauf:
#   1. Sicherstellen, dass keine unerwarteten lokalen Aenderungen an getrackten
#      Dateien vorliegen - Abbruch statt stillschweigendem Ueberschreiben,
#      genau wie im FCProject-Skript. Bekannte, harmlose Submodul-Dirty-
#      Eintraege (OndselSolver, falls dort gerade experimentiert wird) werden
#      ausgefiltert statt als Fehler behandelt.
#   2. git fetch origin.
#   3. git checkout <Zielcommit> - Default: automatisch der aktuelle HEAD-Commit
#      von /home/maxx/freecad/freecad-source (damit Diffs 1:1 vergleichbar
#      bleiben, siehe SANDBOX_NOTES.md), per Argument ueberschreibbar.
#   4. git submodule update --init -- src/3rdParty/OndselSolver.
#   5. Zusammenfassung: was sich in src/Mod/Assembly geaendert hat, plus
#      Hinweis auf naechste Schritte (Patches pruefen, Compile-Check).
#
# Aufruf:
#   ./update-sandbox.sh                     # Zielcommit = aktueller freecad-source-HEAD
#   ./update-sandbox.sh <commit-hash>       # expliziter Zielcommit
#   ./update-sandbox.sh --dry-run           # nur anzeigen, was passieren wuerde
#   ./update-sandbox.sh --dry-run <commit>  # Kombination (Reihenfolge der Argumente egal)

set -euo pipefail

SANDBOX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FREECAD_SOURCE_DIR="/home/maxx/freecad/freecad-source"

cd "$SANDBOX_DIR"

DRY_RUN=0
TARGET_COMMIT=""
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    *) TARGET_COMMIT="$arg" ;;
  esac
done

log() { printf '\n=== %s -- %s ===\n\n' "$(date +%H:%M:%S)" "$*"; }
run() {
  if [[ $DRY_RUN -eq 1 ]]; then
    printf '[dry-run] %s\n' "$*"
  else
    "$@"
  fi
}

log "Start. Aktueller Sandbox-Stand: $(git log -1 --format='%h %cd %s' --date=short)"

if [[ -z "$TARGET_COMMIT" ]]; then
  if [[ ! -d "$FREECAD_SOURCE_DIR/.git" ]]; then
    echo "FEHLER: kein Zielcommit angegeben und FREECAD_SOURCE_DIR (${FREECAD_SOURCE_DIR})" >&2
    echo "ist kein Git-Repo - entweder freecad-source pruefen oder Zielcommit explizit angeben." >&2
    exit 1
  fi
  TARGET_COMMIT="$(git -C "$FREECAD_SOURCE_DIR" rev-parse HEAD)"
  echo "Kein Zielcommit angegeben - nehme aktuellen HEAD von freecad-source: ${TARGET_COMMIT}"
fi

log "Schritt 1/4: pruefe auf unerwartete lokale Aenderungen an getrackten Dateien"
KNOWN_DIRTY=("src/3rdParty/OndselSolver")
UNEXPECTED=""
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  is_known=0
  for f in "${KNOWN_DIRTY[@]}"; do
    [[ "$line" == *"$f" ]] && { is_known=1; break; }
  done
  [[ $is_known -eq 0 ]] && UNEXPECTED+="${line}"$'\n'
done < <(git status --porcelain --untracked-files=no)
if [[ -n "$UNEXPECTED" ]]; then
  echo "FEHLER: unerwartete lokale Aenderungen an getrackten Dateien, breche ab:" >&2
  printf '%s' "$UNEXPECTED" >&2
  echo "(Erst manuell klaeren/sichern - z.B. git stash oder git checkout -- <Datei> - dann erneut starten.)" >&2
  exit 1
fi

log "Schritt 2/4: git fetch origin"
run git fetch origin

BEFORE="$(git rev-parse HEAD)"
log "Schritt 3/4: git checkout ${TARGET_COMMIT}"
run git checkout "$TARGET_COMMIT"

log "Schritt 4/4: Submodule synchronisieren (OndselSolver)"
run git submodule update --init -- src/3rdParty/OndselSolver

if [[ $DRY_RUN -eq 0 ]]; then
  log "Fertig. Zusammenfassung src/Mod/Assembly (${BEFORE:0:10}..${TARGET_COMMIT:0:10})"
  git diff --stat "$BEFORE" "$TARGET_COMMIT" -- src/Mod/Assembly | tail -5
  echo
  echo "Naechste Schritte:"
  echo "  - Patches pruefen, z.B.: git apply --check patches/freecad-assembly-jointobject.patch"
  echo "  - Compile-Check: cmake --build standalone-check/build  (oder Ctrl+Shift+B)"
else
  log "Dry-Run Ende - keine Aenderung vorgenommen."
fi
