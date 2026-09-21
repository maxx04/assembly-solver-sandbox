# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    Copyright (c) 2026 Ondsel <development@ondsel.com>                     *
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

import os
import subprocess
import tempfile

import FreeCAD as App
import UtilsAssembly

from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide.QtCore import QUrl
    from PySide.QtGui import QDesktopServices
    from PySide.QtWidgets import QMessageBox


__title__ = "Assembly Command Show Identity Graph"
__author__ = "Claude"
__url__ = "https://www.freecad.org"


class CommandShowIdentityGraph:
    def __init__(self):
        pass

    def GetResources(self):
        return {
            "Pixmap": "Assembly_ShowIdentityGraph",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_ShowIdentityGraph", "Show Identity Graph"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_ShowIdentityGraph",
                "Render the assembly's parts, joints and grounding as an SVG diagram "
                "(requires Graphviz 'dot' to be installed and on PATH).",
            ),
            "CmdType": "ForEdit",
        }

    def IsActive(self):
        return UtilsAssembly.isAssemblyCommandActive()

    def Activated(self):
        assembly = UtilsAssembly.activeAssembly()
        if not assembly:
            return

        dot_source = assembly.exportIdentityGraphDot()

        tmp_dir = tempfile.gettempdir()
        base_name = "freecad_identity_graph_" + assembly.Document.Name
        dot_path = os.path.join(tmp_dir, base_name + ".dot")
        svg_path = os.path.join(tmp_dir, base_name + ".svg")

        with open(dot_path, "w", encoding="utf-8") as dot_file:
            dot_file.write(dot_source)

        try:
            # Nutzerauftrag 2026-09-21: falls 'dot' aus irgendeinem Grund haengt, soll das NIE
            # als OS-eigenes "Anwendung reagiert nicht - Beenden/Warten"-Popup enden (der
            # synchrone subprocess.run()-Aufruf blockiert sonst den GUI-Thread unbegrenzt) -
            # stattdessen eine eigene, informative Meldung nach spaetestens 15s.
            subprocess.run(
                ["dot", "-Tsvg", dot_path, "-o", svg_path],
                check=True,
                capture_output=True,
                timeout=15,
            )
        except subprocess.TimeoutExpired:
            QMessageBox.warning(
                Gui.getMainWindow(),
                "Graph rendering timed out",
                "Rendering the graph with 'dot' (Graphviz) took longer than 15 seconds and was "
                "aborted. This is likely an unusually large/complex assembly.\n\n"
                f"DOT source was written to:\n{dot_path}",
            )
            return
        except (OSError, subprocess.CalledProcessError) as err:
            detail = err.stderr.decode("utf-8", "replace") if getattr(err, "stderr", None) else str(err)
            QMessageBox.warning(
                Gui.getMainWindow(),
                "Graphviz not found",
                "Could not render the graph with 'dot' (Graphviz). Please install it "
                "and make sure it is on PATH.\n\nDOT source was written to:\n"
                f"{dot_path}\n\n{detail}",
            )
            return

        QDesktopServices.openUrl(QUrl.fromLocalFile(svg_path))


if App.GuiUp:
    Gui.addCommand("Assembly_ShowIdentityGraph", CommandShowIdentityGraph())
