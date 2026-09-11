#!/usr/bin/env bash
#
# check-patches.sh
#
# Prueft die Patches aus patches/PATCHES.txt (EINE Quelle der Wahrheit fuer Liste+Reihenfolge,
# siehe dortige Kommentare) gegen den aktuellen Sandbox-Checkout - rein lesend, schreibt NIE
# in den eigentlichen Arbeitsbaum. Wird von standalone-check/CMakeLists.txt als Custom Target
# "check-patches" aufgerufen (cmake --build standalone-check/build --target check-patches).
#
# Da die Patches teils aufeinander aufbauen (z.B. schliesst ein spaeterer Patch eine Luecke,
# die ein frueherer hinterlaesst), werden sie NICHT unabhaengig gegen den aktuellen Arbeitsbaum
# geprueft, sondern SEQUENZIELL in einem Wegwerf-Git-Worktree angewendet - genau die Reihenfolge
# aus PATCHES.txt, danach der Worktree rueckstandslos wieder entfernt.
#
# Aufruf: ./check-patches.sh <SANDBOX_ROOT>

set -euo pipefail

SANDBOX_ROOT="${1:?Aufruf: $0 <SANDBOX_ROOT>}"
PATCHES_DIR="${SANDBOX_ROOT}/patches"
PATCHES_LIST="${PATCHES_DIR}/PATCHES.txt"

if [[ ! -f "$PATCHES_LIST" ]]; then
    echo "FEHLER: ${PATCHES_LIST} nicht gefunden." >&2
    exit 1
fi

# Liest PATCHES.txt: Leerzeilen und #-Kommentare (auch am Zeilenende) ignorieren.
mapfile -t PATCHES < <(sed -e 's/#.*$//' -e 's/[[:space:]]*$//' "$PATCHES_LIST" | grep -v '^[[:space:]]*$')

echo "== Patches aus PATCHES.txt (sequenziell, im Wegwerf-Worktree) =="
WORKTREE_DIR="$(mktemp -d)"
cleanup() {
    git -C "$SANDBOX_ROOT" worktree remove --force "$WORKTREE_DIR" >/dev/null 2>&1 || true
    rm -rf "$WORKTREE_DIR"
}
trap cleanup EXIT

git -C "$SANDBOX_ROOT" worktree add --detach --quiet "$WORKTREE_DIR" HEAD

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
