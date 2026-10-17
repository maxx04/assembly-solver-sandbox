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

# FCPROJECT-PATCH (2026-10-17, Nutzerauftrag): Dateien mit Gelenktypen, die es nur in dieser
# Sandbox gibt, werden beim Speichern im Dateikopf markiert (ProgramVersion "...-sandbox") und es
# kommt eine Warnung. Andere FreeCAD-Versionen uebergehen solche Gelenke beim Loesen still, der
# Gelenk-Dialog bricht dort mit "'Tangent' is not in list" ab.

import FreeCAD as App

# Gelenktypen, die nur diese Sandbox kennt (am Ende von JointObject.JointTypes angehaengt).
SANDBOX_JOINT_TYPES = ("Tangent",)
SANDBOX_MARK = "-sandbox"


def sandboxJoints(doc):
    """Labels aller Gelenke im Dokument mit einem Sandbox-eigenen Typ."""
    found = []
    for obj in doc.Objects:
        try:
            jointType = obj.JointType if "JointType" in obj.PropertiesList else None
        except Exception:
            jointType = None
        if jointType in SANDBOX_JOINT_TYPES:
            found.append(f"{obj.Label} ({jointType})")
    return found


def markedRevision(revision):
    """BuildRevision mit Sandbox-Kennung, z.B. '49261 (Git)' -> '49261-sandbox (Git)'. Der Kopf
    lautet dann '27.1R49261-sandbox (Git)' - FreeCADs Versionspruefung liest weiter 27.1."""
    if SANDBOX_MARK in revision:
        return revision
    if " (" in revision:
        head, tail = revision.split(" (", 1)
        return f"{head}{SANDBOX_MARK} ({tail}"
    return revision + SANDBOX_MARK


class SandboxSaveCheck:
    def __init__(self):
        self.originalRevision = None

    def slotStartSaveDocument(self, doc, filename):
        self.restore()
        joints = sandboxJoints(doc)
        if not joints:
            return
        revision = App.ConfigGet("BuildRevision")
        self.originalRevision = revision
        App.ConfigSet("BuildRevision", markedRevision(revision))
        App.Console.PrintWarning(
            "Assembly: '{}' enthaelt Gelenke, die nur diese Sandbox-Version kennt: {}. Die Datei "
            "wird als Sandbox-Datei markiert; andere FreeCAD-Versionen koennen diese Gelenke nicht "
            "verwenden (sie werden beim Loesen uebergangen, der Gelenk-Dialog oeffnet nicht).\n".format(
                doc.Label, ", ".join(joints)
            )
        )

    def slotFinishSaveDocument(self, doc, filename):
        self.restore()

    def restore(self):
        if self.originalRevision is not None:
            App.ConfigSet("BuildRevision", self.originalRevision)
            self.originalRevision = None


_observer = None


def install():
    global _observer
    if _observer is None:
        _observer = SandboxSaveCheck()
        App.addDocumentObserver(_observer)
