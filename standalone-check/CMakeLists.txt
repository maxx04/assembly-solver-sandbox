# Standalone-Compile-Check fuer diese Sandbox
# ============================================
#
# KEIN Teil von FreeCAD selbst (kein SPDX-Header wie die echten Mod/*-
# CMakeLists.txt) - reines Hilfsmittel dieser Sandbox, um die hier bearbeiteten
# .cpp-Dateien OHNE einen vollen FreeCAD-Build gegen die echten Header/Typen zu
# kompilieren (Syntax-/Typ-Check des aktuellen Sandbox-Arbeitsstands).
#
# Warum kein echtes "add_library(Assembly SHARED ...)" mit Linking wie im
# echten src/Mod/Assembly/App/CMakeLists.txt: Assembly haengt zur Laufzeit an
# Part/PartDesign/Spreadsheet/FreeCADApp/OndselSolver, die wiederum an OCCT/
# Qt6/Xerces/Boost/Coin3D/... haengen - nichts davon ist in dieser Sandbox
# ausgecheckt (siehe ../SANDBOX_NOTES.md, Abschnitt "Zweck": "kein
# eigenstaendig baubares FreeCAD"). Statt das nachzubilden, werden hier nur
# OBJECT-Bibliotheken gebaut (compile-only, kein Link-Schritt) - das reicht,
# um echte Compiler-Fehler (Syntax, Typen, fehlende Includes, falsche
# Signaturen) an Ort und Stelle zu finden, ohne freecad-source anzufassen.
#
# Voraussetzung: ein bereits konfigurierter/gebauter Klon von
# github.com/FreeCAD/FreeCAD unter FREECAD_SOURCE_DIR (Default: der auf
# dieser Maschine ohnehin vorhandene /home/maxx/freecad/freecad-source, siehe
# ../SANDBOX_NOTES.md) - von dort werden NUR generierte Header
# (*_autogen/include, .pyi-generierte PyImp-Header) und System-Include-Pfade
# wiederverwendet, keine .cpp/.h-Quelldateien. Alle Flags unten sind 1:1 aus
# dessen build/compile_commands.json fuer AssemblyObject.cpp bzw.
# ViewProviderAssembly.cpp uebernommen (Stand 2026-09-04) - bei einem
# `update-and-rebuild-freecad.sh`-Lauf drueben koennen sich neue Upstream-
# Header-Aenderungen ergeben, dann ggf. hier neu abgleichen.
#
# Nutzung:
#   cmake -S standalone-check -B standalone-check/build
#   cmake --build standalone-check/build
# (baut nur Objektdateien unter standalone-check/build/, nichts wird
# installiert oder verlinkt; beliebig oft wiederholbar/loeschbar.)
#
# Optional andere Pfade:
#   cmake -S standalone-check -B standalone-check/build \
#         -DFREECAD_SOURCE_DIR=/pfad/zu/freecad-source \
#         -DFREECAD_BUILD_DIR=/pfad/zu/freecad-source/build

cmake_minimum_required(VERSION 3.16)
project(assembly_sandbox_check CXX)

set(FREECAD_SOURCE_DIR "/home/maxx/freecad/freecad-source" CACHE PATH
    "Checkout von github.com/FreeCAD/FreeCAD, dessen generierte Header/System-Includes wiederverwendet werden")
set(FREECAD_BUILD_DIR "${FREECAD_SOURCE_DIR}/build" CACHE PATH
    "Konfigurierter Build-Ordner von FREECAD_SOURCE_DIR (fuer *_autogen/include, config.h etc.)")

if(NOT EXISTS "${FREECAD_BUILD_DIR}/compile_commands.json")
    message(FATAL_ERROR
        "FREECAD_BUILD_DIR (${FREECAD_BUILD_DIR}) sieht nicht wie ein konfigurierter "
        "FreeCAD-Build aus (compile_commands.json fehlt). Siehe ../SANDBOX_NOTES.md - "
        "dieser Check braucht den echten freecad-source-Build, nicht nur diese Sandbox.")
endif()

get_filename_component(SANDBOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Gemeinsame Defines/Includes, wortgleich aus compile_commands.json fuer
# AssemblyObject.cpp bzw. ViewProviderAssembly.cpp (App- und Gui-Ziel
# unterscheiden sich u.a. durch QT_*_LIB-Defines und die Gui/Qt-Widgets-Includes).
set(COMMON_DEFINES
    BOOST_ATOMIC_DYN_LINK BOOST_ATOMIC_NO_LIB
    BOOST_DATE_TIME_DYN_LINK BOOST_DATE_TIME_NO_LIB
    BOOST_PROGRAM_OPTIONS_DYN_LINK BOOST_PROGRAM_OPTIONS_NO_LIB
    BOOST_REGEX_DYN_LINK BOOST_REGEX_NO_LIB
    BOOST_THREAD_DYN_LINK BOOST_THREAD_NO_LIB
    FMT_SHARED HAVE_CONFIG_H PYCXX_6_2_COMPATIBILITY
    QT_CONCURRENT_LIB QT_CORE_LIB QT_NO_DEBUG QT_NO_KEYWORDS QT_XML_LIB
    _OCC64 NDEBUG
)

set(COMMON_INCLUDES
    # WICHTIG: SANDBOX_ROOT/src steht bewusst VOR FREECAD_SOURCE_DIR/src. Die von
    # generate_from_py() erzeugten *Py.h-Header (z.B. AssemblyObjectPy.h) includen
    # per spitzer Klammer "<Mod/Assembly/App/AssemblyObject.h>" (Suchpfad-basiert,
    # nicht pfadrelativ zur includierenden Datei) - ohne diese Reihenfolge wuerde
    # das den ECHTEN freecad-source-Header treffen, waehrend die .cpp-Datei selbst
    # per Anfuehrungszeichen-Include ("AssemblyObject.h") bereits den Sandbox-Header
    # gezogen hat -> doppelte Definition derselben Klasse (live beobachtet).
    "${SANDBOX_ROOT}/src"
    "${FREECAD_BUILD_DIR}"
    "${FREECAD_BUILD_DIR}/src"
    "${FREECAD_SOURCE_DIR}/src"
    "${FREECAD_SOURCE_DIR}/src/3rdParty/OndselSolver"
    "${FREECAD_SOURCE_DIR}/src/3rdParty/FastSignals/libfastsignals/include"
)
set(COMMON_SYSTEM_INCLUDES
    /usr/include/opencascade
    /usr/include/freetype2
    /usr/include/x86_64-linux-gnu/qt6/QtCore
    /usr/include/x86_64-linux-gnu/qt6
    /usr/include/x86_64-linux-gnu/qt6/QtXml
    "${FREECAD_SOURCE_DIR}/src/3rdParty/PyCXX"
    /usr/include/python3.12
    "${FREECAD_SOURCE_DIR}/src/3rdParty"
    /usr/lib/x86_64-linux-gnu/qt6/mkspecs/linux-g++
    /usr/include/x86_64-linux-gnu/qt6/QtConcurrent
    /usr/include/harfbuzz
    /usr/include/hdf5/serial
    /usr/lib/x86_64-linux-gnu/openmpi/include
    /usr/lib/x86_64-linux-gnu/openmpi/include/openmpi
)

set(COMMON_COMPILE_OPTIONS -Wall -Wextra -Wno-write-strings -O3)

# --- App-Ziel: AssemblyObject.cpp, AssemblyLink.cpp, AssemblyUtils.cpp -----
add_library(assembly_app_check OBJECT
    "${SANDBOX_ROOT}/src/Mod/Assembly/App/AssemblyObject.cpp"
    "${SANDBOX_ROOT}/src/Mod/Assembly/App/AssemblyLink.cpp"
    "${SANDBOX_ROOT}/src/Mod/Assembly/App/AssemblyUtils.cpp"
)
target_compile_features(assembly_app_check PRIVATE cxx_std_20)
target_compile_options(assembly_app_check PRIVATE ${COMMON_COMPILE_OPTIONS})
target_compile_definitions(assembly_app_check PRIVATE
    Assembly_EXPORTS ${COMMON_DEFINES}
)
target_include_directories(assembly_app_check PRIVATE
    "${FREECAD_BUILD_DIR}/src/Mod/Assembly/App/Assembly_autogen/include"
    "${FREECAD_BUILD_DIR}/src/Mod/Assembly/App"
    ${COMMON_INCLUDES}
)
target_include_directories(assembly_app_check SYSTEM PRIVATE ${COMMON_SYSTEM_INCLUDES})

# --- Gui-Ziel: ViewProviderAssembly.cpp ------------------------------------
add_library(assembly_gui_check OBJECT
    "${SANDBOX_ROOT}/src/Mod/Assembly/Gui/ViewProviderAssembly.cpp"
)
target_compile_features(assembly_gui_check PRIVATE cxx_std_20)
target_compile_options(assembly_gui_check PRIVATE ${COMMON_COMPILE_OPTIONS})
target_compile_definitions(assembly_gui_check PRIVATE
    AssemblyGui_EXPORTS ${COMMON_DEFINES}
    QT_GUI_LIB QT_NETWORK_LIB QT_OPENGLWIDGETS_LIB QT_OPENGL_LIB
    QT_PRINTSUPPORT_LIB QT_SVGWIDGETS_LIB QT_SVG_LIB QT_UITOOLS_LIB QT_WIDGETS_LIB
)
target_include_directories(assembly_gui_check PRIVATE
    "${FREECAD_BUILD_DIR}/src/Mod/Assembly/Gui/AssemblyGui_autogen/include"
    "${FREECAD_BUILD_DIR}/src/Mod/Assembly/Gui"
    "${FREECAD_SOURCE_DIR}/src/Gui"
    "${FREECAD_SOURCE_DIR}/src/Gui/Quarter"
    "${FREECAD_SOURCE_DIR}/src/Gui/PreferencePages"
    "${FREECAD_SOURCE_DIR}/src/Gui/Selection"
    "${FREECAD_SOURCE_DIR}/src/Gui/StyleParameters"
    "${FREECAD_BUILD_DIR}/src/Gui"
    "${FREECAD_BUILD_DIR}/src/Gui/Language"
    "${FREECAD_BUILD_DIR}/src/Gui/propertyeditor"
    "${FREECAD_BUILD_DIR}/src/Gui/TaskView"
    "${FREECAD_BUILD_DIR}/src/Gui/Quarter"
    "${FREECAD_BUILD_DIR}/src/Gui/DAGView"
    ${COMMON_INCLUDES}
)
target_include_directories(assembly_gui_check SYSTEM PRIVATE
    ${COMMON_SYSTEM_INCLUDES}
    /usr/include/x86_64-linux-gnu/qt6/QtWidgets
    /usr/include/x86_64-linux-gnu/qt6/QtOpenGL
    /usr/include/x86_64-linux-gnu/qt6/QtOpenGLWidgets
    /usr/include/x86_64-linux-gnu/qt6/QtPrintSupport
    /usr/include/x86_64-linux-gnu/qt6/QtSvg
    /usr/include/x86_64-linux-gnu/qt6/QtSvgWidgets
    /usr/include/x86_64-linux-gnu/qt6/QtNetwork
    /usr/include/x86_64-linux-gnu/qt6/QtUiTools
    /usr/include/x86_64-linux-gnu/qt6/QtGui
)

# Bequemer Sammel-Target fuer "baue alles, was diese Sandbox aktuell aendert".
add_custom_target(check ALL DEPENDS assembly_app_check assembly_gui_check)

# --- Patch-Check: pruft ../patches/*.patch gegen den aktuellen Checkout ----
# NICHT Teil von ALL (laeuft nicht bei jedem normalen Build mit, u.a. weil es
# einen Git-Worktree anlegt/entfernt - separat aufrufen:
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
