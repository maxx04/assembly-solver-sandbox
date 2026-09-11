// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2024 Ondsel <development@ondsel.com>                     *
 *                                                                          *
 *   This file is part of FreeCAD.                                          *
 *                                                                          *
 *   FreeCAD is free software: you can redistribute it and/or modify it     *
 *   under the terms of the GNU Lesser General Public License as            *
 *   published by the Free Software Foundation, either version 2.1 of the   *
 *   License, or (at your option) any later version.                        *
 *                                                                          *
 *   FreeCAD is distributed in the hope that it will be useful, but         *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of             *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
 *   Lesser General Public License for more details.                        *
 *                                                                          *
 *   You should have received a copy of the GNU Lesser General Public       *
 *   License along with FreeCAD. If not, see                                *
 *   <https://www.gnu.org/licenses/>.                                       *
 *                                                                          *
 ***************************************************************************/


#include "AssemblyObject.h"

// inclusion of the generated files (generated out of AssemblyLink.xml)
#include "AssemblyLinkPy.h"
#include "AssemblyLinkPy.cpp"

using namespace Assembly;

// returns a string which represents the object e.g. when printed in python
std::string AssemblyLinkPy::representation() const
{
    return {"<Assembly link>"};
}

PyObject* AssemblyLinkPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int AssemblyLinkPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}

Py::List AssemblyLinkPy::getJoints() const
{
    Py::List ret;
    // FCPROJECT-PATCH (Migrationsschritt 4.4 "Adressieren statt Kopieren", siehe
    // docs/ARCHITECTURE.md Abschnitt 5): getAssemblyLinkPtr()->getJoints() (AssemblyLink::
    // getJoints(), liest die lokale Kopie) liefert seit Migrationsschritt 4.3 IMMER leer, weil
    // updateContents() im Flexibel-Zweig keine lokale JointGroup mehr anlegt - ohne diesen Fix
    // waere die Python-API hier still auf [] zurueckgefallen statt weiterhin eine sinnvolle
    // (jetzt: echte statt kopierte) Liste zu liefern. Fuer eine RIGIDE AssemblyLink bleibt die
    // Liste weiterhin leer (keine eigenen Joints, unveraendertes Verhalten).
    AssemblyObject* linked = getAssemblyLinkPtr()->getLinkedAssembly();
    if (linked) {
        std::vector<App::DocumentObject*> list = extractJointObjects(linked->getJoints(false, false));
        for (auto It : list) {
            ret.append(Py::Object(It->getPyObject(), true));
        }
    }

    return ret;
}
