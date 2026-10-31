import os, re, sys, tempfile
import FreeCAD as App
KT = "/home/maxx/Dokumente/FreeCAD-Development/assembly-solver-sandbox/standalone-check/kinematic-tests"
sys.path.insert(0, os.path.join(KT, "common"))
import joint_test_utils as jtu

def main():
    tmp = tempfile.mkdtemp(prefix="asmtexp")
    out = os.path.join(tmp, "t.FCStd")
    for test in ("test_fixed_joint_flat", "test_revolute_joint_flat"):
        fixture = jtu.fixture_path(os.path.join(KT, test))
        jtu.copy_fixture_to_output(fixture, out)
        doc = App.openDocument(out)
        asm = doc.getObject("Assembly")
        doc.recompute(); asm.solve()
        p = os.path.join(tmp, test + ".asmt")
        asm.exportAsASMT(p)
        lines = open(p).read().split("\n")
        types = [lines[i - 1].strip() for i, l in enumerate(lines) if l.strip() == "Name" and i + 2 < len(lines) and lines[i + 2].strip() == "MarkerI"]
        series = sorted(set(l.split("\t")[0] for l in lines if "Series\t" in l))
        print(f"EXPORT {test}: Gelenk-Typzeilen {types}; Serien {series[:4]}")
        App.closeDocument(doc.Name)
    print("RESULT: PASS")

if __name__ == "__main__":
    main()
