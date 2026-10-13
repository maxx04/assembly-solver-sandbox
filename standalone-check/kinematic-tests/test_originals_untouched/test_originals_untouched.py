"""
test_originals_untouched.py - Regressionstest fuer "Solver arbeitet im Kontext einer BG nur auf
deren Spiegeln, nie auf Originalen" (Plan plans/2026-10-03-solver-nur-auf-spiegeln.md).

Anlass (2026-10-03, BG37 mit eingebauter BG43): der Solver las/schrieb die ORIGINALE in den
Unterdokumenten und rechnete deren Weltlage ueber die ECHTE Container-Kette, waehrend die GUI ueber
die Spiegel-Kette des aktiven Dokuments zeichnet. Wichen die Ketten voneinander ab, startete der
Solver weit neben der sichtbaren Lage (BG37: Joint014 59mm/86 Grad) und konvergierte nicht; ausserdem
veraenderte eine aeussere BG die Originale (Nutzerregel: Original darf von aussen nie geaendert
werden).

Laeuft ueber ALLE verschachtelten Fixturen (Fixturen nur gelesen, Kopie mit Original-Dateinamen,
siehe nested_test_utils.py). Prueft nach Recompute + 3 Solves der obersten Baugruppe:
  1. Originale unveraendert: jede Placement in jedem Unterdokument bitgenau wie vorher.
  2. Solver-Sicht erfuellt alle Joints: frischer ASMT-Export (exportAsASMT baut aus den aktuellen
     Placements, ohne zu loesen), Marker-Lage je Joint typgerecht geprueft.
  3. GUI-Sicht == Solver-Sicht: die Weltlage jedes Solver-Koerpers im ASMT stimmt mit der globalen
     Placement desselben Objekts in der GUI ueberein.

Gibt am Ende genau eine Zeile "RESULT: PASS" oder "RESULT: FAIL ..." aus.
"""
import glob
import math
import os
import re
import shutil
import sys
import tempfile

import FreeCAD as App

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
TESTS_ROOT = os.path.dirname(THIS_DIR)
sys.path.insert(0, os.path.join(TESTS_ROOT, "common"))
import nested_test_utils as ntu  # noqa: E402

ntu.allow_duplicate_labels()

FIXTURE_PATTERNS = ("*_nested_flex", "*_double_nested_flex", "*_duplicate_instance_nested",
                    "*_duplicate_instance_flat")
TOL_POS = 1e-5
TOL_ANG = 1e-4  # Grad


def output_root():
    base = os.environ.get("KINEMATIC_TEST_OUTPUT_FCSTD")
    if base:
        return os.path.splitext(base)[0]
    return os.path.join(THIS_DIR, "fcstd-output", "default")


def fixture_dirs():
    dirs = set()
    for pattern in FIXTURE_PATTERNS:
        for d in glob.glob(os.path.join(TESTS_ROOT, pattern)):
            if glob.glob(os.path.join(d, "fixtures", "*_grand.FCStd")):
                dirs.add(d)
    return sorted(dirs)


def assembly_of(doc):
    for obj in doc.Objects:
        if obj.TypeId == "Assembly::AssemblyObject":
            return obj
    return None


def close_all_documents():
    for name in list(App.listDocuments().keys()):
        App.closeDocument(name)


def snapshot_originals(top_doc):
    snap = {}
    for doc in App.listDocuments().values():
        if doc is top_doc:
            continue
        for obj in doc.Objects:
            if "Placement" in obj.PropertiesList:
                snap[f"{doc.Name}#{obj.Name}"] = App.Placement(obj.Placement)
    return snap


# ---------------------------------------------------------------- ASMT-Auswertung
def _vec(line):
    return [float(x) for x in line.split()]


def parse_asmt(path):
    lines = open(path).read().split("\n")
    parts, amarkers, joints = {}, {}, []
    cur = None
    i = 0
    while i < len(lines):
        line = lines[i]
        s = line.strip()
        ind = len(line) - len(line.lstrip("\t"))
        if s == "Part" and ind == 2:
            name = lines[i + 2].strip()
            parts[name] = {"pos": _vec(lines[i + 4]),
                           "rot": [_vec(lines[i + 6]), _vec(lines[i + 7]), _vec(lines[i + 8])],
                           "markers": {}}
            cur = name
            i += 9
            continue
        if s == "Marker":
            mname = lines[i + 2].strip()
            m = (_vec(lines[i + 4]), [_vec(lines[i + 6]), _vec(lines[i + 7]), _vec(lines[i + 8])])
            if ind == 3:
                amarkers[mname] = m
            elif cur:
                parts[cur]["markers"][mname] = m
            i += 9
            continue
        if s in ("Parts", "KinematicIJs"):
            cur = None
        if s == "Name" and ind == 4 and i + 2 < len(lines) and lines[i + 2].strip() == "MarkerI":
            joints.append((lines[i + 1].strip(), lines[i + 3].strip(), lines[i + 5].strip()))
            i += 6
            continue
        i += 1
    return parts, amarkers, joints


def _mv(R, v):
    return [sum(R[r][c] * v[c] for c in range(3)) for r in range(3)]


def _mm(A, B):
    return [[sum(A[r][k] * B[k][c] for k in range(3)) for c in range(3)] for r in range(3)]


def _marker_world(parts, amarkers, path):
    p = path.split("/")[2:]
    if len(p) == 1:
        return amarkers[p[0]]
    part = parts[p[0]]
    mpos, mrot = part["markers"][p[1]]
    pos = [a + b for a, b in zip(part["pos"], _mv(part["rot"], mpos))]
    return pos, _mm(part["rot"], mrot)


def _col(R, c):
    return [R[r][c] for r in range(3)]


def _angle(a, b):
    dot = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
    return math.degrees(math.acos(dot))


def _off_axis(deg):
    """Abweichung von 0 ODER 180 Grad - die exakte 180-Grad-Loesung ist die bekannte, akzeptierte
    Mehrdeutigkeit skriptgebauter Fixturen (Bugreport #32477, reference-fixed-joint-180-degree-
    bug-rootcause), kein Gegenstand dieses Tests."""
    return min(deg, abs(180.0 - deg))


def joint_object_for(solver_name):
    """MbD-Name = nestingPrefix + doc.Name + '#' + obj.Name (jointInstanceName)."""
    if "#" not in solver_name:
        return None
    head, objname = solver_name.rsplit("#", 1)
    docname = head.split(".")[-1]
    doc = App.listDocuments().get(docname)
    return doc.getObject(objname) if doc else None


def check_joints_in_solver_view(asm, tmpdir):
    path = os.path.join(tmpdir, "check.asmt")
    asm.exportAsASMT(path)
    parts, amarkers, joints = parse_asmt(path)
    errors = []
    for name, mi, mj in joints:
        joint = joint_object_for(name)
        if joint is None or not hasattr(joint, "JointType"):
            continue  # Erdungen und Limits
        if getattr(joint, "Suppressed", False):
            continue
        (pi, Ri), (pj, Rj) = _marker_world(parts, amarkers, mi), _marker_world(parts, amarkers, mj)
        d = [b - a for a, b in zip(pi, pj)]
        dist = math.sqrt(sum(x * x for x in d))
        zi, zj = _col(Ri, 2), _col(Rj, 2)
        jt = joint.JointType
        bad = None
        if jt == "Fixed":
            rot = max(_off_axis(_angle(_col(Ri, c), _col(Rj, c))) for c in range(3))
            if dist > TOL_POS or rot > TOL_ANG:
                bad = f"{dist:.4f}mm/{rot:.3f}deg"
        elif jt == "Revolute":
            ang = _off_axis(_angle(zi, zj))
            if dist > TOL_POS or ang > TOL_ANG:
                bad = f"{dist:.4f}mm/z {ang:.3f}deg"
        elif jt == "Ball":
            if dist > TOL_POS:
                bad = f"{dist:.4f}mm"
        elif jt in ("Slider", "Cylindrical"):
            along = sum(x * y for x, y in zip(d, zi))
            lateral = math.sqrt(max(0.0, dist * dist - along * along))
            ang = _off_axis(_angle(zi, zj))
            if lateral > TOL_POS or ang > TOL_ANG:
                bad = f"quer {lateral:.4f}mm/z {ang:.3f}deg"
        if bad:
            errors.append(f"Joint {name} ({jt}) in Solver-Sicht verletzt: {bad}")
    return errors, parts


def check_gui_equals_solver(parts):
    errors = []
    for pname, part in parts.items():
        if "#" not in pname:
            continue
        docname, objname = pname.split("#", 1)
        doc = App.listDocuments().get(docname)
        obj = doc.getObject(objname) if doc else None
        if obj is None or "Placement" not in obj.PropertiesList or obj.TypeId == "App::Origin":
            continue
        gui = App.Placement(obj.Placement)
        parent = obj.getParentGeoFeatureGroup()
        while parent is not None:
            gui = parent.Placement.multiply(gui)
            parent = parent.getParentGeoFeatureGroup()
        dist = (gui.Base - App.Vector(*part["pos"])).Length
        if dist > TOL_POS:
            errors.append(f"Koerper {pname}: GUI-Weltlage weicht {dist:.4f}mm von Solver-Sicht ab")
    return errors


# ---------------------------------------------------------------- Test
def check_fixture(fixture_dir, tmpdir):
    name = os.path.basename(fixture_dir)
    out_dir = os.path.join(output_root(), name)
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    shutil.copytree(os.path.join(fixture_dir, "fixtures"), out_dir)

    top_doc = App.openDocument(glob.glob(os.path.join(out_dir, "*_grand.FCStd"))[0])
    top = assembly_of(top_doc)
    before = snapshot_originals(top_doc)

    top_doc.recompute()
    for _ in range(3):
        top.solve(False)

    errors = []
    after = snapshot_originals(top_doc)
    for key, plc in before.items():
        now = after.get(key)
        if now is None:
            continue
        if not now.isSame(plc, 1e-12):
            moved = (now.Base - plc.Base).Length
            errors.append(f"Original {key} veraendert ({moved:.4f}mm)")

    joint_errors, parts = check_joints_in_solver_view(top, tmpdir)
    errors += joint_errors
    errors += check_gui_equals_solver(parts)

    errors = [f"{name}: {e}" for e in errors]
    print(f"  {name}: {len(before)} Original-Placements, {'OK' if not errors else f'{len(errors)} FEHLER'}")
    close_all_documents()
    return errors


def main():
    dirs = fixture_dirs()
    if not dirs:
        print("RESULT: FAIL (keine verschachtelten Fixturen gefunden)")
        return 1
    close_all_documents()
    all_errors = []
    with tempfile.TemporaryDirectory() as tmpdir:
        for d in dirs:
            all_errors.extend(check_fixture(d, tmpdir))
    for e in all_errors[:40]:
        print(f"  FEHLER: {e}")
    if all_errors:
        print(f"RESULT: FAIL ({len(all_errors)} Fehler in {len(dirs)} Fixturen)")
        return 1
    print(f"RESULT: PASS ({len(dirs)} Fixturen)")
    return 0
