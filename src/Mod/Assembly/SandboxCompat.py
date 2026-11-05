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

# FCPROJECT-PATCH (2026-11-05, Nutzerauftrag "Datei normalisieren / mit FreeCAD kompatibel machen
# innerhalb Projekt und Verknüpfungen", Plan plans/2026-11-05-datei-normalisieren.md).
#
# Dateiformat: die Sandbox schreibt keine eigenen Eigenschaften, jede Sandbox-Datei öffnet in
# normalem FreeCAD. Unterschiede gibt es an zwei Stellen:
#   1. Aufbau: normales FreeCAD führt in flexiblen Unterbaugruppen Kopien der inneren Gelenke,
#      die Sandbox nicht ("Adressieren statt Kopieren"). Die Weekly legt sie beim Laden selbst an,
#      die stabile 1.1.4 NICHT (gemessen 2026-11-05) - dort ist die Unterbaugruppe innen ohne
#      Gelenk. Das Normalisieren lässt deshalb ein normales FreeCAD die Kopien anlegen
#      (reference-finish), damit der Aufbau exakt dessen eigener ist.
#   2. Deutung: manche Gelenke löst normales FreeCAD anders (Punkt auf Kurve, Mehrfachinstanzen,
#      Grenzen ...). Das ist nicht reparierbar, nur meldbar; die Gegenprobe (reference-check)
#      misst, welches Teil sich wie weit bewegt.
#
# Alles laeuft in eigenen FreeCAD-Prozessen auf KOPIEN der Dateien (SandboxCompatWorker.py) -
# die geöffneten Dokumente und die Originale bleiben unberührt.

import json
import os
import shutil
import subprocess
import tempfile
import time
import zipfile

import FreeCAD as App

PREF_PATH = "User parameter:BaseApp/Preferences/Mod/Assembly"
PREF_REFERENCE = "SandboxCompatReferenceFreeCAD"

# Standard für die Gegenprobe (Nutzerentscheidung 2026-11-05: Weekly-AppImage), danach weitere
# übliche Orte. Auf fremden Rechnern stellt der Nutzer den Pfad im Dialog ein.
REFERENCE_CANDIDATES = (
    "/home/maxx/freecad/App_weekly/freecad.App",
    "/home/maxx/freecad/App_stable/freecad.App",
    "/usr/bin/FreeCADCmd",
    "/usr/bin/freecadcmd",
    "/usr/bin/freecad",
)

WORKER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "SandboxCompatWorker.py")

# Umgebungsvariablen, die die Sandbox-Umgebung (Pixi/conda, eigene Bibliotheken) setzt und die ein
# fremdes FreeCAD durcheinanderbringen wuerden.
_FOREIGN_ENV = (
    "LD_LIBRARY_PATH",
    "PYTHONPATH",
    "PYTHONHOME",
    "PYTHONNOUSERSITE",
    "CONDA_PREFIX",
    "CONDA_DEFAULT_ENV",
    "CONDA_SHLVL",
    "PIXI_PROJECT_ROOT",
    "QT_PLUGIN_PATH",
    "QT_QPA_PLATFORM_PLUGIN_PATH",
    "QML2_IMPORT_PATH",
    "FREECAD_USER_HOME",
    "SSL_CERT_FILE",
)

LEVEL_RED = "rot"
LEVEL_YELLOW = "gelb"
LEVEL_INFO = "info"


# --------------------------------------------------------------------------- Einstellungen


def referenceFreeCAD():
    """Pfad des normalen FreeCAD für Gegenprobe/Abschluss, oder '' wenn keins gefunden."""
    path = App.ParamGet(PREF_PATH).GetString(PREF_REFERENCE, "")
    if path and os.path.exists(path):
        return path
    for cand in REFERENCE_CANDIDATES:
        if os.path.exists(cand):
            return cand
    return ""


def setReferenceFreeCAD(path):
    App.ParamGet(PREF_PATH).SetString(PREF_REFERENCE, path or "")


def _isAppImage(path):
    try:
        with open(path, "rb") as f:
            head = f.read(11)
        return head[8:11] == b"AI\x02"
    except OSError:
        return False


def _commandFor(path):
    """Befehlszeile für FreeCAD ohne Oberfläche aus einem Pfad (AppImage, Binary, Ordner)."""
    if os.path.isdir(path):
        for name in ("FreeCADCmd", "freecadcmd"):
            cand = os.path.join(path, "bin", name)
            if os.path.exists(cand):
                return [cand]
        return None
    if _isAppImage(path):
        return [path, "freecadcmd"]
    base = os.path.basename(path)
    if base.lower() in ("freecadcmd", "freecadcmd.exe"):
        return [path]
    folder = os.path.dirname(path)
    for name in ("FreeCADCmd", "freecadcmd", "FreeCADCmd.exe"):
        cand = os.path.join(folder, name)
        if os.path.exists(cand):
            return [cand]
    return None


def sandboxCommand():
    """FreeCAD ohne Oberfläche aus DIESER Installation (Sandbox)."""
    home = App.getHomePath()
    for name in ("FreeCADCmd", "freecadcmd"):
        cand = os.path.join(home, "bin", name)
        if os.path.exists(cand):
            return [cand]
    return None


# --------------------------------------------------------------------------- Projekt einsammeln


class Project:
    """Aktives Dokument plus alle Dateien, auf die es (rekursiv) verweist."""

    def __init__(self, doc):
        self.doc = doc
        self.docs = [d for d in doc.getDependentDocuments(True)]
        self.unsaved = [d.Label for d in self.docs if not d.FileName]
        self.modified = [d.Label for d in self.docs if d.FileName and d.isTouched()]
        self.files = []
        for d in self.docs:
            if d.FileName and d.FileName not in self.files:
                self.files.append(os.path.normpath(d.FileName))
        top = os.path.normpath(doc.FileName) if doc.FileName else None
        # Hauptdatei zuletzt: erst die Unterbaugruppen, dann wer sie einbaut
        if top in self.files:
            self.files.remove(top)
            self.files.append(top)
        self.top = top
        folders = [os.path.dirname(f) for f in self.files]
        self.root = os.path.commonpath(folders) if folders else ""

    def relative(self, path):
        return os.path.relpath(path, self.root)

    def copyTo(self, target):
        """Alle Projektdateien mit gleicher Ordnerstruktur kopieren (relative Verweise bleiben
        gültig). Gibt die neuen Pfade in derselben Reihenfolge zurück."""
        out = []
        for f in self.files:
            dst = os.path.join(target, self.relative(f))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(f, dst)
            out.append(dst)
        return out


# --------------------------------------------------------------------------- statische Prüfung


def _subShape(obj, sub):
    try:
        import Part

        return Part.getShape(obj, sub, needSubElement=True, noElementMap=True)
    except Exception:  # noqa: BLE001
        return None


def _refKind(ref):
    """'vertex', 'line', 'curve', 'face' oder None für eine Gelenkreferenz."""
    try:
        obj, subs = ref[0], ref[1]
        sub = subs[0] if subs else ""
    except Exception:  # noqa: BLE001
        return None
    element = sub.split(".")[-1] if sub else ""
    if element.startswith("Vertex"):
        return "vertex"
    if element.startswith("Face"):
        return "face"
    if element.startswith("Edge"):
        shape = _subShape(obj, sub)
        try:
            import Part

            if shape is not None and isinstance(shape.Curve, (Part.Line, Part.LineSegment)):
                return "line"
        except Exception:  # noqa: BLE001
            pass
        return "curve"
    return None


def _distanceValue(joint):
    d = getattr(joint, "Distance", 0.0)
    return getattr(d, "Value", d)


def isPointOnCurve(joint):
    if getattr(joint, "JointType", "") != "Distance" or abs(_distanceValue(joint)) > 1e-9:
        return False
    kinds = {_refKind(getattr(joint, "Reference1", None)), _refKind(getattr(joint, "Reference2", None))}
    return kinds == {"vertex", "curve"}


def _hasLimits(joint):
    return any(
        getattr(joint, p, False)
        for p in ("EnableLengthMin", "EnableLengthMax", "EnableAngleMin", "EnableAngleMax")
    )


def staticFindings(project):
    """Regeln aus dem Plan, Abschnitt 1b. Liste von dicts (file, name, label, level, text)."""
    findings = []
    linkTargets = {}
    for doc in project.docs:
        path = os.path.normpath(doc.FileName) if doc.FileName else doc.Label
        for obj in doc.Objects:
            if hasattr(obj, "JointType") and not getattr(obj, "Suppressed", False):
                if isPointOnCurve(obj):
                    findings.append(
                        dict(
                            file=path,
                            name=obj.Name,
                            label=obj.Label,
                            level=LEVEL_RED,
                            kind="pointOnCurve",
                            text="Punkt auf Kurve (Abstand 0): normales FreeCAD kennt das nicht "
                            "und löst es als gewöhnliches Abstandsgelenk - das Teil springt.",
                        )
                    )
                if _hasLimits(obj):
                    findings.append(
                        dict(
                            file=path,
                            name=obj.Name,
                            label=obj.Label,
                            level=LEVEL_INFO,
                            kind="limits",
                            text="Gelenkgrenzen: normales FreeCAD beachtet sie nur beim Ziehen, "
                            "nicht beim Lösen und nicht in der Simulation.",
                        )
                    )
            if obj.TypeId == "Assembly::AssemblyLink":
                target = getattr(obj, "LinkedObject", None)
                if target is not None:
                    linkTargets.setdefault(target.FullName, []).append((path, obj))
    for target, uses in linkTargets.items():
        if len(uses) < 2:
            continue
        for path, obj in uses:
            findings.append(
                dict(
                    file=path,
                    name=obj.Name,
                    label=obj.Label,
                    level=LEVEL_YELLOW,
                    kind="duplicateInstance",
                    text=f"Unterbaugruppe {len(uses)}x eingebaut: normales FreeCAD verliert "
                    "Gelenke an den weiteren Instanzen oder ordnet sie falsch zu.",
                )
            )
    return findings


# --------------------------------------------------------------------------- Arbeitsprozesse


def _cleanEnv(foreign):
    env = dict(os.environ)
    if foreign:
        for k in _FOREIGN_ENV:
            env.pop(k, None)
    env.pop("SESSION_MANAGER", None)
    return env


def runWorker(cmd, mode, files, foreign, extra=None, timeout=900, progress=None):
    """Startet SandboxCompatWorker.py in einem eigenen FreeCAD-Prozess. progress(text) wird
    während des Wartens regelmaessig aufgerufen (Oberfläche bedienbar halten); gibt es False
    zurück, wird abgebrochen."""
    work = tempfile.mkdtemp(prefix="fc-sandbox-compat-")
    try:
        jobPath = os.path.join(work, "job.json")
        outPath = os.path.join(work, "out.json")
        profile = os.path.join(work, "profile")
        os.makedirs(profile)
        job = {"mode": mode, "files": files, "out": outPath}
        job.update(extra or {})
        with open(jobPath, "w", encoding="utf-8") as f:
            json.dump(job, f)
        env = _cleanEnv(foreign)
        env["SANDBOX_COMPAT_JOB"] = jobPath
        # eigenes, leeres Profil: der Hilfsprozess soll weder Addons laden noch beim Beenden die
        # Einstellungen der laufenden Sitzung überschreiben
        env["FREECAD_USER_HOME"] = profile
        logPath = os.path.join(work, "log.txt")
        with open(logPath, "w", encoding="utf-8") as log:
            proc = subprocess.Popen(
                cmd + [WORKER], env=env, cwd=work, stdout=log, stderr=subprocess.STDOUT
            )
            start = time.time()
            while proc.poll() is None:
                if time.time() - start > timeout:
                    proc.kill()
                    return {"error": f"Zeitüberschreitung nach {timeout} s ({' '.join(cmd)})"}
                if progress is not None and progress(f"{mode}: {int(time.time() - start)} s") is False:
                    proc.kill()
                    return {"error": "abgebrochen"}
                time.sleep(0.2)
        if not os.path.exists(outPath):
            with open(logPath, encoding="utf-8", errors="replace") as log:
                tail = log.read()[-1500:]
            return {"error": f"kein Ergebnis von {' '.join(cmd)}:\n{tail}"}
        with open(outPath, encoding="utf-8") as f:
            return json.load(f)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def stripSandboxMark(path):
    """Kennung '-sandbox' aus ProgramVersion im Dateikopf entfernen (nur falls vorhanden)."""
    import re

    pattern = re.compile(r'(ProgramVersion="[^"]*?)-sandbox([^"]*")')
    with zipfile.ZipFile(path) as z:
        xml = z.read("Document.xml").decode("utf-8")
        newText, n = pattern.subn(r"\1\2", xml, count=1)
        if n == 0:
            return False
        items = [(i, z.read(i.filename)) for i in z.infolist()]
    newXml = newText.encode("utf-8")
    tmp = path + ".tmp"
    with zipfile.ZipFile(tmp, "w") as out:
        for info, data in items:
            out.writestr(info, newXml if info.filename == "Document.xml" else data)
    os.replace(tmp, path)
    return True


# --------------------------------------------------------------------------- Abläufe


def _missingCopies(result):
    """Labels flexibler Unterbaugruppen, denen in normalem FreeCAD Gelenkkopien fehlen."""
    return [
        label
        for label, n, expected in (result.get("jointCopies") or {}).values()
        if expected and n < expected
    ]


class Report:
    def __init__(self, project):
        self.project = project
        self.static = []
        self.reference = None  # Ergebnis reference-check
        self.referenceVersion = ""
        self.finish = None
        self.notes = []
        self.errors = []
        self.target = None

    def worstLevel(self):
        levels = [f["level"] for f in self.static]
        if self.reference:
            for r in self.reference:
                if r.get("moves") or r.get("broken") or r.get("error"):
                    levels.append(LEVEL_RED)
                if _missingCopies(r):
                    levels.append(LEVEL_YELLOW)
        if self.errors:
            levels.append(LEVEL_RED)
        for lvl in (LEVEL_RED, LEVEL_YELLOW, LEVEL_INFO):
            if lvl in levels:
                return lvl
        return "gruen"


def _check(report, files, reference, progress):
    cmd = _commandFor(reference) if reference else None
    if not cmd:
        report.notes.append(
            "Keine Gegenprobe: kein normales FreeCAD eingestellt oder gefunden. Es gibt nur die "
            "statische Prüfung."
        )
        return
    res = runWorker(cmd, "reference-check", files, True, progress=progress)
    if "error" in res:
        report.errors.append("Gegenprobe: " + res["error"])
        return
    report.referenceVersion = f"{res.get('version', '?')} ({res.get('build', '')})"
    report.reference = res.get("results", [])
    for r in report.reference:
        r["rel"] = os.path.relpath(r["file"], os.path.dirname(files[-1]))


def checkProject(doc, reference, progress=None):
    """Prüfen: nichts an den Originalen aendern. Kopie in einen Temp-Ordner, dort in der Sandbox
    loesen und speichern, dann Gegenprobe mit normalem FreeCAD auf einer zweiten Kopie."""
    project = Project(doc)
    report = Report(project)
    report.static = staticFindings(project)
    if not project.files:
        report.errors.append("Dokument ist nicht gespeichert.")
        return report
    tmp = tempfile.mkdtemp(prefix="fc-sandbox-check-")
    try:
        files = project.copyTo(tmp)
        cmd = sandboxCommand()
        if cmd:
            res = runWorker(cmd, "sandbox-solve", files, False, progress=progress)
            if "error" in res:
                report.errors.append("Sandbox-Lösen: " + res["error"])
        _check(report, files, reference, progress)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return report


def normalizeProject(doc, target, reference, suppressUnsupported, progress=None):
    """Normalisieren in einen NEUEN Ordner. Originale bleiben unberührt."""
    project = Project(doc)
    report = Report(project)
    report.static = staticFindings(project)
    report.target = target
    if not project.files:
        report.errors.append("Dokument ist nicht gespeichert.")
        return report
    if os.path.exists(target) and os.listdir(target):
        report.errors.append(f"Zielordner ist nicht leer: {target}")
        return report
    files = project.copyTo(target)
    mapping = dict(zip(project.files, files))

    suppress = []
    if suppressUnsupported:
        for f in report.static:
            if f["kind"] == "pointOnCurve" and f["file"] in mapping:
                suppress.append([mapping[f["file"]], f["name"]])
                f["text"] += " In der Kopie deaktiviert."
                f["level"] = LEVEL_YELLOW

    # 1. in der Sandbox loesen (Lagen wie in der Sandbox), ggf. Gelenke deaktivieren, speichern
    cmd = sandboxCommand()
    if cmd:
        res = runWorker(cmd, "sandbox-solve", files, False, {"suppress": suppress}, progress=progress)
        if "error" in res:
            report.errors.append("Sandbox-Lösen: " + res["error"])
            return report
        for r in res.get("results", []):
            if r.get("error"):
                report.errors.append(f"{os.path.basename(r['file'])}: {r['error']}")
    else:
        report.errors.append("FreeCADCmd dieser Installation nicht gefunden.")
        return report

    # 2. normales FreeCAD legt seine Gelenkkopien an (ohne zu loesen) und speichert
    refCmd = _commandFor(reference) if reference else None
    if refCmd:
        res = runWorker(refCmd, "reference-finish", files, True, progress=progress)
        if "error" in res:
            report.errors.append("Abschluss mit normalem FreeCAD: " + res["error"])
        else:
            report.finish = res.get("results", [])
    else:
        report.notes.append(
            "Ohne normales FreeCAD fehlen in flexiblen Unterbaugruppen dessen Gelenkkopien; "
            "FreeCAD 1.1 legt sie beim Öffnen nicht selbst an."
        )

    # 3. Kopfzeile säubern
    for f in files:
        try:
            if stripSandboxMark(f):
                report.notes.append(f"Sandbox-Kennung entfernt: {os.path.basename(f)}")
        except Exception as e:  # noqa: BLE001
            report.errors.append(f"{os.path.basename(f)}: Kopf nicht lesbar ({e})")

    # 4. Gegenprobe auf einer Wegwerf-Kopie des Ergebnisses
    tmp = tempfile.mkdtemp(prefix="fc-sandbox-check-")
    try:
        checkFiles = []
        for f in files:
            dst = os.path.join(tmp, os.path.relpath(f, target))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(f, dst)
            checkFiles.append(dst)
        _check(report, checkFiles, reference, progress)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return report


# --------------------------------------------------------------------------- Bericht


def reportHtml(report, title):
    colors = {LEVEL_RED: "#d33", LEVEL_YELLOW: "#c90", LEVEL_INFO: "#38c", "gruen": "#393"}
    words = {LEVEL_RED: "verhält sich anders", LEVEL_YELLOW: "Hinweis", LEVEL_INFO: "Info", "gruen": "identisch"}
    worst = report.worstLevel()
    p = report.project
    html = [f"<h3>{title}</h3>"]
    html.append(
        f"<p><b style='color:{colors[worst]}'>Gesamt: {words[worst]}</b><br>"
        f"Projektordner: {p.root}<br>{len(p.files)} Datei(en)"
        + (f"<br>Ziel: {report.target}" if report.target else "")
        + (f"<br>Gegenprobe mit FreeCAD {report.referenceVersion}" if report.referenceVersion else "")
        + "</p>"
    )
    if p.unsaved or p.modified:
        html.append(
            "<p style='color:#c90'>Nicht gespeicherte Änderungen werden nicht berücksichtigt: "
            + ", ".join(p.unsaved + p.modified)
            + "</p>"
        )
    for e in report.errors:
        html.append(f"<p style='color:#d33'><b>Fehler:</b> {e}</p>")
    if report.static:
        html.append("<h4>Gelenke und Unterbaugruppen</h4><table cellpadding='3'>")
        for f in report.static:
            html.append(
                f"<tr><td style='color:{colors[f['level']]}'>&#9679;</td>"
                f"<td>{os.path.relpath(f['file'], p.root) if p.root else f['file']}</td>"
                f"<td><b>{f['label']}</b></td><td>{f['text']}</td></tr>"
            )
        html.append("</table>")
    if report.reference is not None:
        html.append("<h4>Gegenprobe: so sieht es in normalem FreeCAD aus</h4><table cellpadding='3'>")
        for r in report.reference:
            name = os.path.basename(r["file"])
            if r.get("error"):
                html.append(f"<tr><td style='color:#d33'>&#9679;</td><td>{name}</td><td>{r['error']}</td></tr>")
                continue
            parts = []
            if r.get("broken"):
                parts.append("Fehlerhafte Objekte: " + ", ".join(r["broken"]))
            missing = _missingCopies(r)
            if missing:
                parts.append(
                    "flexible Unterbaugruppe ohne Gelenkkopien (fällt beim Ziehen auseinander): "
                    + ", ".join(missing)
                )
            for label, dist, angle in (r.get("moves") or [])[:8]:
                parts.append(f"{label}: verschoben {dist:g} mm, gedreht {angle:g}&deg;")
            bad = [s for s in r.get("solve", []) if s[1] not in (0, "0")]
            for asm, code in bad:
                parts.append(f"Lösen von {asm} meldet {code}")
            level = LEVEL_RED if (r.get("moves") or r.get("broken") or bad) else (LEVEL_YELLOW if missing else "gruen")
            text = "<br>".join(parts) if parts else "keine Bewegung, keine Fehler"
            html.append(f"<tr><td style='color:{colors[level]}'>&#9679;</td><td>{name}</td><td>{text}</td></tr>")
        html.append("</table>")
    if report.finish:
        made, shifted = [], []
        for r in report.finish:
            for label, n, expected in (r.get("jointCopies") or {}).values():
                if expected:
                    made.append(f"{os.path.basename(r['file'])}: {label} ({n} von {expected} Gelenken)")
            for label, dist, angle in (r.get("movedOnRecompute") or [])[:8]:
                shifted.append(f"{os.path.basename(r['file'])}: {label} {dist:g} mm / {angle:g}&deg;")
        if made:
            html.append("<h4>Gelenkkopien für normales FreeCAD angelegt</h4><p>" + "<br>".join(made) + "</p>")
        if shifted:
            html.append(
                "<p style='color:#c90'>Normales FreeCAD hat beim Neuberechnen verschoben (in der Kopie "
                "auf die Sandbox-Lage zurückgesetzt):<br>" + "<br>".join(shifted) + "</p>"
            )
    for n in report.notes:
        html.append(f"<p style='color:#38c'>{n}</p>")
    return "\n".join(html)
