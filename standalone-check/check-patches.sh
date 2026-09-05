#!/usr/bin/env bash
#
# check-patches.sh
#
# Prueft die 6 aktuellen Patches aus patches/ (Stand siehe patches/README.md
# bzw. Projekt-Memory "todo-cmake-patch-apply-target") gegen den aktuellen
# Sandbox-Checkout - rein lesend, schreibt NIE in den eigentlichen
# Arbeitsbaum. Wird von standalone-check/CMakeLists.txt als Custom Target
# "check-patches" aufgerufen (cmake --build standalone-check/build --target
# check-patches).
#
# Die meisten Patches sind unabhaengig und werden per `git apply --check`
# direkt gegen den Arbeitsbaum geprueft (schreibt nichts). Das
# AssemblyLink-Paar (grounded-joint-nested-flex.patch muss VOR
# link-subjoints-revert.patch angewendet werden - Letzterer schliesst eine
# Luecke, die Ersterer hinterlaesst, siehe patches/README.md) kann nicht
# einzeln mit --check geprueft werden (der zweite Patch erwartet den
# bereits gepatchten Zwischenstand als Kontext) - dafuer wird ein
# Wegwerf-Git-Worktree angelegt, dort wirklich angewendet, und danach
# rueckstandslos wieder entfernt.
#
# Aufruf: ./check-patches.sh <SANDBOX_ROOT>

set -euo pipefail

SANDBOX_ROOT="${1:?Aufruf: $0 <SANDBOX_ROOT>}"
PATCHES_DIR="${SANDBOX_ROOT}/patches"
FAIL=0

check_independent() {
    local patch="$1"
    if git -C "$SANDBOX_ROOT" apply --check "${PATCHES_DIR}/${patch}" 2>/tmp/check-patches-err.$$; then
        echo "OK      ${patch}"
    else
        echo "FEHLER  ${patch}  ($(head -1 /tmp/check-patches-err.$$))"
        FAIL=1
    fi
    rm -f /tmp/check-patches-err.$$
}

echo "== Unabhaengige Patches (git apply --check, schreibt nichts) =="
check_independent "freecad-assembly-jointobject.patch"
check_independent "freecad-assembly-addressing-utils.patch"
check_independent "freecad-assembly-viewprovider-null-crash.patch"

echo
echo "== AssemblyLink-Paar (sequenziell, im Wegwerf-Worktree) =="
WORKTREE_DIR="$(mktemp -d)"
cleanup() {
    git -C "$SANDBOX_ROOT" worktree remove --force "$WORKTREE_DIR" >/dev/null 2>&1 || true
    rm -rf "$WORKTREE_DIR"
}
trap cleanup EXIT

git -C "$SANDBOX_ROOT" worktree add --detach --quiet "$WORKTREE_DIR" HEAD

if git -C "$WORKTREE_DIR" apply "${PATCHES_DIR}/freecad-assembly-grounded-joint-nested-flex.patch" 2>/tmp/check-patches-err1.$$; then
    echo "OK      freecad-assembly-grounded-joint-nested-flex.patch"
    if git -C "$WORKTREE_DIR" apply --check "${PATCHES_DIR}/freecad-assembly-link-subjoints-revert.patch" 2>/tmp/check-patches-err2.$$; then
        echo "OK      freecad-assembly-link-subjoints-revert.patch (nach nested-flex)"
    else
        echo "FEHLER  freecad-assembly-link-subjoints-revert.patch  ($(head -1 /tmp/check-patches-err2.$$))"
        FAIL=1
    fi
    rm -f /tmp/check-patches-err2.$$
else
    echo "FEHLER  freecad-assembly-grounded-joint-nested-flex.patch  ($(head -1 /tmp/check-patches-err1.$$))"
    echo "uebersprungen  freecad-assembly-link-subjoints-revert.patch (haengt vom vorigen ab)"
    FAIL=1
fi
rm -f /tmp/check-patches-err1.$$

echo
echo "== Nicht pruefbar in dieser Sandbox =="
echo "uebersprungen  freecad-app-getplacementof-partdesign-feature.patch (betrifft src/App/*, hier nicht ausgecheckt - gegen freecad-source direkt pruefen)"

if [[ $FAIL -ne 0 ]]; then
    echo
    echo "Mindestens ein Patch aus der als aktuell verifizierten Liste passt nicht mehr - siehe FEHLER oben."
    exit 1
fi
