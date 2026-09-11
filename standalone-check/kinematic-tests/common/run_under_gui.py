"""
Runner-Wrapper fuer die Kinematik-Testfaelle unter voller FreeCAD-Gui (Xvfb-Offscreen).

Notwendig, weil Assembly/JointObject.py PySide (Qt) unconditional importiert - das
funktioniert nur, wenn die Gui-Subsystem initialisiert ist (--console/-c verhindert das
komplett, auch mit gesetztem DISPLAY). Muss deshalb als normales Startup-Argument an die
volle FreeCAD-Binary (nicht FreeCADCmd) uebergeben werden, siehe
standalone-check/kinematic-tests/run-test.sh.

Erwartet den Pfad zum eigentlichen Testfall-Skript als erstes Kommandozeilenargument
(nach FreeCADs eigenen). Das Testfall-Skript muss eine Funktion main() -> int (0=PASS)
bereitstellen.
"""
import os
import runpy
import sys

import FreeCAD as App


def main():
    # Ueber Umgebungsvariable statt argv, da FreeCAD beim Start nur EIN .py-Argument als
    # auszufuehrendes Skript entgegennimmt - ein zweites wuerde als zu oeffnendes Dokument
    # fehlinterpretiert.
    test_path = os.environ.get("KINEMATIC_TEST_PATH")
    if not test_path:
        App.Console.PrintError("run_under_gui.py: kein Testfall-Skript uebergeben\n")
        os._exit(2)

    try:
        ns = runpy.run_path(test_path, run_name="__kinematic_test__")
        rc = ns["main"]()
    except Exception:
        import traceback

        traceback.print_exc()
        rc = 1

    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(rc)


main()
