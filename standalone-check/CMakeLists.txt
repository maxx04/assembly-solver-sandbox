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
# unveraendert sind (Sekunden statt Stunden, verifiziert). FUENF Targets noetig, nicht nur
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
# - "FreeCADMain" (CMake-Targetname des Haupt-Executables - die resultierende Binary heisst
#   "FreeCAD", siehe SET_BIN_DIR(FreeCADMain FreeCAD) in src/Main/CMakeLists.txt): faellt NICHT
#   automatisch als Abhaengigkeit der obigen Targets mit an, wird von denen naemlich nur
#   GELADEN (dlopen), nicht gelinkt. Nach einem groesseren Vanilla-Upstream-Sync (der
#   SURROUNDING Build-Tree wird dabei ebenfalls auf den neuen Commit gebracht, siehe
#   [[reference-vanilla-resync-2026-09-23-methodology]]) aendern sich auch Kernbibliotheken wie
#   libFreeCADBase.so/libFreeCADApp.so (z.B. Funktionssignaturen durch eine
#   std::string_view-Migration) - das bereits gebaute bin/FreeCAD bleibt dann auf dem ALTEN ABI
#   stehen und stuerzt beim Start mit "undefined symbol: ...setOperatingSystemNumericLocale..."
#   ab, obwohl Assembly/AssemblyGui selbst tadellos bauen. Live als echter Absturz aufgetreten
#   (2026-09-23): bin/FreeCAD zuletzt vom 2026-09-06, Kernbibliotheken frisch vom 2026-09-23 -
#   reiner "Assembly AssemblyGui"-Build sieht das Problem gar nicht, weil es nicht in seinem
#   Abhaengigkeitsgraphen liegt. "FreeCADMain" hier immer MIT auffuehren, auch wenn es im
#   Alltag (reine Assembly-Aenderung, kein Vanilla-Sync) fast immer ein No-Op ist (Make erkennt
#   selbst, dass sich nichts geaendert hat, siehe live vermessen: ~5s Mehrkosten fuer den reinen
#   Dependency-Check) - der Preis dafuer, dass ein Vanilla-Sync nicht wieder unbemerkt eine
#   kaputte Installation hinterlaesst.
#
# KEIN deploy-so.sh-Umweg noetig wie im alten /home/maxx/freecad/-Workflow: dort entstand die
# RUNPATH-Gefahr durch `cp -a` zwischen verschiedenen Installationsordnern. Hier installiert
# `cmake --install` direkt in das eigene, dafuer konfigurierte install/-Verzeichnis
# (CMAKE_INSTALL_PREFIX) - keine Kopie zwischen zwei Orten, keine RUNPATH-Problematik.
set(FREECAD_SANDBOX_BUILD_DIR "/home/maxx/freecad-sandbox/freecad-source/build" CACHE PATH
    "Build-Ordner des eigenstaendigen freecad-source-Klons (Symlinks auf diese Sandbox)")

# FCPROJECT-PATCH (2026-11-01, Ubuntu 26.04): Bau-Umgebung ist FreeCADs eigene Pixi-Umgebung
# (conda-forge, wie FreeCADs Pixi-CI und die Weekly-AppImage) statt der frueheren KDE-Neon-Qt6-
# Umgebung. Jeder Bau-/Installationsschritt laeuft ueber `pixi run` im Quellordner des Build-Baums
# (Compiler clang + mold, cmake, Qt 6.11, Python 3.13 aus .pixi/envs/default). CFLAGS & Co. leer wie
# in FreeCADs Pixi-Tasks.
set(SANDBOX_PIXI "/home/maxx/.pixi/bin/pixi" CACHE FILEPATH "pixi (Version wie FreeCADs CI)")
set(SANDBOX_INSTALL_DIR "/home/maxx/freecad-sandbox/install" CACHE PATH "Installationsziel der Sandbox")
set(SANDBOX_PIXI_RUN "${SANDBOX_PIXI}" run --manifest-path "${FREECAD_SANDBOX_BUILD_DIR}/../pixi.toml" --frozen
    env CFLAGS= CXXFLAGS= DEBUG_CFLAGS= DEBUG_CXXFLAGS=)

# FCPROJECT-PATCH (2026-10-04): parallel auf allen Kernen bauen - ohne --parallel lief make
# einzeln (ein Vollbau nach einem Vanilla-Sprung dauerte so Stunden statt ~13-24 Minuten).
cmake_host_system_information(RESULT SANDBOX_BUILD_JOBS QUERY NUMBER_OF_LOGICAL_CORES)

add_custom_target(build-and-deploy
    COMMAND ${SANDBOX_PIXI_RUN}
            cmake --build "${FREECAD_SANDBOX_BUILD_DIR}" --parallel ${SANDBOX_BUILD_JOBS}
            --target Assembly OndselSolver AssemblyGui AssemblyTests FreeCADMain
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/install-clean.sh"
            "${FREECAD_SANDBOX_BUILD_DIR}/.." "${FREECAD_SANDBOX_BUILD_DIR}" "${SANDBOX_INSTALL_DIR}"
    COMMENT "Baut Assembly+OndselSolver+AssemblyGui+Python-Skripte+FreeCAD-Executable neu und installiert nach /home/maxx/freecad-sandbox/install"
    VERBATIM
    USES_TERMINAL
)

# --- Patch-Check: prueft ../patches/*.patch gegen den aktuellen Checkout ----
# NICHT Teil von ALL (legt/entfernt einen Git-Worktree) - separat aufrufen:
#   cmake --build standalone-check/build --target check-patches
# Siehe check-patches.sh fuer Details/Begruendung der aktuellen 6-Patch-Liste
# (Rest wurde als ueberholt/TEMP identifiziert und geloescht, siehe
# patches/README.md bzw. Projekt-Memory "todo-cmake-patch-apply-target").
# FCPROJECT-PATCH (2026-10-04, Vanilla-Resync auf 7d2c4213e9): nach einem Wechsel des Build-Baums
# (update-sandbox.sh Schritt 8, neuer Vanilla-Stand) reicht build-and-deploy NICHT - es baut nur
# Assembly + FreeCADMain mit ihren Abhaengigkeiten, alle anderen Module in der Installation bleiben
# alt und passen nicht mehr zur neuen Basisbibliothek (live: "MeasureGui.so: undefined symbol
# _ZN4Base4Unit7InertiaE"). Dieses Ziel baut und installiert ALLE Module.
add_custom_target(build-all-and-deploy
    COMMAND ${SANDBOX_PIXI_RUN} cmake --build "${FREECAD_SANDBOX_BUILD_DIR}" --parallel ${SANDBOX_BUILD_JOBS}
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/install-clean.sh"
            "${FREECAD_SANDBOX_BUILD_DIR}/.." "${FREECAD_SANDBOX_BUILD_DIR}" "${SANDBOX_INSTALL_DIR}"
    COMMENT "Baut ALLE FreeCAD-Module neu und installiert nach /home/maxx/freecad-sandbox/install (nach einem Vanilla-Wechsel des Build-Baums noetig)"
    VERBATIM
    USES_TERMINAL
)

# FCPROJECT-PATCH (2026-10-10, Nutzerauftrag: woechentliches Update immer komplett, als
# Standardvorgang): nach update-sandbox.sh (Vanilla-Stand + Neon-Qt6/PySide6/Shiboken6) frisch aus
# einem leeren Build-Ordner bauen und installieren. Die bisherigen CMake-Optionen werden dabei
# exakt uebernommen (siehe rebuild-fresh.sh). Bricht vor dem Installieren ab, wenn die
# Sandbox-FreeCAD noch laeuft.
add_custom_target(rebuild-fresh-and-deploy
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/rebuild-fresh.sh"
            "${FREECAD_SANDBOX_BUILD_DIR}/.." "${FREECAD_SANDBOX_BUILD_DIR}"
            "${SANDBOX_INSTALL_DIR}" ${SANDBOX_BUILD_JOBS}
    COMMENT "Kompletter Neubau aus leerem Build-Ordner + Installation (woechentliches Standard-Update)"
    VERBATIM
    USES_TERMINAL
)

# FCPROJECT-PATCH (2026-11-05, Nutzerauftrag "cmake ziel appimage zum weitergeben"): aktuellen
# Sandbox-Stand als Linux-AppImage packen, nach FreeCADs eigenem Weg fuer die Weekly-AppImage
# (package/bundle). Eigener Release-Build-Ordner (/home/maxx/freecad-sandbox/appimage-build) und
# Staging-Ordner - Pixi-Umgebung und normale Installation bleiben unberuehrt. Ergebnis unter
# /home/maxx/freecad-sandbox/appimage/. Details siehe build-appimage.sh.
#   appimage        - inkrementell (erster Lauf = kompletter Bau)
#   appimage-fresh  - Build-Ordner vorher leeren (nach einem Vanilla-Update)
add_custom_target(appimage
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/build-appimage.sh"
            "${FREECAD_SANDBOX_BUILD_DIR}/.." ${SANDBOX_BUILD_JOBS}
    COMMENT "Packt den Sandbox-Stand als AppImage zum Weitergeben (/home/maxx/freecad-sandbox/appimage)"
    VERBATIM
    USES_TERMINAL
)
add_custom_target(appimage-fresh
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/build-appimage.sh"
            "${FREECAD_SANDBOX_BUILD_DIR}/.." ${SANDBOX_BUILD_JOBS} --fresh
    COMMENT "Wie appimage, aber aus leerem Build-Ordner"
    VERBATIM
    USES_TERMINAL
)

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
# FCPROJECT-PATCH (2026-10-10, Nutzerauftrag): Vanilla-Referenz ist die offizielle Weekly-AppImage
# (wird vom woechentlichen Update per update-weekly-appimage.sh mitgeholt); run-test.sh nimmt eine
# AppImage direkt statt eines Installationsordners. Die fruehere, selbst gebaute install-clean-test
# (Stand 29.08.) ist entfernt.
set(KINEMATIC_TEST_INSTALL_VANILLA "/home/maxx/freecad/App_weekly/freecad.App" CACHE FILEPATH
    "Unmodifiziertes FreeCAD als Vergleichsreferenz (Weekly-AppImage)")

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
