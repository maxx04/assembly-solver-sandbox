#!/usr/bin/env bash
#
# build-appimage.sh - packt den aktuellen Sandbox-Stand als Linux-AppImage zum Weitergeben.
# Nutzerauftrag 2026-11-05 ("cmake ziel appimage zum weitergeben"). Aufgerufen vom CMake-Ziel
# `appimage` (standalone-check/CMakeLists.txt).
#
# Vorbild ist FreeCADs eigener Weg fuer die Weekly-AppImage (.github/workflows/build_release.yml):
# package/bundle/build.sh baut mit dem Preset conda-linux-release und RELATIVE_RPATH=ON und
# installiert DIREKT in die Pixi-Umgebung, package/bundle/linux/create_bundle.sh kopiert dann die
# ganze Umgebung nach AppDir/usr, wirft Ballast raus und ruft appimagetool auf.
#
# Abweichungen (bewusst):
#   - Installiert wird NICHT in die Pixi-Umgebung (das ist unsere Bau-Umgebung fuer die normale
#     Sandbox-Installation), sondern in einen Staging-Ordner. Dorthin wird vorher eine KOPIE der
#     Umgebung gelegt. Eigener Build-Ordner (Release statt RelWithDebInfo), die normale
#     Installation /home/maxx/freecad-sandbox/install bleibt unberuehrt.
#   - INSTALL_TO_SITEPACKAGES=OFF: sonst landet der freecad-Python-Namensraum ueber den absoluten
#     purelib-Pfad in der echten Pixi-Umgebung statt im Staging-Ordner (FreeCAD findet ihn dann
#     unter <prefix>/Ext).
#   - FREECAD_WARN_ERROR=OFF (conda-release schaltet es ein): Warnungen sollen den Bau eines
#     Weitergabe-Pakets nicht abbrechen.
#   - Eigenes Benutzerprofil: AppRun setzt FREECAD_USER_HOME auf
#     ${XDG_DATA_HOME:-~/.local/share}/FreeCAD-Sandbox, damit sich die Sandbox weder mit einer
#     normalen FreeCAD-Installation noch mit der Weekly-AppImage ein Profil teilt.
#   - Kein Update-Kanal (zsync), kein Hochladen.
#
# Aufruf: build-appimage.sh <freecad-source> <jobs> [--fresh]
#   --fresh: Build-Ordner der AppImage vorher leeren (nach einem Vanilla-Update sinnvoll).
# Ergebnis: /home/maxx/freecad-sandbox/appimage/FreeCAD_sandbox-<datum>-<rev>-Linux-x86_64.AppImage

set -euo pipefail

SRC="$(realpath -m "${1:?freecad-source fehlt}")"
JOBS="${2:?jobs fehlt}"
FRESH="${3:-}"
PIXI="${PIXI:-/home/maxx/.pixi/bin/pixi}"
ENV_DIR="${SRC}/.pixi/envs/default"
WORK="${APPIMAGE_WORK:-/home/maxx/freecad-sandbox/appimage}"
BUILD="${APPIMAGE_BUILD:-/home/maxx/freecad-sandbox/appimage-build}"
APPDIR="${WORK}/AppDir"
USR="${APPDIR}/usr"
ARCH="$(uname -m)"
SANDBOX_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

pixi_run() {
    "$PIXI" run --manifest-path "${SRC}/pixi.toml" --frozen \
        env CFLAGS= CXXFLAGS= DEBUG_CFLAGS= DEBUG_CXXFLAGS= "$@"
}
log() { printf '\n=== %s -- %s ===\n\n' "$(date +%H:%M:%S)" "$*"; }

mkdir -p "$WORK"

log "1/7: Pixi-Umgebung nach pixi.lock"
(cd "$SRC" && "$PIXI" install --locked)

log "2/7: konfigurieren (Release, relative RPATHs, Ziel ${USR})"
if [[ "$FRESH" == "--fresh" ]]; then
    rm -rf "$BUILD"
fi
(cd "$SRC" && pixi_run cmake --preset conda-linux-release -B "$BUILD" \
    -DCMAKE_INSTALL_PREFIX="$USR" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DRELATIVE_RPATH=ON \
    -DINSTALL_TO_SITEPACKAGES=OFF \
    -DBUILD_DYNAMIC_LINK_PYTHON=OFF \
    -DFREECAD_WARN_ERROR=OFF \
    -DFREECAD_VERSION_INCLUDE_GIT=ON) > "${WORK}/configure.log" 2>&1 || {
    echo "FEHLER beim Konfigurieren - siehe ${WORK}/configure.log" >&2
    tail -20 "${WORK}/configure.log" >&2
    exit 1
}
echo "Konfiguriert."

log "3/7: bauen (${JOBS} Jobs)"
start=$(date +%s)
(cd "$SRC" && pixi_run cmake --build "$BUILD" --parallel "$JOBS")
echo "Gebaut in $(( ($(date +%s) - start) / 60 )) min."

log "4/7: AppDir aus Umgebungskopie + Installation zusammenstellen"
rm -rf "$APPDIR"
mkdir -p "$APPDIR"
cp -a "${SRC}/package/bundle/linux/AppDir/." "$APPDIR/"
cp -a "${ENV_DIR}" "$USR"
# freecad-Namensraum der normalen Sandbox-Installation (INSTALL_TO_SITEPACKAGES=ON dort) - hier
# kommt er aus <prefix>/Ext, eine zweite Kopie in site-packages wuerde ihn verdecken.
rm -rf "$USR"/lib/python3*/site-packages/freecad
(cd "$SRC" && pixi_run cmake --install "$BUILD") > "${WORK}/install.log" 2>&1 || {
    echo "FEHLER beim Installieren - siehe ${WORK}/install.log" >&2
    tail -20 "${WORK}/install.log" >&2
    exit 1
}
mv "$USR/bin/FreeCAD" "$USR/bin/freecad"
mv "$USR/bin/FreeCADCmd" "$USR/bin/freecadcmd"

log "5/7: Ballast entfernen (Liste wie package/bundle/linux/create_bundle.sh)"
rm -rf "$USR/include"
find "$USR" -name '*.a' -delete
mv "$USR/bin" "$USR/bin_tmp"
mkdir "$USR/bin"
for b in freecad freecadcmd ccx python pip pyside6-rcc gmsh dot unflatten; do
    if [[ -e "$USR/bin_tmp/$b" ]]; then cp -a "$USR/bin_tmp/$b" "$USR/bin/"; fi
done
rm -rf "$USR/bin_tmp"
"$USR/bin/dot" -c 2>/dev/null || true
sed -i '1s|.*|#!/usr/bin/env python|' "$USR/bin/pip"
find "$APPDIR" -path '*/__pycache__/*' -delete
find "$APPDIR" -name '*.pyc' -type f -delete
for p in conda conda-meta doc etc/conda lib/clang lib/cmake lib/gcc lib/libarcher.so.bak \
         'lib/libasan.*' 'lib/libhwasan.*' 'lib/libgmock*.*' 'lib/libgtest.*' 'lib/liblsan.*' \
         'lib/libtsan.*' 'lib/libubsan.*' 'lib/libnode.so.*' 'lib/libclang-cpp.so.*' \
         'lib/libclang.so.*' 'lib/libLLVM-*.so' 'lib/libLLVM.so.*' 'lib/libdrm*.so*' \
         lib/node_modules lib/objects-Release lib/perl5 \
         'lib/python*/site-packages/conda' 'lib/python*/site-packages/distlib' \
         'lib/python*/site-packages/pandas/tests' 'lib/python*/site-packages/pre_commit' \
         'lib/python*/site-packages/pycparser' 'lib/python*/site-packages/pyright*' \
         'lib/python*/site-packages/qtpy/tests' 'lib/python*/site-packages/tests' \
         'lib/python*/site-packages/tornado' 'lib/python*/site-packages/zstandard' \
         lib/qt6/bin libexec/gcc libexec/git-core man share/cmake 'share/cmake-*' share/emacs \
         share/git-core share/git-gui share/gitk share/gitweb share/gtk-doc share/info share/man \
         share/swig share/vim aarch64-conda-linux-gnu x86_64-conda-linux-gnu; do
    # shellcheck disable=SC2086
    (cd "$USR" && rm -rf $p)
done
find "$APPDIR" -name '*.h' -type f -delete
find "$APPDIR" -name '*.cmake' -type f -delete

cp "$USR/share/applications/org.freecad.FreeCAD.desktop" "$APPDIR/"
sed -i -e 's/Exec=FreeCAD/Exec=AppRun/g' \
       -e 's/^Name=.*/Name=FreeCAD Sandbox (Assembly-Solver)/' "$APPDIR/org.freecad.FreeCAD.desktop"
cp "$USR/share/icons/hicolor/scalable/apps/org.freecad.FreeCAD.svg" "$APPDIR/"
cp "${SRC}/src/Gui/Icons/launch-error.svg" "$APPDIR/"
"$PIXI" list --manifest-path "${SRC}/pixi.toml" -e default > "$APPDIR/packages.txt" 2>/dev/null || true
if command -v msgfmt >/dev/null 2>&1; then
    for po in "$APPDIR"/locale/*/AppRun.po; do
        d="$(dirname "$po")/LC_MESSAGES"; mkdir -p "$d"; msgfmt -o "$d/AppRun.mo" "$po"
    done
else
    echo "Hinweis: msgfmt fehlt - Startfehler-Meldungen der AppImage nur auf Englisch."
fi

# Eigenes Profil statt ~/.config/FreeCAD. FREECAD_USER_HOME wird von FreeCAD stillschweigend
# verworfen, wenn das Verzeichnis nicht existiert - daher mkdir -p.
python3 - "$APPDIR/AppRun" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
block = '''# FreeCAD-Sandbox: eigenes Benutzerprofil (nicht mit normalem FreeCAD teilen).
# Ueberschreibbar per FREECAD_USER_HOME.
if [ -z "${FREECAD_USER_HOME:-}" ]; then
    export FREECAD_USER_HOME="${XDG_DATA_HOME:-${HOME}/.local/share}/FreeCAD-Sandbox"
fi
mkdir -p "${FREECAD_USER_HOME}"
unset SESSION_MANAGER

'''
marker = 'exec "${MAIN}" "$@"'
assert marker in s, "AppRun: exec-Zeile nicht gefunden"
s = s.replace(marker, block + marker)
open(p, "w").write(s)
EOF
chmod a+x "$APPDIR/AppRun"

log "6/7: Selbsttests im AppDir (ohne Pixi-Umgebung)"
# wie create_bundle.sh, aber in einer leeren Umgebung, damit nichts aus Pixi/System mitspielt
clean_env=(env -i HOME="$HOME" PATH=/usr/bin:/bin LANG=C.UTF-8 FREECAD_USER_HOME="${WORK}/test-profile")
mkdir -p "${WORK}/test-profile"
"${clean_env[@]}" "$APPDIR/AppRun" freecadcmd --safe-mode --version
"${clean_env[@]}" "$APPDIR/AppRun" freecadcmd --safe-mode --console \
    "import pivy; from pivy import coin; print('Pivy/Coin', coin.SoDB.getVersion())"
"${clean_env[@]}" "$APPDIR/AppRun" freecadcmd --safe-mode --console \
    "import freecad, FreeCAD, Part, AssemblyApp; print('Module OK', FreeCAD.Version()[0:3])"
# keine Bibliothek darf auf den Bau-Rechner zeigen
leaks="$(grep -rlI --include='*.so*' "/home/maxx/freecad-sandbox/install" "$USR/lib" "$USR/Mod" 2>/dev/null | head -5 || true)"
if [[ -n "$leaks" ]]; then
    echo "WARNUNG: absolute Pfade der normalen Sandbox-Installation in:"; echo "$leaks"
fi

log "7/7: AppImage packen"
TOOL="${WORK}/appimagetool-${ARCH}.AppImage"
if [[ ! -x "$TOOL" ]]; then
    curl -fL -o "$TOOL" \
        "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${ARCH}.AppImage"
    chmod a+x "$TOOL"
fi
REV="$(git -C "$SANDBOX_REPO" rev-parse --short=10 HEAD)"
NAME="FreeCAD_sandbox-$(date +%Y.%m.%d)-${REV}-Linux-${ARCH}"
rm -f "${WORK}/${NAME}.AppImage"
VERSION="sandbox-$(date +%Y.%m.%d)-${REV}" ARCH="$ARCH" "$TOOL" --appimage-extract-and-run \
    --comp zstd --mksquashfs-opt -Xcompression-level --mksquashfs-opt 22 \
    "$APPDIR" "${WORK}/${NAME}.AppImage"
ls -lh "${WORK}/${NAME}.AppImage"
echo "Fertig: ${WORK}/${NAME}.AppImage"
