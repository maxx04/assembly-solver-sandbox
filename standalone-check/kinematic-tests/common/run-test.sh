#!/usr/bin/env bash
#
# run-test.sh - fuehrt einen Kinematik-Testfall (run_under_gui.py + Testfall-Skript) headless
# unter Xvfb gegen eine gegebene FreeCAD-Installation aus. Nicht-interaktiv, fuer autonome
# Nutzung gedacht (siehe Nutzerauftrag 2026-09-06: "generell keine interaktiven Tests
# einplanen, selbststaendig arbeiten").
#
# Notwendig, weil Assembly/JointObject.py PySide (Qt) unconditional importiert - funktioniert
# weder in FreeCADCmd noch in FreeCAD --console (beide initialisieren kein Qt), UND PySide6
# selbst ist im System-Python gar nicht installiert, nur im projekteigenen .venv - deshalb
# venv-Aktivierung + volle FreeCAD-Gui-Binary + Xvfb (X-Server ohne sichtbaren Bildschirm).
#
# Aufruf (vom kinematic-tests/-Wurzelverzeichnis aus, ODER mit vollem/relativem Pfad zu diesem
# Skript - der Testfall-Pfad ist IMMER relativ zur kinematic-tests/-Wurzel, nicht zu diesem
# common/-Ordner):
#   ./common/run-test.sh <freecad-install-dir> <testordner>/<testfall-skript.py> [Xvfb-Display, default :95]
#
# Beispiel:
#   ./common/run-test.sh /home/maxx/freecad-sandbox/install test_fixed_joint_flat/test_fixed_joint_flat.py
#   ./common/run-test.sh /home/maxx/freecad-sandbox/install-clean-test test_fixed_joint_flat/test_fixed_joint_flat.py   # vanilla-Vergleich
#
# Exit-Code = Exit-Code des Testfall-Skripts (0 = PASS, sonst FAIL) - fuer Skript-Verkettung.

set -euo pipefail

INSTALL_DIR="${1:?Aufruf: $0 <freecad-install-dir> <testordner>/<testfall-skript.py> [display]}"
TEST_SCRIPT="${2:?Aufruf: $0 <freecad-install-dir> <testordner>/<testfall-skript.py> [display]}"
DISPLAY_NUM="${3:-:95}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# kinematic-tests/-Wurzel = Elternverzeichnis von common/ (wo dieses Skript liegt) - seit der
# Ordner-Umstrukturierung 2026-09-09 liegt jeder Testfall in seinem EIGENEN Unterordner dieser
# Wurzel, nicht mehr direkt neben run-test.sh.
KINEMATIC_TESTS_ROOT="$(dirname "$SCRIPT_DIR")"
if [[ ! "$TEST_SCRIPT" = /* ]]; then
    TEST_SCRIPT="${KINEMATIC_TESTS_ROOT}/${TEST_SCRIPT}"
fi
if [[ ! -f "$TEST_SCRIPT" ]]; then
    echo "FEHLER: Testfall-Skript nicht gefunden: $TEST_SCRIPT" >&2
    exit 2
fi

FC_BIN="${INSTALL_DIR}/bin/FreeCAD"
if [[ ! -x "$FC_BIN" ]]; then
    echo "FEHLER: keine FreeCAD-Gui-Binary unter ${FC_BIN}" >&2
    exit 2
fi
FC_LIB_DIR="$(cd "$(dirname "$FC_BIN")/../lib" && pwd)"

VENV_DIR="/home/maxx/Dokumente/FreeCAD-Development/.venv"
# shellcheck disable=SC1091
source "${VENV_DIR}/bin/activate"
export VIRTUAL_ENV="${VENV_DIR}"
PYSIDE_QT="${VIRTUAL_ENV}/lib/python3.12/site-packages/PySide6/Qt"
export QT_PLUGIN_PATH="${PYSIDE_QT}/plugins"
# RUNPATH-Fix (eigener lib-Ordner zuerst) - siehe docs/JOURNAL.md "ROOT CAUSE GEFUNDEN UND
# BEHOBEN": jede kopierte FreeCAD-Installation traegt sonst ein fest einprogrammiertes RUNPATH
# auf die urspruengliche Installation.
export LD_LIBRARY_PATH="${FC_LIB_DIR}:${PYSIDE_QT}/lib:/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONNOUSERSITE=1
export PYTHONPATH="${VIRTUAL_ENV}/lib/python3.12/site-packages"
export QT_QPA_PLATFORM=xcb

# Eigenes, isoliertes Xvfb starten falls das gewuenschte Display noch nicht läuft - nie ein
# fremdes/echtes Display kapern.
XVFB_STARTED_HERE=0
if ! xdpyinfo -display "$DISPLAY_NUM" >/dev/null 2>&1; then
    Xvfb "$DISPLAY_NUM" -screen 0 1280x1024x24 >/tmp/xvfb-kinematic-test.log 2>&1 &
    XVFB_PID=$!
    XVFB_STARTED_HERE=1
    # kurz warten, bis der X-Server tatsaechlich Verbindungen annimmt
    for _ in $(seq 1 20); do
        xdpyinfo -display "$DISPLAY_NUM" >/dev/null 2>&1 && break
        sleep 0.2
    done
fi
cleanup() {
    if [[ $XVFB_STARTED_HERE -eq 1 ]]; then
        kill "$XVFB_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Bezeichner OHNE das Wort "install" (Nutzerauftrag 2026-09-09) - diese Sandbox nutzt
# durchgaengig nur genau EIN patched/vanilla-Paar (/home/maxx/freecad-sandbox/install bzw.
# .../install-clean-test, siehe Projekt-Memory "todo-cleanup-freecad-dir"); "clean" im
# Verzeichnisnamen markiert die unmodifizierte Vergleichsinstallation, alles andere gilt als
# die gepatchte Sandbox-Installation - dieselbe Terminologie, die im Rest des Projekts
# (Kommentare, README.md, Projekt-Memory) ohnehin durchgaengig verwendet wird.
case "$(basename "$INSTALL_DIR")" in
    *clean*) INSTALL_TAG="vanilla" ;;
    *)       INSTALL_TAG="patched" ;;
esac

export DISPLAY="$DISPLAY_NUM"
export FREECAD_USER_HOME="/tmp/freecad-kinematic-test-profile-${INSTALL_TAG}"
mkdir -p "$FREECAD_USER_HOME"
export KINEMATIC_TEST_PATH="$TEST_SCRIPT"

# Ergebnis-.FCStd aufheben (Nutzerauftrag 2026-09-06), je Testfall in einem EIGENEN
# Unterordner (Nutzerauftrag 2026-09-07 - gilt fuer alle Testfaelle, auch kuenftige):
# <testordner>/fcstd-output/<testname>_<installationsname>.FCStd - seit der Ordner-
# Umstrukturierung 2026-09-09 direkt IM Testordner selbst (Testcode+Fixture+Ergebnis komplett
# gekapselt), nicht mehr zentral. Der Testname im Dateinamen (Nutzerauftrag 2026-09-09) macht
# die Datei auch OHNE Ordnerkontext eindeutig identifizierbar (z.B. beim Oeffnen aus einer
# Dateiliste); der Installationsname sorgt dafuer, dass ein patched- und ein vanilla-Lauf
# desselben Testfalls sich nicht gegenseitig ueberschreiben und man sie nebeneinander oeffnen
# kann. Mehrdateien-Testfaelle (z.B. verschachtelte Baugruppen mit Sub+GrandTop) haengen bei
# Bedarf selbst ein weiteres Suffix an denselben Pfad an (siehe test_fixed_nested_flex.py's
# _output_paths()).
TEST_NAME="$(basename "$TEST_SCRIPT" .py)"
FCSTD_OUTPUT_DIR="$(dirname "$TEST_SCRIPT")/fcstd-output"
mkdir -p "$FCSTD_OUTPUT_DIR"
export KINEMATIC_TEST_OUTPUT_FCSTD="${FCSTD_OUTPUT_DIR}/${TEST_NAME}_${INSTALL_TAG}.FCStd"

LOG_FILE="/tmp/freecad-kinematic-test-${INSTALL_TAG}-$(basename "$TEST_SCRIPT" .py).log"

# WICHTIG (Lektion aus docs/JOURNAL.md): stdout eines Gui-Prozesses ist unzuverlaessig (print()
# landet ueber die Report-View-Umleitung, nicht garantiert auf dem eigenen stdout) - IMMER das
# echte --log-file auswerten, nie nur die Bash-Ausgabe.
timeout 60 "$FC_BIN" -l --log-file "$LOG_FILE" "${SCRIPT_DIR}/run_under_gui.py" >/dev/null 2>&1 || true

echo "=== ${INSTALL_DIR} / $(basename "$TEST_SCRIPT") ==="
grep "^Msg:" "$LOG_FILE" | sed 's/^Msg: //'

if grep -q "RESULT: PASS" "$LOG_FILE"; then
    exit 0
else
    echo "(voller Log: $LOG_FILE)"
    exit 1
fi
