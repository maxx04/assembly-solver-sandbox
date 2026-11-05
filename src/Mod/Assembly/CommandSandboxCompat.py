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

# FCPROJECT-PATCH (2026-11-05): Befehle "Für FreeCAD prüfen…" und "Für FreeCAD normalisieren…"
# im Menü Assembly -> Sandbox. Logik in SandboxCompat.py, Plan plans/2026-11-05-datei-normalisieren.md.

import os

import FreeCAD as App

from PySide.QtCore import QT_TRANSLATE_NOOP

if App.GuiUp:
    import FreeCADGui as Gui
    from PySide import QtCore, QtWidgets

import SandboxCompat

__title__ = "Assembly Sandbox Compatibility Commands"
__author__ = "Claude"
__url__ = "https://www.freecad.org"


def _activeDocument():
    doc = App.ActiveDocument
    return doc if doc is not None and doc.FileName else None


class CompatDialog(QtWidgets.QDialog if App.GuiUp else object):
    def __init__(self, doc):
        super().__init__(Gui.getMainWindow())
        self.doc = doc
        self.lastReport = None
        self.setWindowTitle(f"Für FreeCAD prüfen / normalisieren – {doc.Label}")
        self.resize(820, 620)
        lay = QtWidgets.QVBoxLayout(self)

        intro = QtWidgets.QLabel(
            "Prüft die Datei samt allen verknüpften Dateien des Projekts darauf, wie sie in "
            "normalem FreeCAD aussieht. Normalisieren schreibt eine angepasste Kopie in einen neuen "
            "Ordner – die Originale bleiben unverändert."
        )
        intro.setWordWrap(True)
        lay.addWidget(intro)

        row = QtWidgets.QHBoxLayout()
        row.addWidget(QtWidgets.QLabel("Normales FreeCAD für die Gegenprobe:"))
        self.refEdit = QtWidgets.QLineEdit(SandboxCompat.referenceFreeCAD())
        self.refEdit.setPlaceholderText("AppImage, FreeCAD- oder FreeCADCmd-Programm (leer = keine Gegenprobe)")
        row.addWidget(self.refEdit, 1)
        browse = QtWidgets.QPushButton("…")
        browse.clicked.connect(self.browseReference)
        row.addWidget(browse)
        lay.addLayout(row)

        self.suppressBox = QtWidgets.QCheckBox(
            "Gelenke, die normales FreeCAD nicht kann (Punkt auf Kurve), in der Kopie deaktivieren"
        )
        self.suppressBox.setChecked(True)
        lay.addWidget(self.suppressBox)

        self.view = QtWidgets.QTextBrowser()
        self.view.setOpenExternalLinks(True)
        lay.addWidget(self.view, 1)

        buttons = QtWidgets.QHBoxLayout()
        self.checkBtn = QtWidgets.QPushButton("Prüfen")
        self.checkBtn.clicked.connect(self.runCheck)
        self.normBtn = QtWidgets.QPushButton("Normalisieren…")
        self.normBtn.clicked.connect(self.runNormalize)
        copyBtn = QtWidgets.QPushButton("Bericht kopieren")
        copyBtn.clicked.connect(self.copyReport)
        closeBtn = QtWidgets.QPushButton("Schließen")
        closeBtn.clicked.connect(self.close)
        for b in (self.checkBtn, self.normBtn, copyBtn):
            buttons.addWidget(b)
        buttons.addStretch(1)
        buttons.addWidget(closeBtn)
        lay.addLayout(buttons)

    # ---------------------------------------------------------------- Hilfen

    def browseReference(self):
        path, _ = QtWidgets.QFileDialog.getOpenFileName(
            self, "Normales FreeCAD wählen", os.path.dirname(self.refEdit.text()) or os.path.expanduser("~")
        )
        if path:
            self.refEdit.setText(path)

    def reference(self):
        path = self.refEdit.text().strip()
        SandboxCompat.setReferenceFreeCAD(path)
        return path

    def _busy(self, title):
        dlg = QtWidgets.QProgressDialog(title, "Abbrechen", 0, 0, self)
        dlg.setWindowModality(QtCore.Qt.WindowModal)
        dlg.setMinimumDuration(0)
        dlg.show()

        def progress(text):
            dlg.setLabelText(f"{title}\n{text}")
            QtWidgets.QApplication.processEvents()
            return not dlg.wasCanceled()

        return dlg, progress

    def _unsavedWarning(self):
        project = SandboxCompat.Project(self.doc)
        pending = project.unsaved + project.modified
        if not pending:
            return True
        answer = QtWidgets.QMessageBox.question(
            self,
            "Nicht gespeichert",
            "Diese Dokumente haben ungespeicherte Änderungen, geprüft wird der gespeicherte Stand:\n"
            + "\n".join(pending)
            + "\n\nTrotzdem fortfahren?",
        )
        return answer == QtWidgets.QMessageBox.Yes

    def _show(self, report, title):
        self.lastReport = report
        self.view.setHtml(SandboxCompat.reportHtml(report, title))

    def _setEnabled(self, on):
        self.checkBtn.setEnabled(on)
        self.normBtn.setEnabled(on)

    # ---------------------------------------------------------------- Abläufe

    def runCheck(self):
        if not self._unsavedWarning():
            return
        self._setEnabled(False)
        dlg, progress = self._busy("Prüfe Projekt …")
        try:
            report = SandboxCompat.checkProject(self.doc, self.reference(), progress)
        finally:
            dlg.close()
            self._setEnabled(True)
        self._show(report, "Prüfung (Originale unverändert)")

    def runNormalize(self):
        if not self._unsavedWarning():
            return
        project = SandboxCompat.Project(self.doc)
        parent = QtWidgets.QFileDialog.getExistingDirectory(
            self,
            "Wohin soll die normalisierte Kopie? (es wird ein neuer Unterordner angelegt)",
            os.path.dirname(project.root),
        )
        if not parent:
            return
        base = os.path.basename(project.root.rstrip(os.sep)) or "Projekt"
        target = os.path.join(parent, base + "_FreeCAD")
        n = 2
        while os.path.exists(target):
            target = os.path.join(parent, f"{base}_FreeCAD_{n}")
            n += 1
        self._setEnabled(False)
        dlg, progress = self._busy(f"Normalisiere nach {target} …")
        try:
            report = SandboxCompat.normalizeProject(
                self.doc, target, self.reference(), self.suppressBox.isChecked(), progress
            )
        finally:
            dlg.close()
            self._setEnabled(True)
        self._show(report, "Normalisiert (Kopie)")

    def copyReport(self):
        QtWidgets.QApplication.clipboard().setText(self.view.toPlainText())


class _CompatCommand:
    mode = "check"

    def IsActive(self):
        return _activeDocument() is not None

    def Activated(self):
        doc = _activeDocument()
        if doc is None:
            return
        dlg = CompatDialog(doc)
        dlg.show()
        if self.mode == "check":
            dlg.runCheck()
        else:
            dlg.runNormalize()


class CommandSandboxCheck(_CompatCommand):
    mode = "check"

    def GetResources(self):
        return {
            "Pixmap": "Assembly_ExportASMT",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_SandboxCheck", "Für FreeCAD prüfen…"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_SandboxCheck",
                "Prüft die Datei samt verknüpften Dateien: verhält sie sich in normalem FreeCAD "
                "genauso wie in dieser Sandbox? Ändert nichts.",
            ),
        }


class CommandSandboxNormalize(_CompatCommand):
    mode = "normalize"

    def GetResources(self):
        return {
            "Pixmap": "Assembly_ExportASMT",
            "MenuText": QT_TRANSLATE_NOOP("Assembly_SandboxNormalize", "Für FreeCAD normalisieren…"),
            "ToolTip": QT_TRANSLATE_NOOP(
                "Assembly_SandboxNormalize",
                "Schreibt das Projekt als Kopie in einen neuen Ordner, aufbereitet für normales "
                "FreeCAD (Gelenkkopien in Unterbaugruppen, optional nicht unterstützte Gelenke "
                "deaktiviert), mit Gegenprobe.",
            ),
        }


if App.GuiUp:
    Gui.addCommand("Assembly_SandboxCheck", CommandSandboxCheck())
    Gui.addCommand("Assembly_SandboxNormalize", CommandSandboxNormalize())
