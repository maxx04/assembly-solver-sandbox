# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# **************************************************************************/

# FCPROJECT-PATCH (2026-11-05, Befehl "Für FreeCAD prüfen/normalisieren", siehe
# plans/2026-11-05-datei-normalisieren.md): Arbeitsskript für einen eigenen FreeCAD-Prozess ohne
# Oberfläche (FreeCADCmd). Laeuft sowohl in der Sandbox als auch in einem NORMALEN FreeCAD
# (Weekly, 1.1.x) - darf deshalb nichts aus der Sandbox importieren und nur Python-API nutzen,
# die es in normalem FreeCAD gibt.
#
# Auftrag kommt als JSON-Datei, Pfad in der Umgebungsvariable SANDBOX_COMPAT_JOB:
#   {"mode": "...", "files": [...], "out": "<ergebnis.json>", ...}
# Modi:
#   sandbox-solve    jede Datei oeffnen, alle Baugruppen loesen, ggf. Gelenke deaktivieren
#                    ("suppress": [[datei, objektname], ...]), speichern
#   reference-finish jede Datei oeffnen, flexible Unterbaugruppen neu berechnen lassen (normales
#                    FreeCAD legt dabei seine Gelenkkopien an), speichern - NICHT loesen
#   reference-check  jede Datei oeffnen, Lagen merken, alle Baugruppen loesen, Bewegung je Teil
#                    messen; nichts speichern

import json
import math
import os
import traceback

import FreeCAD as App


def _closeAll():
    for name in list(App.listDocuments().keys()):
        try:
            App.closeDocument(name)
        except Exception:
            pass


def _assemblies(doc):
    return [o for o in doc.Objects if o.TypeId == "Assembly::AssemblyObject"]


def _placements(doc):
    result = {}
    for o in doc.Objects:
        if o.TypeId in ("App::Link", "Assembly::AssemblyLink", "App::Part", "PartDesign::Body") or (
            o.isDerivedFrom("Part::Feature") and o.getParentGeoFeatureGroup() is not None
        ):
            plc = getattr(o, "Placement", None)
            if plc is not None:
                result[o.Name] = (o.Label, list(plc.Base), list(plc.Rotation.Q))
    return result


def _jointCopies(doc):
    """Flexible Unterbaugruppen: (Label, Gelenkkopien darin, Gelenke im Original, die normales
    FreeCAD kopieren wuerde). Starre Gruppen und Erdungen haben keinen JointType und werden von
    normalem FreeCAD nicht kopiert - sie zaehlen nicht mit."""
    out = {}
    for o in doc.Objects:
        if o.TypeId != "Assembly::AssemblyLink" or getattr(o, "Rigid", True):
            continue
        n = 0
        for g in o.Group:
            if g.TypeId == "Assembly::JointGroup":
                n += len(g.Group)
        expected = 0
        src = getattr(o, "LinkedObject", None)
        for g in getattr(src, "Group", []) or []:
            if g.TypeId == "Assembly::JointGroup":
                expected += sum(
                    1
                    for j in g.Group
                    if hasattr(j, "JointType") and not getattr(j, "Suppressed", False)
                )
        out[o.Name] = (o.Label, n, expected)
    return out


def _brokenObjects(doc):
    return [o.Label for o in doc.Objects if "Invalid" in o.State or "Error" in o.State]


def _solveAll(doc):
    codes = []
    for asm in _assemblies(doc):
        try:
            codes.append([asm.Label, int(asm.solve())])
        except Exception as e:  # noqa: BLE001
            codes.append([asm.Label, str(e)])
    return codes


def _movement(before, after):
    moves = []
    for name, (label, base0, q0) in before.items():
        if name not in after:
            continue
        _, base1, q1 = after[name]
        dist = math.dist(base0, base1)
        dot = min(1.0, abs(sum(a * b for a, b in zip(q0, q1))))
        angle = math.degrees(2.0 * math.acos(dot))
        if dist > 1e-4 or angle > 1e-3:
            moves.append([label, round(dist, 4), round(angle, 3)])
    moves.sort(key=lambda m: -(m[1] + m[2] / 10.0))
    return moves


def _sandboxSolve(job):
    suppress = {}
    for path, objName in job.get("suppress", []):
        suppress.setdefault(os.path.normpath(path), []).append(objName)
    results = []
    for path in job["files"]:
        entry = {"file": path}
        try:
            doc = App.openDocument(path)
            entry["solve"] = _solveAll(doc)
            done = []
            for objName in suppress.get(os.path.normpath(path), []):
                obj = doc.getObject(objName)
                if obj is not None and hasattr(obj, "Suppressed"):
                    obj.Suppressed = True
                    done.append(obj.Label)
            entry["suppressed"] = done
            doc.recompute()
            doc.save()
        except Exception:  # noqa: BLE001
            entry["error"] = traceback.format_exc(limit=3)
        _closeAll()
        results.append(entry)
    return results


def _referenceFinish(job):
    results = []
    for path in job["files"]:
        entry = {"file": path}
        try:
            doc = App.openDocument(path)
            links = [
                o
                for o in doc.Objects
                if o.TypeId == "Assembly::AssemblyLink" and not getattr(o, "Rigid", True)
            ]
            before = _placements(doc)
            for link in links:
                link.touch()
            doc.recompute()
            entry["jointCopies"] = _jointCopies(doc)
            # Neuberechnen kann in normalem FreeCAD schon loesen und Teile verschieben. Die Kopie
            # soll aber die Lagen der Sandbox behalten - die Abweichung meldet die Gegenprobe.
            entry["movedOnRecompute"] = _movement(before, _placements(doc))
            for name, (_label, base, q) in before.items():
                obj = doc.getObject(name)
                if obj is not None:
                    obj.Placement = App.Placement(App.Vector(*base), App.Rotation(*q))
            doc.save()
        except Exception:  # noqa: BLE001
            entry["error"] = traceback.format_exc(limit=3)
        _closeAll()
        results.append(entry)
    return results


def _referenceCheck(job):
    results = []
    for path in job["files"]:
        entry = {"file": path}
        try:
            doc = App.openDocument(path)
            entry["broken"] = _brokenObjects(doc)
            entry["jointCopies"] = _jointCopies(doc)
            before = _placements(doc)
            entry["solve"] = _solveAll(doc)
            entry["moves"] = _movement(before, _placements(doc))
            entry["brokenAfter"] = _brokenObjects(doc)
        except Exception:  # noqa: BLE001
            entry["error"] = traceback.format_exc(limit=3)
        _closeAll()
        results.append(entry)
    return results


def run():
    jobPath = os.environ.get("SANDBOX_COMPAT_JOB")
    if not jobPath:
        return
    with open(jobPath, encoding="utf-8") as f:
        job = json.load(f)
    handlers = {
        "sandbox-solve": _sandboxSolve,
        "reference-finish": _referenceFinish,
        "reference-check": _referenceCheck,
    }
    out = {"version": ".".join(App.Version()[0:3]), "build": App.Version()[3]}
    try:
        out["results"] = handlers[job["mode"]](job)
    except Exception:  # noqa: BLE001
        out["error"] = traceback.format_exc(limit=5)
    with open(job["out"], "w", encoding="utf-8") as f:
        json.dump(out, f)


run()
