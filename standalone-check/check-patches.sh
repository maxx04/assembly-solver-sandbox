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

# Nutzerauftrag 2026-09-27 ("wer prueft, dass alle Commits im Patch landen?"): VANILLA_BASE aus
# PATCHES.txt lesen - der Commit, gegen den die Patches ihren Diff bilden. Ohne diese Zeile
# (aeltere PATCHES.txt-Version) wird die Vollstaendigkeitspruefung weiter unten uebersprungen,
# alles andere bleibt unveraendert funktionsfaehig.
VANILLA_BASE="$(grep -oP '^#\s*VANILLA_BASE:\s*\K\S+' "$PATCHES_LIST" || true)"

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

# Nutzerauftrag 2026-09-27: reines "git apply klappt syntaktisch" (oben) beweist NICHT, dass
# PATCHES.txt auch inhaltlich alle Commits seit VANILLA_BASE erfasst - ein Commit, der schlicht
# vergessen wurde, in den Patch nachzutragen, waere hier unsichtbar geblieben. Deshalb zusaetzlich
# das TATSAECHLICHE Ergebnis (Worktree nach allen Patches) byte-genau gegen den aktuellen
# committeten Sandbox-Stand vergleichen - das ist der eigentliche Beweis.
echo
if [[ -z "$VANILLA_BASE" ]]; then
    echo "== Vollstaendigkeitspruefung uebersprungen (kein 'VANILLA_BASE:'-Eintrag in PATCHES.txt) =="
else
    echo "== Vollstaendigkeitspruefung: Ergebnis vs. aktueller committeter Sandbox-Stand =="
    DIRTY="$(git -C "$SANDBOX_ROOT" status --porcelain -- src/Mod/Assembly src/3rdParty/OndselSolver)"
    if [[ -n "$DIRTY" ]]; then
        echo "UEBERSPRUNGEN: uncommittete Aenderungen an src/Mod/Assembly oder" >&2
        echo "src/3rdParty/OndselSolver - erst committen, dann erneut pruefen:" >&2
        echo "$DIRTY" >&2
    else
        # -x .git/__pycache__: git-fremde Laufzeitartefakte (kompilierter Python-Bytecode-Cache
        # aus Testlaeufen, nie getrackt) - existieren nur im echten Sandbox-Baum, nie im frischen
        # Worktree, waeren sonst ein Fehlalarm ohne jeden Bezug zu fehlenden Commits.
        COMPLETENESS_DIFF="$(diff -rq -x .git -x __pycache__ \
            "${WORKTREE_DIR}/src/Mod/Assembly" "${SANDBOX_ROOT}/src/Mod/Assembly" 2>&1 || true)"
        COMPLETENESS_DIFF+=$'\n'"$(diff -rq -x .git -x __pycache__ \
            "${WORKTREE_DIR}/src/3rdParty/OndselSolver" "${SANDBOX_ROOT}/src/3rdParty/OndselSolver" 2>&1 || true)"
        COMPLETENESS_DIFF="$(echo "$COMPLETENESS_DIFF" | grep -v '^[[:space:]]*$' || true)"
        if [[ -n "$COMPLETENESS_DIFF" ]]; then
            echo "FEHLER: Patches ergeben NICHT denselben Stand wie der aktuelle Commit -" >&2
            echo "PATCHES.txt ist unvollstaendig/veraltet (ein Commit fehlt vermutlich):" >&2
            echo "$COMPLETENESS_DIFF" >&2
            echo >&2
            echo "Neu generieren mit: git diff ${VANILLA_BASE} -- src/Mod/Assembly src/3rdParty/OndselSolver > patches/<datei>.patch" >&2
            exit 1
        fi
        echo "OK - Patches ergeben exakt den aktuellen committeten Stand (VANILLA_BASE=${VANILLA_BASE})."
    fi
fi
