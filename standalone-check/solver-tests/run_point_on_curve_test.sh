#!/usr/bin/env bash
# Baut und startet point_on_curve_test.cpp gegen die OndselSolver-Bibliothek des Build-Baums.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)/src/3rdParty/OndselSolver"
LIB="${ONDSEL_LIB_DIR:-/home/maxx/freecad-sandbox/freecad-source/build/lib}"
OUT="${TMPDIR:-/tmp}/point_on_curve_test"
g++ -std=c++20 -O1 -I"$SRC" "$HERE/point_on_curve_test.cpp" -L"$LIB" -lOndselSolver -Wl,-rpath,"$LIB" -o "$OUT"
"$OUT"
