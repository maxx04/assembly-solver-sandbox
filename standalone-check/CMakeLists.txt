# Sandbox-Hilfswerkzeuge (Build+Install der Aenderungen, Patch-Check, Kinematik-Tests)
# =======================================================================================
#
# KEIN Teil von FreeCAD selbst - reine Hilfsmittel dieser Sandbox.
#
# Nutzerauftrag (2026-09-06): JEDE Aenderung an Assembly-/OndselSolver-Dateien in dieser
# Sandbox soll ueber ein CMake-Target HIER IM REPO gebaut+installiert werden - siehe Target
# "build-and-deploy" unten. Alternativ (gleichwertig, dasselbe Ziel) ueber VS Codes
# CMake-Tools-Extension direkt (siehe ../.vscode/settings.json, cmake.sourceDirectory zeigt
# auf /home/maxx/freecad-sandbox/freecad-source, wo src/Mod/Assembly + src/3rdParty/
# OndselSolver Symlinks auf genau diese Sandbox sind - Build dort aendert/testet also direkt
# diese Dateien, kein Kopierschritt).
#
# Patch-Check und Kinematik-Tests sind DAVON UNABHAENGIGE Zwecke, bewusst weiterhin als
# .sh-Skripte (kein Zwang, ALLES in CMake nachzubilden - Nutzerklarstellung 2026-09-06):
#   - check-patches: prueft patches/*.patch gegen den aktuellen Sandbox-Checkout
#   - kinematic-test-*: autonome Xvfb-Tests (analytisch vs. vanilla vs. gepatcht)
#
# Nutzung:
#   cmake -S standalone-check -B standalone-check/build
#   cmake --build standalone-check/build --target build-and-deploy
#   cmake --build standalone-check/build --target check-patches
#   cmake --build standalone-check/build --target kinematic-test-patched

cmake_minimum_required(VERSION 3.16)
project(assembly_sandbox_tools NONE)

get_filename_component(SANDBOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# --- Build+Install der Sandbox-Aenderungen (Assembly + OndselSolver + Python-Skripte) -----
# /home/maxx/freecad-sandbox/freecad-source/src/Mod/Assembly (+ src/3rdParty/OndselSolver)
# sind Symlinks auf diese Sandbox - Make erkennt selbst, dass Coin3D/Part/Gui/etc.
# unveraendert sind (Sekunden statt Stunden, verifiziert). VIER Targets noetig, nicht nur
# "Assembly":
# - "Assembly"/"OndselSolver": kompilieren+linken das App-seitige C++ (AssemblyApp.so)
# - "AssemblyGui": kompiliert+linkt das Gui-seitige C++ (AssemblyGui.so, u.a.
#   ViewProviderAssembly.cpp) - OHNE dieses Target bleibt AssemblyGui.so beim ALTEN ABI
#   stehen, wenn sich eine App-seitige Funktionssignatur aendert (z.B. getJoints() bekam
#   durch einen Patch zwei zusaetzliche Parameter) -> "undefined symbol"-Crash beim naechsten
#   Start, weil AssemblyGui.so noch die alte (2-Parameter-)Version der Funktion sucht, die es
#   in der neuen AssemblyApp.so nicht mehr gibt. Live als echter Crash aufgetreten, als nach
#   dem Anwenden mehrerer Patches nur "Assembly OndselSolver" neu gebaut wurde.
# - "AssemblyTests": kopiert die .py-Dateien (JointObject.py, CommandSolveAssembly.py,
#   UtilsAssembly.py, ...) - NICHT "AssemblyScripts" trotz des irrefuehrenden Namens (siehe
#   src/Mod/Assembly/CMakeLists.txt: fc_copy_sources() fuer die eigentlichen Assembly_Scripts
#   haengt dort dran) - live verifiziert: nur "Assembly OndselSolver" zu bauen liess eine
#   .py-Aenderung unbemerkt liegen.
#
# KEIN deploy-so.sh-Umweg noetig wie im alten /home/maxx/freecad/-Workflow: dort entstand die
# RUNPATH-Gefahr durch `cp -a` zwischen verschiedenen Installationsordnern. Hier installiert
# `cmake --install` direkt in das eigene, dafuer konfigurierte install/-Verzeichnis
# (CMAKE_INSTALL_PREFIX) - keine Kopie zwischen zwei Orten, keine RUNPATH-Problematik.
set(FREECAD_SANDBOX_BUILD_DIR "/home/maxx/freecad-sandbox/freecad-source/build" CACHE PATH
    "Build-Ordner des eigenstaendigen freecad-source-Klons (Symlinks auf diese Sandbox)")

add_custom_target(build-and-deploy
    COMMAND ${CMAKE_COMMAND} --build "${FREECAD_SANDBOX_BUILD_DIR}"
            --target Assembly OndselSolver AssemblyGui AssemblyTests
    COMMAND ${CMAKE_COMMAND} --install "${FREECAD_SANDBOX_BUILD_DIR}"
    COMMENT "Baut Assembly+OndselSolver+AssemblyGui+Python-Skripte neu und installiert nach /home/maxx/freecad-sandbox/install"
    VERBATIM
    USES_TERMINAL
)

# --- Patch-Check: prueft ../patches/*.patch gegen den aktuellen Checkout ----
# NICHT Teil von ALL (legt/entfernt einen Git-Worktree) - separat aufrufen:
#   cmake --build standalone-check/build --target check-patches
# Siehe check-patches.sh fuer Details/Begruendung der aktuellen 6-Patch-Liste
# (Rest wurde als ueberholt/TEMP identifiziert und geloescht, siehe
# patches/README.md bzw. Projekt-Memory "todo-cmake-patch-apply-target").
add_custom_target(check-patches
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/check-patches.sh" "${SANDBOX_ROOT}"
    WORKING_DIRECTORY "${SANDBOX_ROOT}"
    COMMENT "Pruefe patches/*.patch gegen den aktuellen Sandbox-Checkout (schreibt nichts im Hauptarbeitsbaum)"
    VERBATIM
    USES_TERMINAL
)

# --- Kinematik-Testsuite: analytisch vs. vanilla-FreeCAD vs. gepatchter Stand -------------
# Autonome Xvfb-Tests, siehe kinematic-tests/README.md. NICHT Teil von ALL (startet einen
# eigenen FreeCAD-Gui-Prozess pro Installation, braucht Xvfb). Zwei separate Targets, damit
# gezielt einzeln geprueft werden kann, ob eine Aenderung vanilla-Verhalten beeinflusst hat.
#
# Beide Targets laufen ueber run-all-tests.sh und fuehren damit die GESAMTE Testmatrix aus
# (alle test_*.py in kinematic-tests/, aktuell: Fixed/Revolute/Cylindrical/Slider/Ball, flach -
# siehe README.md "Aktuelle Tests"), nicht mehr nur test_fixed_joint_flat.py wie zu Beginn.
# Einen einzelnen Testfall gezielt debuggen: run-test.sh direkt aufrufen (siehe dessen eigene
# Kommentare) statt ueber dieses Target.
set(KINEMATIC_TEST_INSTALL_PATCHED "/home/maxx/freecad-sandbox/install" CACHE PATH
    "Zu testende gepatchte FreeCAD-Installation")
set(KINEMATIC_TEST_INSTALL_VANILLA "/home/maxx/freecad-sandbox/install-clean-test" CACHE PATH
    "Unmodifizierte FreeCAD-Installation als Vergleichsreferenz")

add_custom_target(kinematic-test-patched
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/kinematic-tests/common/run-all-tests.sh"
            "${KINEMATIC_TEST_INSTALL_PATCHED}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/kinematic-tests"
    COMMENT "Kinematik-Testmatrix (alle test_*.py) gegen gepatchten Stand (${KINEMATIC_TEST_INSTALL_PATCHED})"
    VERBATIM
    USES_TERMINAL
)
add_custom_target(kinematic-test-vanilla
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/kinematic-tests/common/run-all-tests.sh"
            "${KINEMATIC_TEST_INSTALL_VANILLA}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/kinematic-tests"
    COMMENT "Kinematik-Testmatrix (alle test_*.py) gegen unmodifiziertes FreeCAD (${KINEMATIC_TEST_INSTALL_VANILLA})"
    VERBATIM
    USES_TERMINAL
)
