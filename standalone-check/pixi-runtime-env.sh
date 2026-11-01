# pixi-runtime-env.sh - aktiviert FreeCADs Pixi-Umgebung (conda-forge, Python 3.13, Qt 6.11) fuer
# den Start der Sandbox-FreeCAD. Wird von run-freecad-sandbox.sh und kinematic-tests/common/
# run-test.sh per "source" eingebunden (seit 2026-11-01, Ubuntu 26.04; vorher Neon-Qt6 + .venv).
#
# Gleiche Aktivierung wie `pixi run` (pixi shell-hook: PATH, CONDA_PREFIX, conda-activate.d).
# Die Bibliotheken findet die Installation ueber ihren RPATH (rebuild-fresh.sh), Qt seine Plugins
# ueber den eigenen Praefix in der Umgebung.
PIXI="${PIXI:-/home/maxx/.pixi/bin/pixi}"
SANDBOX_SRC="${SANDBOX_SRC:-/home/maxx/freecad-sandbox/freecad-source}"
if [[ ! -x "$PIXI" || ! -d "${SANDBOX_SRC}/.pixi/envs/default" ]]; then
    echo "FEHLER: Pixi-Umgebung fehlt (${SANDBOX_SRC}/.pixi/envs/default) - zuerst" >&2
    echo "  cmake --build standalone-check/build --target rebuild-fresh-and-deploy" >&2
    return 1 2>/dev/null || exit 1
fi
# Die conda-Aktivierungsskripte (u.a. bash-completion) vertragen kein "set -u"/"set -e" des
# aufrufenden Skripts (run-test.sh) - nur fuer die Aktivierung abschalten.
_pixi_flags="$-"
set +eu
eval "$("$PIXI" shell-hook --manifest-path "${SANDBOX_SRC}/pixi.toml" --frozen)"
[[ "$_pixi_flags" == *e* ]] && set -e
[[ "$_pixi_flags" == *u* ]] && set -u
unset _pixi_flags
unset VIRTUAL_ENV
