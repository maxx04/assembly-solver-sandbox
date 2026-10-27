"""
run_matrix.py - fuehrt jointtype_matrix aus: je FALL x KONTEXT Fixtur kopieren, oeffnen, loesen,
Zwangsbedingung pruefen (Paar-Gelenke) bzw. ziehen und Uebersetzung pruefen (Mechanismen),
aeussere Verankerung und unveraenderte Originale pruefen. Gibt eine Tabelle aus.

Nicht Teil von run-all-tests.sh (kein test_-Praefix): in Phase 1 erst die Bestandsaufnahme
gegen Vanilla, danach entscheidet sich, welche Faelle in die Standard-Matrix wandern.

Aufruf:
    ./common/run-test.sh <freecad-install-dir|AppImage> jointtype-matrix/run_matrix.py
Auswahl: JTM_ONLY="parallel__flat,gears__nested" (Komma-Liste von FALL__KONTEXT).
Mechanismen: JTM_MODE=sim treibt per Simulation statt per Ziehen an (Vanilla immer Simulation).
Gibt am Ende "RESULT: PASS" (alle bestanden) oder "RESULT: FAIL ..." aus.
"""
import os
import shutil
import sys
import tempfile
import traceback

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, THIS_DIR)
sys.path.insert(0, os.path.join(THIS_DIR, "..", "common"))
import jointtype_matrix as jtm  # noqa: E402
import nested_test_utils as ntu  # noqa: E402

FIXTURES = os.path.join(THIS_DIR, "fixtures")


def run_case(case, context, workdir):
    cid = jtm.case_id(case, context)
    src = os.path.join(FIXTURES, cid)
    if not os.path.isdir(src):
        return "FEHLT", ["Fixtur fehlt - build_fixtures.py laufen lassen"]
    dst = os.path.join(workdir, cid)
    shutil.copytree(src, dst)
    msgs = []
    top = App.openDocument(os.path.join(dst, "top.FCStd"))
    # Mechanismen: Ziehen (preDrag, nur Sandbox) oder Simulation (auch Vanilla). JTM_MODE=sim
    # erzwingt die Simulation; ohne preDrag in Python (Vanilla) wird immer simuliert.
    ntu.remember_originals(top)
    jtm.remember_joint_originals(top)
    top_asm = [o for o in top.Objects if o.TypeId == "Assembly::AssemblyObject"][0]
    mode = os.environ.get("JTM_MODE", "drag")
    if not hasattr(top_asm, "preDrag"):
        mode = "sim"
    top.recompute()
    rc = top_asm.solve()
    top.recompute()
    ok = rc == 0
    msgs.append(f"solve rc={rc}")
    worlds = jtm.world_index(top_asm)
    good, outer = jtm.check_outer(top_asm, context, worlds)
    ok &= good
    msgs += outer
    for name, container in jtm.content_instances(top_asm, context):
        if case in jtm.PAIR_CASES:
            good, text = jtm.check_pair(case, container, worlds)
        else:
            if mode == "sim":
                good, text = jtm.simulate_mechanism(case, top_asm, container)
            else:
                good, text = jtm.drive_mechanism(case, top_asm, container)
            worlds = jtm.world_index(top_asm)
        ok &= good
        msgs.append(f"{name}: {text} -> {'OK' if good else 'FEHLER'}")
    if context != "flat":
        good = ntu.originals_unchanged(cid)
        ok &= good
        msgs.append(f"Originale unveraendert: {'ja' if good else 'NEIN'}")
        good, changed = jtm.joint_originals_unchanged()
        ok &= good
        msgs.append(f"Gelenk-Originale unveraendert: {'ja' if good else 'NEIN ' + ', '.join(changed)}")
    return ("OK" if ok else "FEHLER"), msgs


def main():
    only = [s for s in os.environ.get("JTM_ONLY", "").split(",") if s]
    pairs = [(c, k) for c in jtm.all_cases() for k in jtm.CONTEXTS]
    pairs = [p for p in pairs if not only or jtm.case_id(*p) in only]
    results = []
    with tempfile.TemporaryDirectory() as workdir:
        for case, context in pairs:
            cid = jtm.case_id(case, context)
            try:
                status, msgs = run_case(case, context, workdir)
            except Exception as exc:  # noqa: BLE001
                status, msgs = "AUSNAHME", [repr(exc), traceback.format_exc().splitlines()[-1]]
            for name in list(App.listDocuments()):
                App.closeDocument(name)
            results.append((cid, status))
            print(f"=== {cid}: {status}")
            for m in msgs:
                print(f"    {m}")
    print("")
    print("JTM-TABELLE")
    width = max(len(c) for c in jtm.all_cases())
    print("JTM " + "fall".ljust(width) + " | " + " | ".join(k.ljust(9) for k in jtm.CONTEXTS))
    status_of = dict(results)
    for case in jtm.all_cases():
        cells = [status_of.get(jtm.case_id(case, k), "-").ljust(9) for k in jtm.CONTEXTS]
        print("JTM " + case.ljust(width) + " | " + " | ".join(cells))
    bad = [c for c, s in results if s != "OK"]
    if bad:
        print(f"RESULT: FAIL ({len(bad)} von {len(results)} nicht OK)")
        return 1
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
