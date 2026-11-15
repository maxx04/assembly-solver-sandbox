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

# FCPROJECT-PATCH (2026-11-15, Nutzerentscheidung): Hinweis-Objekt in Baugruppen-Dateien der Sandbox.
#
# Problem: normales FreeCAD oeffnet Sandbox-Baugruppen ohne Fehler, loest aber manche Gelenke
# anders (siehe plans/2026-11-05-datei-normalisieren.md). Umgekehrt gibt es kein Problem.
#
# Loesung: beim Speichern einer Datei mit Baugruppe legt AssemblySandboxSaveCheck.py ein
# App::FeaturePython mit diesem Modul als Proxy an. Normales FreeCAD kennt das Modul nicht und
# importiert es beim Laden aus Sicherheitsgruenden nicht ("module not permitted") - es zeigt das
# Objekt mit seiner Beschriftung im Baum und meldet einen Fehler im Ausgabefenster. Sonst laedt die
# Datei vollstaendig; nichts geht verloren.
#
# In der Sandbox ist das Modul vorhanden: kein Fehler, und die Ansichtsklasse blendet das Objekt im
# Baum aus. Das Ausblenden (ShowInTree) wird NIE gespeichert - AssemblySandboxSaveCheck schaltet es
# fuer die Dauer des Speicherns wieder ein, damit normales FreeCAD den Hinweis sieht.
#
# "Fuer FreeCAD normalisieren" (SandboxCompat.py) entfernt das Objekt aus der Kopie.

import FreeCAD as App

MARKER_NAME = "SandboxAssemblyMarker"
MARKER_LABEL = "⚠ SANDBOX ASSEMBLY – compatibility issues with standard FreeCAD"
MARKER_INFO = (
    "This file was saved with the FreeCAD Assembly Solver Sandbox. Standard FreeCAD opens it, "
    "but may solve some joints differently (e.g. point on curve, nested or multiply inserted "
    "sub-assemblies, joint limits). In the sandbox, use Assembly > Sandbox > 'Normalize for "
    "FreeCAD' to export a compatible copy. "
    "Source: https://github.com/maxx04/assembly-solver-sandbox"
)


class SandboxAssemblyMarker:
    """Proxy des Hinweis-Objekts. Hat keinen eigenen Zustand."""

    def __init__(self, obj):
        obj.Proxy = self
        if not hasattr(obj, "Info"):
            obj.addProperty("App::PropertyString", "Info", "Sandbox", "Why this object exists")
        obj.Info = MARKER_INFO
        obj.setEditorMode("Info", 1)  # nur lesen

    def execute(self, obj):
        pass

    def dumps(self):
        return None

    def loads(self, state):
        return None


class ViewProviderSandboxAssemblyMarker:
    """Nur in der Sandbox geladen: blendet das Hinweis-Objekt im Baum aus."""

    def __init__(self, vobj):
        vobj.Proxy = self
        self.attach(vobj)

    def attach(self, vobj):
        self.vobj = vobj

    def finishRestoring(self):
        setTreeVisible(getattr(self, "vobj", None), False)

    def getIcon(self):
        return ":/icons/Assembly_ExportASMT.svg"

    def dumps(self):
        return None

    def loads(self, state):
        return None


def setTreeVisible(vobj, visible):
    if vobj is None:
        return
    try:
        if vobj.ShowInTree != visible:
            vobj.ShowInTree = visible
    except Exception:  # noqa: BLE001
        pass


def hasAssembly(doc):
    return any(o.TypeId == "Assembly::AssemblyObject" for o in doc.Objects)


def markers(doc):
    return [
        o
        for o in doc.Objects
        if o.TypeId == "App::FeaturePython"
        and (
            isinstance(getattr(o, "Proxy", None), SandboxAssemblyMarker)
            or o.Name.startswith(MARKER_NAME)
        )
    ]


def ensureMarker(doc):
    """Legt das Hinweis-Objekt an, falls das Dokument eine Baugruppe hat und noch keins enthaelt."""
    if not hasAssembly(doc) or markers(doc):
        return None
    obj = doc.addObject("App::FeaturePython", MARKER_NAME)
    SandboxAssemblyMarker(obj)
    obj.Label = MARKER_LABEL
    if App.GuiUp and getattr(obj, "ViewObject", None) is not None:
        ViewProviderSandboxAssemblyMarker(obj.ViewObject)
        setTreeVisible(obj.ViewObject, False)
    return obj


def removeMarkers(doc):
    for obj in markers(doc):
        doc.removeObject(obj.Name)


def showMarkersInTree(doc, visible):
    """Vor dem Speichern True (gespeicherter Wert = sichtbar fuer normales FreeCAD), danach False."""
    if not App.GuiUp:
        return
    for obj in markers(doc):
        setTreeVisible(getattr(obj, "ViewObject", None), visible)
