#!/usr/bin/env bash
#
# update-sandbox.sh
#
# Synchronisiert diese Sandbox (Sparse-Checkout von src/Mod/Assembly +
# src/3rdParty/OndselSolver) mit einem neueren Commit von
# github.com/FreeCAD/FreeCAD - siehe SANDBOX_NOTES.md, Abschnitt
# "Sync mit Upstream". Wendet danach automatisch alle Patches aus
# patches/PATCHES.txt neu an (Nutzerauftrag 2026-09-07) - Ablauf analog zu
# FCProjects patches/update-and-rebuild-freecad.sh: Patches erst ENTFERNEN
# (sonst wuerde Schritt 1 sie als "unerwartete lokale Aenderungen" werten und
# der Sync bricht ab), dann syncen, dann neu anwenden.
#
# Ablauf:
#   1. Alle von patches/PATCHES.txt betroffenen Dateien auf den zuletzt
#      committeten Stand zuruecksetzen (git checkout --), damit sie den
#      Sync nicht blockieren. Patches selbst bleiben unangetastet (liegen
#      als eigene Dateien in patches/, nicht betroffen).
#   2. Sicherstellen, dass danach keine UNERWARTETEN lokalen Aenderungen an
#      getrackten Dateien mehr vorliegen - Abbruch statt stillschweigendem
#      Ueberschreiben. Bekannte, harmlose Submodul-Dirty-Eintraege
#      (OndselSolver, falls dort gerade experimentiert wird) werden
#      ausgefiltert statt als Fehler behandelt.
#   3. git fetch origin.
#   4. src/Mod/Assembly + src/3rdParty/OndselSolver per PFAD-BESCHRAENKTEM
#      "git checkout <Zielcommit> -- <pfade>" auf den Zielcommit bringen -
#      NIEMALS ein volles "git checkout <Zielcommit>" (siehe Kommentar an
#      der Stelle im Skript - hat live einmal patches/, docs/,
#      standalone-check/ etc. aus dem Arbeitsverzeichnis verschwinden
#      lassen, weil die nur auf unserem Branch existieren, nicht in
#      FreeCADs eigener Historie). HEAD bleibt dabei auf solver-sandbox,
#      kein detached HEAD. Zielcommit-Default: automatisch der aktuelle
#      HEAD-Commit von /home/maxx/freecad/freecad-source (damit Diffs 1:1
#      vergleichbar bleiben, siehe SANDBOX_NOTES.md), per Argument
#      ueberschreibbar.
#   5. git submodule update --init -- src/3rdParty/OndselSolver.
#   6. Alle Patches aus patches/PATCHES.txt in der dort angegebenen
#      Reihenfolge neu anwenden. Bricht bei JEDEM Patch, der nicht mehr
#      passt, sofort mit klarer Meldung ab (welcher Patch, warum) - kein
#      automatisches Ueberspringen/Ignorieren. Nachfolgende Patches werden
#      dann NICHT versucht (koennten vom fehlgeschlagenen abhaengen).
#   7. Zusammenfassung: was sich in src/Mod/Assembly geaendert hat, plus
#      Hinweis auf naechste Schritte (Build+Deploy, Kinematik-Tests).
#   8. standalone-check/check-neon-sync.sh (rein lesend, siehe dortiger
#      Kommentar): prueft nebenbei, ob die eigene Neon-Qt6/PySide6/Shiboken6-
#      Umgebung noch dem aktuellen Stand des Neon-Repos entspricht - FreeCADs
#      eigenes CI-Skript pinnt dort selbst keine Version, "synchron mit
#      FreeCAD" heisst hier "synchron mit dem Neon-Repo". Kein automatisches
#      Update, nur ein Hinweis.
#
# Aufruf:
#   ./update-sandbox.sh                     # Zielcommit = aktueller freecad-source-HEAD
#   ./update-sandbox.sh <commit-hash>       # expliziter Zielcommit
#   ./update-sandbox.sh --dry-run           # nur anzeigen, was passieren wuerde
#   ./update-sandbox.sh --dry-run <commit>  # Kombination (Reihenfolge der Argumente egal)
#   ./update-sandbox.sh --no-patches        # nur syncen, Patches NICHT automatisch anwenden

set -euo pipefail

SANDBOX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FREECAD_SOURCE_DIR="/home/maxx/freecad/freecad-source"
PATCHES_DIR="${SANDBOX_DIR}/patches"
PATCHES_LIST="${PATCHES_DIR}/PATCHES.txt"

cd "$SANDBOX_DIR"

DRY_RUN=0
APPLY_PATCHES=1
TARGET_COMMIT=""
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --no-patches) APPLY_PATCHES=0 ;;
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

# Liest PATCHES.txt: Leerzeilen und #-Kommentare (auch am Zeilenende) ignorieren.
read_patch_list() {
  sed -e 's/#.*$//' -e 's/[[:space:]]*$//' "$PATCHES_LIST" | grep -v '^[[:space:]]*$' || true
}

log "Start. Aktueller Sandbox-Stand: $(git log -1 --format='%h %cd %s' --date=short)"

if [[ ! -f "$PATCHES_LIST" ]]; then
  echo "FEHLER: ${PATCHES_LIST} nicht gefunden." >&2
  exit 1
fi
mapfile -t PATCHES < <(read_patch_list)

if [[ -z "$TARGET_COMMIT" ]]; then
  if [[ ! -d "$FREECAD_SOURCE_DIR/.git" ]]; then
    echo "FEHLER: kein Zielcommit angegeben und FREECAD_SOURCE_DIR (${FREECAD_SOURCE_DIR})" >&2
    echo "ist kein Git-Repo - entweder freecad-source pruefen oder Zielcommit explizit angeben." >&2
    exit 1
  fi
  TARGET_COMMIT="$(git -C "$FREECAD_SOURCE_DIR" rev-parse HEAD)"
  echo "Kein Zielcommit angegeben - nehme aktuellen HEAD von freecad-source: ${TARGET_COMMIT}"
fi

log "Schritt 1/8: von PATCHES.txt betroffene Dateien zuruecksetzen (damit sie den Sync nicht blockieren)"
# Dateiliste dynamisch aus den Patches selbst ableiten (jede "+++ b/<pfad>"-Zeile) statt
# manuell gepflegt - bleibt automatisch aktuell, wenn PATCHES.txt sich aendert.
PATCHED_FILES=()
if [[ ${#PATCHES[@]} -gt 0 ]]; then
  mapfile -t PATCHED_FILES < <(
    for p in "${PATCHES[@]}"; do
      grep -h '^+++ b/' "${PATCHES_DIR}/${p}" 2>/dev/null | sed 's#^+++ b/##'
    done | sort -u
  )
fi
for f in "${PATCHED_FILES[@]}"; do
  # Nur zuruecksetzen, was tatsaechlich hier ausgecheckt ist (z.B. src/App/* aus
  # freecad-app-getplacementof-partdesign-feature.patch ist es nicht).
  [[ -f "$f" ]] && run git checkout -- "$f"
done

log "Schritt 2/8: pruefe auf unerwartete lokale Aenderungen an getrackten Dateien"
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
if [[ -n "$UNEXPECTED" && $DRY_RUN -eq 0 ]]; then
  echo "FEHLER: unerwartete lokale Aenderungen an getrackten Dateien, breche ab:" >&2
  printf '%s' "$UNEXPECTED" >&2
  echo "(Erst manuell klaeren/sichern - z.B. git stash oder git checkout -- <Datei> - dann erneut starten.)" >&2
  exit 1
fi

log "Schritt 3/8: git fetch origin"
run git fetch origin

BEFORE="$(git rev-parse HEAD)"
log "Schritt 4/8: src/Mod/Assembly + src/3rdParty/OndselSolver auf ${TARGET_COMMIT} bringen"
# WICHTIG (live als echter Vorfall aufgetreten, 2026-09-07): NIEMALS ein volles
# "git checkout <upstream-commit>" hier - das ersetzt den KOMPLETTEN Arbeitsbaum durch den
# Tree dieses reinen Upstream-Commits, der patches/, docs/, standalone-check/, resources/,
# .vscode/, SANDBOX_NOTES.md etc. gar nicht kennt (die existieren nur auf UNSEREM Branch,
# nicht in FreeCADs eigener Historie) - alle diese Ordner verschwinden dabei sofort aus dem
# Arbeitsverzeichnis (nichts geht in git verloren, der Branch-Pointer bleibt stehen, aber der
# Schreck ist trotzdem unnoetig). Pfad-beschraenkter Checkout stattdessen: aktualisiert NUR
# diese zwei Pfade auf den Stand von TARGET_COMMIT, HEAD bleibt auf dem eigenen Branch,
# nichts sonst wird angefasst.
run git checkout "$TARGET_COMMIT" -- src/Mod/Assembly src/3rdParty/OndselSolver

log "Schritt 5/8: Submodule synchronisieren (OndselSolver)"
run git submodule update --init -- src/3rdParty/OndselSolver

if [[ $APPLY_PATCHES -eq 1 ]]; then
  log "Schritt 6/8: Patches aus PATCHES.txt neu anwenden (${#PATCHES[@]} Stueck)"
  for p in "${PATCHES[@]}"; do
    echo "--- ${p} ---"
    run git apply "${PATCHES_DIR}/${p}"
  done
else
  log "Schritt 6/8: uebersprungen (--no-patches)"
fi

if [[ $DRY_RUN -eq 0 ]]; then
  log "Schritt 7/8: Zusammenfassung src/Mod/Assembly (${BEFORE:0:10}..${TARGET_COMMIT:0:10})"
  git diff --stat "$BEFORE" "$TARGET_COMMIT" -- src/Mod/Assembly | tail -5
  echo
  echo "Naechste Schritte:"
  echo "  - Build+Deploy: cmake --build standalone-check/build --target build-and-deploy  (oder Strg+Shift+B)"
  echo "  - Kinematik-Test: cmake --build standalone-check/build --target kinematic-test-patched"

  log "Schritt 8/8: pruefe Neon-Qt6/PySide6/Shiboken6-Umgebung auf Versionsdrift (rein lesend)"
  "${SANDBOX_DIR}/standalone-check/check-neon-sync.sh" || true
else
  log "Dry-Run Ende - keine Aenderung vorgenommen."
fi
