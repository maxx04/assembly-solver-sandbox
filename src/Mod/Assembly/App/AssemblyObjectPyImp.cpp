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


// inclusion of the generated files (generated out of AssemblyObject.xml)
#include "AssemblyObjectPy.h"
#include "AssemblyObjectPy.cpp"

using namespace Assembly;

// returns a string which represents the object e.g. when printed in python
std::string AssemblyObjectPy::representation() const
{
    return {"<Assembly object>"};
}

PyObject* AssemblyObjectPy::getCustomAttributes(const char* /*attr*/) const
{
    return nullptr;
}

int AssemblyObjectPy::setCustomAttributes(const char* /*attr*/, PyObject* /*obj*/)
{
    return 0;
}

PyObject* AssemblyObjectPy::solve(PyObject* args) const
{
    PyObject* enableUndoPy;
    bool enableUndo;

    if (!PyArg_ParseTuple(args, "O!", &PyBool_Type, &enableUndoPy)) {
        PyErr_Clear();
        if (!PyArg_ParseTuple(args, "")) {
            return nullptr;
        }
        else {
            enableUndo = false;
        }
    }
    else {
        enableUndo = Base::asBoolean(enableUndoPy);
    }

    int ret = this->getAssemblyObjectPtr()->solve(enableUndo);
    return Py_BuildValue("i", ret);
}

PyObject* AssemblyObjectPy::generateSimulation(PyObject* args) const
{
    PyObject* pyobj;

    if (!PyArg_ParseTuple(args, "O", &pyobj)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    int ret = this->getAssemblyObjectPtr()->generateSimulation(obj);
    return Py_BuildValue("i", ret);
}

PyObject* AssemblyObjectPy::ensureIdentityPlacements(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    this->getAssemblyObjectPtr()->ensureIdentityPlacements();
    Py_Return;
}

PyObject* AssemblyObjectPy::updateForFrame(PyObject* args) const
{
    unsigned long index {};

    if (!PyArg_ParseTuple(args, "k", &index)) {
        throw Py::RuntimeError("updateForFrame requires an integer index");
    }
    PY_TRY
    {
        this->getAssemblyObjectPtr()->updateForFrame(index);
    }
    PY_CATCH;

    Py_Return;
}

PyObject* AssemblyObjectPy::numberOfFrames(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    size_t ret = this->getAssemblyObjectPtr()->numberOfFrames();
    return Py_BuildValue("k", ret);
}

PyObject* AssemblyObjectPy::updateSolveStatus(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    this->getAssemblyObjectPtr()->updateSolveStatus();
    Py_Return;
}

PyObject* AssemblyObjectPy::undoSolve(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    this->getAssemblyObjectPtr()->undoSolve();
    Py_Return;
}

PyObject* AssemblyObjectPy::clearUndo(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    this->getAssemblyObjectPtr()->clearUndo();
    Py_Return;
}

PyObject* AssemblyObjectPy::isPartConnected(PyObject* args) const
{
    PyObject* pyobj;
    // FCPROJECT-PATCH (2026-10-03, Diagnose BG67-"Part_Detached"-Overlay): optionales zweites
    // Argument schaltet das bereits vorhandene verboseLog von isPartConnected() aus Python frei.
    int verboseLog = 0;

    if (!PyArg_ParseTuple(args, "O!|p", &(App::DocumentObjectPy::Type), &pyobj, &verboseLog)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    bool ok = this->getAssemblyObjectPtr()->isPartConnected(obj, verboseLog != 0);
    return Py_BuildValue("O", (ok ? Py_True : Py_False));
}

PyObject* AssemblyObjectPy::exportIdentityGraphDot(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    std::string dot = this->getAssemblyObjectPtr()->exportIdentityGraphDot();
    return Py_BuildValue("s", dot.c_str());
}

PyObject* AssemblyObjectPy::verifyIdentityGraphAgainstAsmt(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    std::string report = this->getAssemblyObjectPtr()->verifyIdentityGraphAgainstAsmt();
    return Py_BuildValue("s", report.c_str());
}

PyObject* AssemblyObjectPy::isPartGrounded(PyObject* args) const
{
    PyObject* pyobj;

    if (!PyArg_ParseTuple(args, "O!", &(App::DocumentObjectPy::Type), &pyobj)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    bool ok = this->getAssemblyObjectPtr()->isPartGrounded(obj);
    return Py_BuildValue("O", (ok ? Py_True : Py_False));
}

PyObject* AssemblyObjectPy::isJointConnectingPartToGround(PyObject* args) const
{
    PyObject* pyobj;
    char* pname;

    if (!PyArg_ParseTuple(args, "O!s", &(App::DocumentObjectPy::Type), &pyobj, &pname)) {
        return nullptr;
    }
    auto* obj = static_cast<App::DocumentObjectPy*>(pyobj)->getDocumentObjectPtr();
    bool ok = this->getAssemblyObjectPtr()->isJointConnectingPartToGround(obj, pname);
    return Py_BuildValue("O", (ok ? Py_True : Py_False));
}

PyObject* AssemblyObjectPy::exportAsASMT(PyObject* args) const
{
    char* utf8Name;
    if (!PyArg_ParseTuple(args, "et", "utf-8", &utf8Name)) {
        return nullptr;
    }

    std::string fileName = utf8Name;
    PyMem_Free(utf8Name);

    if (fileName.empty()) {
        PyErr_SetString(PyExc_ValueError, "Passed string is empty");
        return nullptr;
    }

    this->getAssemblyObjectPtr()->exportAsASMT(fileName);

    Py_Return;
}

PyObject* AssemblyObjectPy::verifyIdentityGraphEquivalence(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    std::vector<std::string> lines = this->getAssemblyObjectPtr()->verifyIdentityGraphEquivalence();

    Py::List ret;
    for (const auto& line : lines) {
        ret.append(Py::String(line));
    }
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::verifyIdentityGraphHandleConsistency(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    std::vector<std::string> lines
        = this->getAssemblyObjectPtr()->verifyIdentityGraphHandleConsistency();

    Py::List ret;
    for (const auto& line : lines) {
        ret.append(Py::String(line));
    }
    return Py::new_reference_to(ret);
}

Py::List AssemblyObjectPy::getJoints() const
{
    Py::List ret;
    // FCPROJECT-PATCH (Migrationsschritt 3 "Adressieren statt Kopieren", siehe
    // docs/ARCHITECTURE.md Abschnitt 5): getJoints() liefert seit diesem Schritt vector<JointRef>
    // statt vector<DocumentObject*> - die Python-Signatur bleibt unveraendert (Liste von
    // Joint-Objekten), nestingPrefix wird hier (noch) nicht gebraucht.
    std::vector<App::DocumentObject*> list
        = Assembly::extractJointObjects(getAssemblyObjectPtr()->getJoints());

    for (auto It : list) {
        ret.append(Py::Object(It->getPyObject(), true));
    }

    return ret;
}

PyObject* AssemblyObjectPy::getDownstreamParts(PyObject* args) const
{
    PyObject* pyPart;
    PyObject* pyJoint;
    PyObject* pyAlsoIgnore = nullptr;

    // Parse the arguments: a part object, a joint object and (FCPROJECT-PATCH 2026-10-11)
    // optionally a list of further joints to ignore
    if (!PyArg_ParseTuple(
            args,
            "O!O!|O!",
            &(App::DocumentObjectPy::Type),
            &pyPart,
            &(App::DocumentObjectPy::Type),
            &pyJoint,
            &PyList_Type,
            &pyAlsoIgnore
        )) {
        return nullptr;
    }

    auto* part = static_cast<App::DocumentObjectPy*>(pyPart)->getDocumentObjectPtr();
    auto* joint = static_cast<App::DocumentObjectPy*>(pyJoint)->getDocumentObjectPtr();
    std::vector<App::DocumentObject*> alsoIgnore;
    if (pyAlsoIgnore) {
        for (Py_ssize_t i = 0; i < PyList_Size(pyAlsoIgnore); i++) {
            PyObject* item = PyList_GetItem(pyAlsoIgnore, i);
            if (!PyObject_TypeCheck(item, &(App::DocumentObjectPy::Type))) {
                PyErr_SetString(PyExc_TypeError, "Expected a list of App.DocumentObject");
                return nullptr;
            }
            alsoIgnore.push_back(static_cast<App::DocumentObjectPy*>(item)->getDocumentObjectPtr());
        }
    }

    // Call the C++ method
    std::vector<Assembly::ObjRef> downstreamParts
        = this->getAssemblyObjectPtr()->getDownstreamParts(part, joint, alsoIgnore);

    // Convert the result into a Python list of DocumentObjects
    Py::List ret;
    for (const auto& objRef : downstreamParts) {
        if (objRef.obj) {
            ret.append(Py::Object(objRef.obj->getPyObject(), true));
        }
    }

    return Py::new_reference_to(ret);
}

// FCPROJECT-PATCH (2026-10-04, siehe feedback-drag-test-strategy-needed.md): die Suche nach der
// Ziehart (ViewProviderAssembly::findDragMode()) headless nachvollziehbar machen.
// FCPROJECT-PATCH (2026-10-10, Plan plans/2026-10-10-simulation-an-struktur-anpassen.md,
// Schritt 3/4): Gelenke je Instanz samt Verschachtelungspfad fuer die Motion-Auswahl.
PyObject* AssemblyObjectPy::getJointInstances(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    Py::List ret;
    for (const auto& jr : this->getAssemblyObjectPtr()->getJoints()) {
        if (!jr.joint) {
            continue;
        }
        Py::Tuple item(2);
        item.setItem(0, Py::Object(jr.joint->getPyObject(), true));
        item.setItem(1, Py::String(jr.nestingPrefix));
        ret.append(item);
    }
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::getJointOfPartConnectingToGround(PyObject* args) const
{
    PyObject* pyPart;
    if (!PyArg_ParseTuple(args, "O!", &(App::DocumentObjectPy::Type), &pyPart)) {
        return nullptr;
    }
    auto* part = static_cast<App::DocumentObjectPy*>(pyPart)->getDocumentObjectPtr();
    std::string name;
    std::string prefix;
    App::DocumentObject* joint
        = this->getAssemblyObjectPtr()->getJointOfPartConnectingToGround(part, name, {}, &prefix);
    Py::Tuple ret(3);
    ret.setItem(0, joint ? Py::Object(joint->getPyObject(), true) : Py::None());
    ret.setItem(1, Py::String(name));
    ret.setItem(2, Py::String(prefix));
    return Py::new_reference_to(ret);
}

PyObject* AssemblyObjectPy::getUpstreamMovingPart(PyObject* args) const
{
    PyObject* pyPart;
    if (!PyArg_ParseTuple(args, "O!", &(App::DocumentObjectPy::Type), &pyPart)) {
        return nullptr;
    }
    auto* part = static_cast<App::DocumentObjectPy*>(pyPart)->getDocumentObjectPtr();
    App::DocumentObject* joint = nullptr;
    std::string name;
    App::DocumentObject* upPart = this->getAssemblyObjectPtr()->getUpstreamMovingPart(part, joint, name);
    Py::Tuple ret(3);
    ret.setItem(0, upPart ? Py::Object(upPart->getPyObject(), true) : Py::None());
    ret.setItem(1, joint ? Py::Object(joint->getPyObject(), true) : Py::None());
    ret.setItem(2, Py::String(name));
    return Py::new_reference_to(ret);
}

// FCPROJECT-PATCH (2026-10-11): ueberbestimmte Gruppen des letzten Solves, je Gruppe eine Zeile
// "Verursacher (Typ), zusammen mit: ..." (siehe AssemblyObject::updateSolveStatus()).
PyObject* AssemblyObjectPy::getRedundancyGroups(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    Py::List ret;
    for (const auto& group : this->getAssemblyObjectPtr()->getLastRedundancyGroups()) {
        ret.append(Py::String(group));
    }
    return Py::new_reference_to(ret);
}

// FCPROJECT-PATCH (2026-10-14): herausgenommene (redundante) Bedingungen des letzten Solves mit
// Abweichung und "geschluckt"/"Konflikt" (siehe AssemblyObject::updateSolveStatus(), Regel C7).
PyObject* AssemblyObjectPy::getRedundancyStates(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    Py::List ret;
    for (const auto& line : this->getAssemblyObjectPtr()->getLastRedundancyStates()) {
        ret.append(Py::String(line));
    }
    return Py::new_reference_to(ret);
}

// FCPROJECT-PATCH (2026-10-13): Gelenkgrenzen, die der letzte Solve nicht einhalten konnte, je
// Grenze eine Zeile (siehe AssemblyObject::reportUnsatisfiedLimits()).
PyObject* AssemblyObjectPy::getLimitViolations(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }
    Py::List ret;
    for (const auto& line : this->getAssemblyObjectPtr()->getLastLimitViolations()) {
        ret.append(Py::String(line));
    }
    return Py::new_reference_to(ret);
}

// FCPROJECT-PATCH (2026-10-12): Diagnose/Test fuer findDragMode() - siehe
// AssemblyObject::getDragJointOfPart().
PyObject* AssemblyObjectPy::getDragJointOfPart(PyObject* args) const
{
    PyObject* pyPart;
    if (!PyArg_ParseTuple(args, "O!", &(App::DocumentObjectPy::Type), &pyPart)) {
        return nullptr;
    }
    auto* part = static_cast<App::DocumentObjectPy*>(pyPart)->getDocumentObjectPtr();
    App::DocumentObject* joint = nullptr;
    std::string name;
    std::vector<App::DocumentObject*> bypassed;
    App::DocumentObject* dragPart
        = this->getAssemblyObjectPtr()->getDragJointOfPart(part, joint, name, bypassed);
    Py::List pyBypassed;
    for (auto* b : bypassed) {
        pyBypassed.append(Py::Object(b->getPyObject(), true));
    }
    Py::Tuple ret(4);
    ret.setItem(0, dragPart ? Py::Object(dragPart->getPyObject(), true) : Py::None());
    ret.setItem(1, joint ? Py::Object(joint->getPyObject(), true) : Py::None());
    ret.setItem(2, Py::String(name));
    ret.setItem(3, pyBypassed);
    return Py::new_reference_to(ret);
}

// FCPROJECT-PATCH (2026-09-23, siehe feedback-drag-test-strategy-needed.md): preDrag()/
// doDragStep()/postDrag() nach Python exportiert, damit sich ein interaktiver Zug headless
// nachstellen laesst (Placement der dragParts-Objekte manuell um ein festes Delta verschieben,
// dann doDragStep() aufrufen - genau das, was tryMouseMove()/ViewProviderAssembly.cpp sonst pro
// Mausereignis tut), statt jedes Mal einen Live-Test mit echter Maus in der GUI zu brauchen.
PyObject* AssemblyObjectPy::preDrag(PyObject* args) const
{
    PyObject* pyList;
    if (!PyArg_ParseTuple(args, "O!", &PyList_Type, &pyList)) {
        return nullptr;
    }

    std::vector<App::DocumentObject*> dragParts;
    Py_ssize_t size = PyList_Size(pyList);
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject* item = PyList_GetItem(pyList, i);
        if (!PyObject_TypeCheck(item, &(App::DocumentObjectPy::Type))) {
            PyErr_SetString(PyExc_TypeError, "Expected a list of App.DocumentObject");
            return nullptr;
        }
        dragParts.push_back(static_cast<App::DocumentObjectPy*>(item)->getDocumentObjectPtr());
    }

    this->getAssemblyObjectPtr()->preDrag(dragParts);
    Py_Return;
}

PyObject* AssemblyObjectPy::doDragStep(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    this->getAssemblyObjectPtr()->doDragStep();
    Py_Return;
}

PyObject* AssemblyObjectPy::postDrag(PyObject* args) const
{
    if (!PyArg_ParseTuple(args, "")) {
        return nullptr;
    }

    this->getAssemblyObjectPtr()->postDrag();
    Py_Return;
}
