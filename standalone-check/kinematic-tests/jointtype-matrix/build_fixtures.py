"""
build_fixtures.py - baut die festen Ausgangsdateien fuer jointtype_matrix (alle FALL x KONTEXT,
oder per JTM_ONLY="fall__kontext,..." eine Auswahl) unter fixtures/<fall>__<kontext>/.

Aufruf:
    ./common/run-test.sh /home/maxx/freecad-sandbox/install jointtype-matrix/build_fixtures.py
"""
import os
import shutil
import sys

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import jointtype_matrix as jtm  # noqa: E402

FIXTURES = os.path.join(THIS_DIR, "fixtures")


def selected():
    only = [s for s in os.environ.get("JTM_ONLY", "").split(",") if s]
    pairs = [(c, k) for c in jtm.all_cases() for k in jtm.CONTEXTS]
    return [p for p in pairs if not only or jtm.case_id(*p) in only]


def main():
    failed = []
    for case, context in selected():
        cid = jtm.case_id(case, context)
        directory = os.path.join(FIXTURES, cid)
        if os.path.isdir(directory):
            shutil.rmtree(directory)
        try:
            jtm.build_fixture(case, context, directory)
            print(f"GEBAUT {cid}")
        except Exception as exc:  # noqa: BLE001 - jeder Fall soll berichtet werden
            failed.append(cid)
            print(f"FEHLER {cid}: {exc!r}")
        for name in list(App.listDocuments()):
            App.closeDocument(name)
        for leftover in os.listdir(directory) if os.path.isdir(directory) else []:
            if leftover.endswith(".FCBak"):
                os.remove(os.path.join(directory, leftover))
    if failed:
        print(f"RESULT: FAIL ({len(failed)} Fixtur(en) nicht gebaut: {', '.join(failed)})")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
