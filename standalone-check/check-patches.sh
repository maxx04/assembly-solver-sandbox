#!/usr/bin/env bash
#
# check-patches.sh
#
# Prueft die Patches aus patches/PATCHES.txt (EINE Quelle der Wahrheit fuer Liste+Reihenfolge,
# siehe dortige Kommentare) gegen ein ECHTES VANILLA-Checkout (Ziel-Commit von
# github.com/FreeCAD/FreeCAD) - rein lesend, schreibt NIE in den eigentlichen Arbeitsbaum.
# Wird von standalone-check/CMakeLists.txt als Custom Target "check-patches" aufgerufen
# (cmake --build standalone-check/build --target check-patches).
#
# WICHTIG (2026-09-23 korrigiert, siehe Memory "fix-check-patches-verify-against-vanilla"):
# Fruehere Version pruefte gegen den Wegwerf-Worktree von HEAD des Sandbox-Branches. Das
# schlaegt IMMER fehl, sobald src/Mod/Assembly ueber normale Commits weiterentwickelt wird
# (was hier laengst der Fall ist - HEAD enthaelt die Patch-Inhalte direkt als committeten
# Quellcode, nicht als separat gehaltene, wiederholt anwendbare Diffs). Ein "git apply" auf
# einen bereits gepatchten Stand schlaegt fehl, weil der "Vorher"-Kontext des Patches nicht
# mehr existiert - das ist kein Zeichen fuer einen kaputten Patch, sondern schlicht ein
# falscher Vergleichspunkt. Die eigentliche Frage, die dieses Skript beantworten soll -
# "koennte update-sandbox.sh die Patches gerade jetzt sauber auf echtes Vanilla anwenden?" -
# braucht deshalb einen Worktree auf dem Ziel-Commit selbst, nicht auf HEAD.
#
# Da die Patches teils aufeinander aufbauen (z.B. schliesst ein spaeterer Patch eine Luecke,
# die ein frueherer hinterlaesst), werden sie NICHT unabhaengig gegen den aktuellen Arbeitsbaum
# geprueft, sondern SEQUENZIELL in einem Wegwerf-Git-Worktree angewendet - genau die Reihenfolge
# aus PATCHES.txt, danach der Worktree rueckstandslos wieder entfernt.
#
# Aufruf: ./check-patches.sh <SANDBOX_ROOT> [ZIEL_COMMIT]
# ZIEL_COMMIT default: aktueller HEAD von /home/maxx/freecad/freecad-source (wie
# update-sandbox.sh es per Default auch waehlt) - explizit ueberschreibbar als 2. Argument.

set -euo pipefail

SANDBOX_ROOT="${1:?Aufruf: $0 <SANDBOX_ROOT> [ziel-commit]}"
PATCHES_DIR="${SANDBOX_ROOT}/patches"
PATCHES_LIST="${PATCHES_DIR}/PATCHES.txt"
FREECAD_SOURCE_DIR="/home/maxx/freecad/freecad-source"

if [[ ! -f "$PATCHES_LIST" ]]; then
    echo "FEHLER: ${PATCHES_LIST} nicht gefunden." >&2
    exit 1
fi

TARGET_COMMIT="${2:-}"
if [[ -z "$TARGET_COMMIT" ]]; then
    if [[ ! -d "$FREECAD_SOURCE_DIR/.git" ]]; then
        echo "FEHLER: kein Ziel-Commit angegeben und ${FREECAD_SOURCE_DIR} ist kein Git-Repo -" >&2
        echo "entweder freecad-source pruefen oder Ziel-Commit explizit als 2. Argument angeben." >&2
        exit 1
    fi
    TARGET_COMMIT="$(git -C "$FREECAD_SOURCE_DIR" rev-parse HEAD)"
fi
echo "Vanilla-Ziel-Commit: ${TARGET_COMMIT}"

if ! git -C "$SANDBOX_ROOT" cat-file -e "${TARGET_COMMIT}^{commit}" 2>/dev/null; then
    echo "Ziel-Commit lokal nicht vorhanden, versuche 'git fetch origin ${TARGET_COMMIT}'..."
    git -C "$SANDBOX_ROOT" fetch origin "$TARGET_COMMIT" || {
        echo "FEHLER: Ziel-Commit ${TARGET_COMMIT} auch nach fetch nicht erreichbar." >&2
        exit 1
    }
fi

# Liest PATCHES.txt: Leerzeilen und #-Kommentare (auch am Zeilenende) ignorieren.
mapfile -t PATCHES < <(sed -e 's/#.*$//' -e 's/[[:space:]]*$//' "$PATCHES_LIST" | grep -v '^[[:space:]]*$')

echo "== Patches aus PATCHES.txt (sequenziell, im Wegwerf-Worktree gegen Vanilla) =="
WORKTREE_DIR="$(mktemp -d)"
cleanup() {
    git -C "$SANDBOX_ROOT" worktree remove --force "$WORKTREE_DIR" >/dev/null 2>&1 || true
    rm -rf "$WORKTREE_DIR"
}
trap cleanup EXIT

git -C "$SANDBOX_ROOT" worktree add --detach --quiet "$WORKTREE_DIR" "$TARGET_COMMIT"
# Vorsorglich, falls ein kuenftiger Patch mal OndselSolver-Dateien beruehrt (aktuell keiner) -
# ohne initialisiertes Submodule waeren dessen Dateien im Worktree leer/nicht vorhanden.
git -C "$WORKTREE_DIR" submodule update --init -- src/3rdParty/OndselSolver >/dev/null 2>&1 || true

FAIL=0
PREV_FAILED=0
for patch in "${PATCHES[@]}"; do
    if [[ $PREV_FAILED -eq 1 ]]; then
        echo "uebersprungen  ${patch}  (haengt vom vorigen fehlgeschlagenen Patch ab)"
        continue
    fi
    if git -C "$WORKTREE_DIR" apply "${PATCHES_DIR}/${patch}" 2>/tmp/check-patches-err.$$; then
        echo "OK      ${patch}"
    else
        echo "FEHLER  ${patch}  ($(head -1 /tmp/check-patches-err.$$))"
        FAIL=1
        PREV_FAILED=1
    fi
    rm -f /tmp/check-patches-err.$$
done

echo
echo "== Nicht Teil von PATCHES.txt (siehe dortige Begruendung) =="
echo "uebersprungen  freecad-app-getplacementof-partdesign-feature.patch (betrifft src/App/*, hier nicht ausgecheckt - gegen freecad-source direkt pruefen)"

if [[ $FAIL -ne 0 ]]; then
    echo
    echo "Mindestens ein Patch aus PATCHES.txt passt nicht mehr - siehe FEHLER oben."
    exit 1
fi
