#!/usr/bin/env bash
#
# run-all-tests.sh - fuehrt ALLE Kinematik-Testfaelle (test_*/test_*.py, je ein eigener
# Unterordner pro Testfall seit der Ordner-Umstrukturierung 2026-09-09) nacheinander gegen eine
# gegebene FreeCAD-Installation aus, per run-test.sh. Reihenfolge: alphabetisch nach Ordnername -
# solange Testordner konsequent nach Schema "test_<szenario>_flat/" vor "test_<szenario>_nested*/"
# benannt werden, entspricht das automatisch "flach vor verschachtelt" (Nutzerauftrag
# 2026-09-06/07).
#
# Aufruf:
#   ./common/run-all-tests.sh <freecad-install-dir> [Xvfb-Display, default :95]
#
# Beispiel:
#   ./common/run-all-tests.sh /home/maxx/freecad-sandbox/install
#   ./common/run-all-tests.sh /home/maxx/freecad-sandbox/install-clean-test   # vanilla-Vergleich
#
# Exit-Code: 0 nur wenn ALLE Tests PASS, sonst 1 (fuer Skript-Verkettung/CI). Ein einzelner
# fehlgeschlagener Test bricht NICHT die restlichen ab (bewusst kein "set -e" ueber die
# Testschleife), damit eine vollstaendige Zusammenfassung entsteht statt beim ersten Fehler
# stehenzubleiben.

set -uo pipefail

INSTALL_DIR="${1:?Aufruf: $0 <freecad-install-dir> [display]}"
DISPLAY_NUM="${2:-:95}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# kinematic-tests/-Wurzel = Elternverzeichnis von common/ - seit der Ordner-Umstrukturierung
# 2026-09-09 liegt jeder Testfall in seinem EIGENEN Unterordner dieser Wurzel
# (test_<jointtyp>/test_<jointtyp>.py), nicht mehr direkt neben run-all-tests.sh.
KINEMATIC_TESTS_ROOT="$(dirname "$SCRIPT_DIR")"

mapfile -t TEST_SCRIPTS < <(cd "$KINEMATIC_TESTS_ROOT" && ls */test_*.py 2>/dev/null | sort)

if [[ ${#TEST_SCRIPTS[@]} -eq 0 ]]; then
    echo "FEHLER: keine test_*.py Skripte unter ${KINEMATIC_TESTS_ROOT}/*/ gefunden" >&2
    exit 2
fi

declare -a RESULTS=()
OVERALL_RC=0

for test_script in "${TEST_SCRIPTS[@]}"; do
    echo
    echo "############################################################"
    echo "# ${test_script}"
    echo "############################################################"
    if "${SCRIPT_DIR}/run-test.sh" "$INSTALL_DIR" "$test_script" "$DISPLAY_NUM"; then
        RESULTS+=("OK    ${test_script}")
    else
        RESULTS+=("FEHLER ${test_script}")
        OVERALL_RC=1
    fi
done

echo
echo "=== Zusammenfassung (${INSTALL_DIR}) ==="
printf '%s\n' "${RESULTS[@]}"

exit "$OVERALL_RC"
