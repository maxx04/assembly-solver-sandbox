#!/usr/bin/env bash
# Baut OndselSolver (mit den Sandbox-Patches, so wie sie im Submodul liegen) eigenstaendig und
# fuehrt die Solver-Tests in diesem Ordner aus. Ohne FreeCAD, ohne Xvfb.
#
#   ./run-solver-tests.sh [build-verzeichnis]
#
# Exit-Code 0 = alle Tests bestanden.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SANDBOX_ROOT="$(cd "${HERE}/../.." && pwd)"
SOLVER_DIR="${SANDBOX_ROOT}/src/3rdParty/OndselSolver"
BUILD_DIR="${1:-${HERE}/build}"

mkdir -p "${BUILD_DIR}"
cmake -S "${SOLVER_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "${BUILD_DIR}" -j"$(nproc)" >/dev/null

for src in "${HERE}"/test_*.cpp; do
    name="$(basename "${src}" .cpp)"
    g++ -std=c++20 -O2 -I"${SOLVER_DIR}/OndselSolver" "${src}" \
        -L"${BUILD_DIR}/OndselSolver" -lOndselSolver -Wl,-rpath,"${BUILD_DIR}/OndselSolver" \
        -o "${BUILD_DIR}/${name}"
    echo "=== ${name}"
    # Solver-Protokoll ("MbD: ...", "Time = ...") ausblenden, nur Testergebnisse zeigen.
    "${BUILD_DIR}/${name}" | grep -vE '^(MbD: |Time = )'
    [ "${PIPESTATUS[0]}" -eq 0 ]
done
