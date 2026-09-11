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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>


#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObjectGroup.h>
#include <App/FeaturePythonPyImp.h>
#include <App/Link.h>
#include <App/PropertyLinks.h>
#include <App/PropertyPythonObject.h>
#include <Base/Console.h>
#include <Base/Placement.h>
#include <Base/Rotation.h>
#include <Base/Tools.h>
#include <Base/Interpreter.h>

#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/TopoShape.h>
#include <Mod/PartDesign/App/Body.h>
#include <Mod/Part/App/DatumFeature.h>

#include "AssemblyObject.h"
#include "AssemblyUtils.h"
#include "Groups.h"

#include "AssemblyLink.h"
#include "AssemblyLinkPy.h"

namespace PartApp = Part;

using namespace Assembly;

// ================================ Assembly Object ============================

PROPERTY_SOURCE(Assembly::AssemblyLink, App::Part)

bool AssemblyLink::updatingContents = false;

AssemblyLink::AssemblyLink()
{
    ADD_PROPERTY_TYPE(
        Rigid,
        (true),
        "General",
        (App::PropertyType)(App::Prop_None),
        "If the sub-assembly is set to Rigid, it will act "
        "as a rigid body. Else its joints will be taken into account."
    );

    ADD_PROPERTY_TYPE(
        LinkedObject,
        (nullptr),
        "General",
        (App::PropertyType)(App::Prop_None),
        "The linked assembly."
    );
}

AssemblyLink::~AssemblyLink() = default;

PyObject* AssemblyLink::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new AssemblyLinkPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

App::DocumentObjectExecReturn* AssemblyLink::execute()
{
    updateContents();

    return App::Part::execute();
}

void AssemblyLink::onChanged(const App::Property* prop)
{
    if (App::GetApplication().isRestoring()) {
        App::Part::onChanged(prop);
        return;
    }

    if (prop == &Group) {
        for (auto* obj : getInList()) {
            if (auto* assemblyLink = freecad_cast<AssemblyLink*>(obj)) {
                assemblyLink->updateContents();
            }
        }
    }

    if (prop == &Rigid) {
        Base::Placement movePlc;

        // A flexible sub-assembly cannot be grounded.
        // If a rigid sub-assembly has an object that is grounded, we also remove it.
        auto groundedJoints = getParentAssembly()->getGroundedJoints();
        for (auto* joint : groundedJoints) {
            auto* propObj = dynamic_cast<App::PropertyLink*>(
                joint->getPropertyByName("ObjectToGround")
            );
            if (!propObj) {
                continue;
            }
            auto* groundedObj = propObj->getValue();
            if (auto* linkElt = dynamic_cast<App::LinkElement*>(groundedObj)) {
                // hasObject does not handle link groups so we must handle it manually.
                groundedObj = linkElt->getLinkGroup();
            }

            if (Rigid.getValue() ? hasObject(groundedObj) : groundedObj == this) {
                getDocument()->removeObject(joint->getNameInDocument());
            }
        }

        if (Rigid.getValue()) {
            // movePlc needs to be computed before updateContents.
            App::DocumentObject* firstLink = nullptr;
            for (auto* obj : Group.getValues()) {
                if (obj && (obj->isDerivedFrom<App::Link>() || obj->isDerivedFrom<AssemblyLink>())) {
                    firstLink = obj;
                    break;
                }
            }

            if (firstLink) {
                App::DocumentObject* sourceObj = nullptr;
                if (auto* link = dynamic_cast<App::Link*>(firstLink)) {
                    sourceObj = link->getLinkedObject(false);  // Get non-recursive linked object
                }
                else if (auto* asmLink = dynamic_cast<AssemblyLink*>(firstLink)) {
                    sourceObj = asmLink->getLinkedAssembly();
                }

                if (sourceObj) {
                    auto* propSource = dynamic_cast<App::PropertyPlacement*>(
                        sourceObj->getPropertyByName("Placement")
                    );
                    auto* propLink = dynamic_cast<App::PropertyPlacement*>(
                        firstLink->getPropertyByName("Placement")
                    );

                    if (propSource && propLink) {
                        movePlc = propLink->getValue() * propSource->getValue().inverse();
                    }
                }
            }
        }

        updateContents();

        auto* propPlc = dynamic_cast<App::PropertyPlacement*>(getPropertyByName("Placement"));
        if (!propPlc) {
            return;
        }

        if (!Rigid.getValue()) {
            // when the assemblyLink becomes flexible, we need to make sure its placement is
            // identity or it's going to mess up moving parts placement within.
            Base::Placement plc = propPlc->getValue();
            if (!plc.isIdentity()) {
                propPlc->setValue(Base::Placement());

                // We need to apply the placement of the assembly link to the children or they will
                // move.
                std::vector<App::DocumentObject*> group = Group.getValues();
                for (auto* obj : group) {
                    if (!obj->isDerivedFrom<App::Part>() && !obj->isDerivedFrom<PartApp::Feature>()
                        && !obj->isDerivedFrom<App::Link>()) {
                        continue;
                    }

                    if (obj->isLinkGroup()) {
                        auto* srcLink = static_cast<App::Link*>(obj);
                        const std::vector<App::DocumentObject*> srcElements
                            = srcLink->ElementList.getValues();

                        for (auto elt : srcElements) {
                            if (!elt) {
                                continue;
                            }

                            auto* prop = dynamic_cast<App::PropertyPlacement*>(
                                elt->getPropertyByName("Placement")
                            );
                            if (prop) {
                                prop->setValue(plc * prop->getValue());
                            }
                        }
                    }
                    else {
                        auto* prop = dynamic_cast<App::PropertyPlacement*>(
                            obj->getPropertyByName("Placement")
                        );
                        if (prop) {
                            prop->setValue(plc * prop->getValue());
                        }
                    }
                }

                AssemblyObject::redrawJointPlacements(getJoints());
            }
        }
        else {
            // For the assemblylink not to move to origin, we need to update its placement.
            if (!movePlc.isIdentity()) {
                propPlc->setValue(movePlc);
            }
        }
        updateParentJoints();
        return;
    }
    App::Part::onChanged(prop);
}

void AssemblyLink::onDocumentRestored()
{
    App::Part::onDocumentRestored();
    updateContents();
}

void AssemblyLink::updateParentJoints()
{
    AssemblyObject* parent = getParentAssembly();
    if (!parent) {
        return;
    }

    bool rigid = Rigid.getValue();
    // Iterate joints in the immediate parent assembly only (recursive=false)
    for (auto* joint : parent->getJoints(false, false)) {
        for (const char* refName : {"Reference1", "Reference2"}) {
            auto* prop = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName(refName));
            if (!prop) {
                continue;
            }
            App::DocumentObject* refObj = prop->getValue();
            if (!refObj) {
                continue;
            }

            if (rigid) {  // Flexible -> Rigid
                if (hasObject(refObj)) {
                    // The joint currently points to a child (refObj) inside this AssemblyLink.
                    // We must repoint it to 'this' and prepend the child's name to the sub-elements.
                    std::vector<std::string> subs = prop->getSubValues();
                    std::vector<std::string> newSubs;
                    std::string prefix = refObj->getNameInDocument();
                    prefix += ".";
                    for (const auto& s : subs) {
                        newSubs.push_back(prefix + s);
                    }
                    prop->setValue(this);
                    prop->setSubValues(std::move(newSubs));
                }
            }
            else {  // Rigid -> Flexible
                if (refObj == this) {
                    // The joint currently points to 'this'.
                    // We must extract the child's name from the sub-element, point to the child,
                    // and strip the prefix.
                    std::vector<std::string> subs = prop->getSubValues();
                    if (subs.empty()) {
                        continue;
                    }
                    std::vector<std::string> parts = Base::Tools::splitSubName(subs[0]);
                    if (parts.empty()) {
                        continue;
                    }
                    std::string childName = parts[0];
                    App::DocumentObject* child = getDocument()->getObject(childName.c_str());
                    if (child && hasObject(child)) {
                        std::vector<std::string> newSubs;
                        size_t prefixLen = childName.length() + 1;  // "Name."
                        for (const auto& s : subs) {
                            if (s.length() >= prefixLen) {
                                newSubs.push_back(s.substr(prefixLen));
                            }
                            else {
                                newSubs.push_back(s);
                            }
                        }
                        prop->setValue(child);
                        prop->setSubValues(std::move(newSubs));
                    }
                }
            }
        }
        if (joint->isTouched()) {
            joint->recomputeFeature();
        }
    }
}

void AssemblyLink::updateContents()
{
    // See the `updatingContents` declaration in the header for why this guard is needed and
    // why it has to be shared across all AssemblyLink instances rather than per-instance.
    if (updatingContents) {
        return;
    }
    Base::StateLocker guard(updatingContents, true);

    synchronizeComponents();

    if (isRigid()) {
        ensureNoJointGroup();
    }
    else {
        synchronizeJoints();
        // FCPROJECT-PATCH: DEAKTIVIERT (2026-08-22) - hat beim Loeschen einer verschachtelten
        // AssemblyLink einen FreeCAD-Absturz ausgeloest (Endlosschleife: Log zeigt hunderte
        // abwechselnde "alreadyMirrored=1"/"Could not map..."-Zeilen kurz vor dem Crash).
        // Vermutete Ursache: die Python-Aufrufe hier (Joint anlegen/loeschen) loesen beim
        // Loeschen einer Baugruppe eine Reentrancy-Kaskade aus, die vom bestehenden
        // updatingContents-Guard nicht abgefangen wird. Siehe patches/README.md fuer den vollen
        // Befund - nicht wieder aktivieren, ohne das Reentrancy-Problem zuerst zu loesen.
        // synchronizeGroundedAndRigidJoints();
    }
    purgeTouched();
}

void AssemblyLink::synchronizeComponents()
{
    App::Document* doc = getDocument();

    AssemblyObject* assembly = getLinkedAssembly();
    if (!assembly) {
        return;
    }

    objLinkMap.clear();

    std::vector<App::DocumentObject*> assemblyGroup = assembly->Group.getValues();
    std::vector<App::DocumentObject*> assemblyLinkGroup = Group.getValues();

    // Filter out child objects from Part-workbench features to get only top-level components.
    // An object is considered a child if it's referenced by another object's 'Base', 'Tool',
    // or 'Shapes' property within the same group.
    std::set<App::DocumentObject*> children;
    for (auto* obj : assemblyGroup) {
        if (auto* partFeat = dynamic_cast<PartApp::Feature*>(obj)) {
            if (auto* prop = dynamic_cast<App::PropertyLink*>(partFeat->getPropertyByName("Base"))) {
                if (prop->getValue()) {
                    children.insert(prop->getValue());
                }
            }
            if (auto* prop = dynamic_cast<App::PropertyLink*>(partFeat->getPropertyByName("Tool"))) {
                if (prop->getValue()) {
                    children.insert(prop->getValue());
                }
            }
            if (auto* prop
                = dynamic_cast<App::PropertyLinkList*>(partFeat->getPropertyByName("Shapes"))) {
                for (auto* shapeObj : prop->getValues()) {
                    children.insert(shapeObj);
                }
            }
        }
    }

    std::vector<App::DocumentObject*> topLevelComponents;
    std::copy_if(
        assemblyGroup.begin(),
        assemblyGroup.end(),
        std::back_inserter(topLevelComponents),
        [&children](App::DocumentObject* obj) { return children.find(obj) == children.end(); }
    );

    // We check if a component needs to be added to the AssemblyLink
    for (auto* obj : topLevelComponents) {
        if (!obj->isDerivedFrom<App::Part>() && !obj->isDerivedFrom<PartApp::Feature>()
            && !obj->isDerivedFrom<App::Link>()) {
            continue;
        }

        // Note, the user can have nested sub-assemblies.
        // In which case we need to add an AssemblyLink and not a Link.
        App::DocumentObject* link = nullptr;
        bool found = false;
        std::set<App::Link*> linkGroupsAdded;

        for (auto* obj2 : assemblyLinkGroup) {
            App::DocumentObject* linkedObj;

            auto* subAsmLink = freecad_cast<AssemblyLink*>(obj2);
            auto* link2 = dynamic_cast<App::Link*>(obj2);

            if (subAsmLink) {
                linkedObj = subAsmLink->getLinkedObject2(false);  // not recursive
            }
            else if (link2) {
                if (obj->isLinkGroup() && link2->isLinkGroup()) {
                    auto* srcLink = static_cast<App::Link*>(obj);
                    if ((srcLink->getTrueLinkedObject(false) == link2->getTrueLinkedObject(false))
                        && link2->ElementCount.getValue() == srcLink->ElementCount.getValue()
                        && linkGroupsAdded.find(srcLink) == linkGroupsAdded.end()) {
                        found = true;
                        link = obj2;
                        // In case where there are more than 2 link groups with the
                        // same number of elements.
                        linkGroupsAdded.insert(srcLink);

                        const std::vector<App::DocumentObject*> srcElements
                            = srcLink->ElementList.getValues();
                        const std::vector<App::DocumentObject*> newElements
                            = link2->ElementList.getValues();
                        for (size_t i = 0; i < srcElements.size(); ++i) {
                            objLinkMap[srcElements[i]] = newElements[i];
                        }
                        break;
                    }
                }
                else if (obj->isLinkGroup() && !link2->isLinkGroup()) {
                    continue;  // make sure we migrate sub assemblies that had link to linkgroups
                }
                linkedObj = link2->getLinkedObject(false);  // not recursive
            }
            else {
                // We consider only Links and AssemblyLinks
                continue;
            }

            if (linkedObj == obj) {
                found = true;
                link = obj2;
                break;
            }
        }
        if (!found) {
            // Add a link or a AssemblyLink to it in the AssemblyLink.
            if (obj->isDerivedFrom<AssemblyLink>()) {
                auto* asmLink = static_cast<AssemblyLink*>(obj);

                App::DocumentObject* newObj
                    = doc->addObject("Assembly::AssemblyLink", obj->getNameInDocument());
                auto* subAsmLink = static_cast<AssemblyLink*>(newObj);
                subAsmLink->LinkedObject.setValue(obj);
                subAsmLink->Rigid.setValue(asmLink->Rigid.getValue());
                subAsmLink->Label.setValue(obj->Label.getValue());
                addObject(subAsmLink);
                link = subAsmLink;
            }
            else if (obj->isDerivedFrom<App::Link>() && obj->isLinkGroup()) {
                auto* srcLink = static_cast<App::Link*>(obj);

                auto* newLink = static_cast<App::Link*>(
                    doc->addObject("App::Link", obj->getNameInDocument())
                );
                newLink->LinkedObject.setValue(srcLink->getTrueLinkedObject(false));

                newLink->Label.setValue(obj->Label.getValue());
                addObject(newLink);

                newLink->ElementCount.setValue(srcLink->ElementCount.getValue());
                const std::vector<App::DocumentObject*> srcElements = srcLink->ElementList.getValues();
                const std::vector<App::DocumentObject*> newElements = newLink->ElementList.getValues();
                for (size_t i = 0; i < srcElements.size(); ++i) {
                    auto* newObj = newElements[i];
                    auto* srcObj = srcElements[i];
                    if (newObj && srcObj) {
                        syncPlacements(srcObj, newObj);
                    }
                    objLinkMap[srcObj] = newObj;
                }

                link = newLink;
            }
            else {
                App::DocumentObject* newObj = doc->addObject("App::Link", obj->getNameInDocument());
                auto* newLink = static_cast<App::Link*>(newObj);
                newLink->LinkedObject.setValue(obj);
                newLink->Label.setValue(obj->Label.getValue());
                addObject(newLink);
                link = newLink;
            }
        }

        objLinkMap[obj] = link;
    }

    // If the assemblyLink is rigid, then we keep all placements synchronized.
    if (isRigid()) {
        for (const auto& [sourceObj, linkObj] : objLinkMap) {
            syncPlacements(sourceObj, linkObj);
        }
    }

    // We check if a component needs to be removed from the AssemblyLink
    // NOTE: this is not being executed when a src link is deleted, because the link
    // is then in error, and so AssemblyLink::execute() does not get called.
    std::set<App::DocumentObject*> validLinks;
    for (const auto& pair : objLinkMap) {
        validLinks.insert(pair.second);
    }
    for (auto* obj : assemblyLinkGroup) {
        // We don't need to update assemblyLinkGroup after the addition since we're not removing
        // something we just added.
        if (!obj->isDerivedFrom<App::Part>() && !obj->isDerivedFrom<PartApp::Feature>()
            && !obj->isDerivedFrom<App::Link>()) {
            continue;
        }
        if (validLinks.find(obj) == validLinks.end()) {
            doc->removeObject(obj->getNameInDocument());
        }
    }
}

namespace
{
template<typename T>
void copyPropertyIfDifferent(
    App::DocumentObject* source,
    App::DocumentObject* target,
    const char* propertyName
)
{
    auto sourceProp = freecad_cast<T*>(source->getPropertyByName(propertyName));
    auto targetProp = freecad_cast<T*>(target->getPropertyByName(propertyName));
    if (sourceProp && targetProp && sourceProp->getValue() != targetProp->getValue()) {
        targetProp->setValue(sourceProp->getValue());
    }
}

[[maybe_unused]] std::string removeUpToName(const std::string& sub, const std::string& name)
{
    size_t pos = sub.find(name);
    if (pos != std::string::npos) {
        // Move the position to the character after the found substring and the following '.'
        pos += name.length() + 1;
        if (pos < sub.length()) {
            return sub.substr(pos);
        }
    }
    // If s2 is not found in s1, return the original string
    return sub;
}

[[maybe_unused]] std::string replaceLastOccurrence(
    const std::string& str,
    const std::string& oldStr,
    const std::string& newStr
)
{
    size_t pos = str.rfind(oldStr);
    if (pos != std::string::npos) {
        std::string result = str;
        result.replace(pos, oldStr.length(), newStr);
        return result;
    }
    return str;
}
};  // namespace

void AssemblyLink::synchronizeJoints()
{
    App::Document* doc = getDocument();
    AssemblyObject* assembly = getLinkedAssembly();
    if (!assembly) {
        return;
    }

    JointGroup* jGroup = ensureJointGroup();

    // FCPROJECT-PATCH (2026-08-28, "kleiner erster Schritt" aus
    // patches/assembly-architecture-overview.md, Abschnitt "Fix-Konzept: Adressieren statt
    // Kopieren"): subJoints war hier zwischenzeitlich auf true gesetzt (statt false), damit ein
    // Joint aus einer ENKEL-Baugruppe wenigstens strukturell sichtbar wurde (fuer die
    // isMbDJointValid()-Diagnose in AssemblyObject.cpp) - OHNE ihn tatsaechlich solvebar zu
    // machen.
    //
    // FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix): auf
    // false ZURUECKgesetzt - der urspruengliche, vor dem "kleinen ersten Schritt" jahrelang
    // stabile Wert. Grund: AssemblyObject::getJoints()s subJoints-Zweig wurde in Teilschritt 2 von
    // "liest die HIER (assembly-lokal) bereits kopierten Joints der eigenen Unter-AssemblyLinks"
    // auf "steigt rekursiv in die ECHTE verlinkte AssemblyObject-Instanz jeder Unter-AssemblyLink
    // ab, beliebig tief" umgestellt. Mit subJoints=true wuerde DIESER Aufruf hier jetzt beliebig
    // tief in 'assembly's eigene Unter-Baugruppen hinabsteigen und deren ORIGINAL-Joints
    // zurueckliefern - deren Reference1/2 zeigen auf Objekte in einem KOMPLETT ANDEREN Dokument
    // (der jeweils tiefsten Unter-Baugruppe), fuer die findLocalAncestor() (das die Struktur-
    // Eltern-Kette IM SELBEN Dokument hochlaeuft) prinzipiell nicht ausgelegt ist - ein neues,
    // hier nie getestetes Fehlerbild in einer Codezone mit zweifacher Absturzhistorie, fuer keinen
    // erkennbaren Nutzen: solve() erreicht tiefe Joints seit Teilschritt 2 bereits ueber einen
    // komplett EIGENSTAENDIGEN Pfad (AssemblyObject::getJoints() direkt, nicht ueber diese
    // Kopier-Pipeline) - der urspruengliche Grund fuer 'true' hier (Diagnose-Sichtbarkeit in
    // isMbDJointValid()) ist damit gegenstandslos geworden. Diese Kopier-Pipeline
    // (synchronizeJoints()/handleJointReference()/findLocalAncestor()) bleibt bewusst unangetastet
    // - sie ist fuer eine spaetere, separate Aufraeum-Sitzung vorgesehen, nicht Teil dieses
    // Teilschritts.
    std::vector<App::DocumentObject*> assemblyJoints = assembly->getJoints(false, false);
    std::vector<App::DocumentObject*> assemblyLinkJoints = getJoints();

    // We delete the excess of joints if any
    for (size_t i = assemblyJoints.size(); i < assemblyLinkJoints.size(); ++i) {
        doc->removeObject(assemblyLinkJoints[i]->getNameInDocument());
    }

    // We make sure the joints match.
    for (size_t i = 0; i < assemblyJoints.size(); ++i) {
        App::DocumentObject* joint = assemblyJoints[i];
        App::DocumentObject* lJoint;
        if (i < assemblyLinkJoints.size()) {
            lJoint = assemblyLinkJoints[i];
        }
        else {
            auto ret = doc->copyObject({joint});
            if (ret.size() != 1) {
                continue;
            }
            lJoint = ret[0];
            jGroup->addObject(lJoint);
        }

        // Then we have to check the properties one by one.
        copyPropertyIfDifferent<App::PropertyBool>(joint, lJoint, "Suppressed");
        copyPropertyIfDifferent<App::PropertyFloat>(joint, lJoint, "Distance");
        copyPropertyIfDifferent<App::PropertyFloat>(joint, lJoint, "Distance2");
        copyPropertyIfDifferent<App::PropertyEnumeration>(joint, lJoint, "JointType");
        copyPropertyIfDifferent<App::PropertyPlacement>(joint, lJoint, "Offset1");
        copyPropertyIfDifferent<App::PropertyPlacement>(joint, lJoint, "Offset2");

        copyPropertyIfDifferent<App::PropertyBool>(joint, lJoint, "Detach1");
        copyPropertyIfDifferent<App::PropertyBool>(joint, lJoint, "Detach2");

        copyPropertyIfDifferent<App::PropertyFloat>(joint, lJoint, "AngleMax");
        copyPropertyIfDifferent<App::PropertyFloat>(joint, lJoint, "AngleMin");
        copyPropertyIfDifferent<App::PropertyFloat>(joint, lJoint, "LengthMax");
        copyPropertyIfDifferent<App::PropertyFloat>(joint, lJoint, "LengthMin");
        copyPropertyIfDifferent<App::PropertyBool>(joint, lJoint, "EnableAngleMax");
        copyPropertyIfDifferent<App::PropertyBool>(joint, lJoint, "EnableAngleMin");
        copyPropertyIfDifferent<App::PropertyBool>(joint, lJoint, "EnableLengthMax");
        copyPropertyIfDifferent<App::PropertyBool>(joint, lJoint, "EnableLengthMin");

        // The reference needs to be handled specifically
        handleJointReference(joint, lJoint, "Reference1");
        handleJointReference(joint, lJoint, "Reference2");
    }

    assemblyLinkJoints = getJoints();

    for (auto* joint : assemblyLinkJoints) {
        joint->purgeTouched();
    }
}

// FCPROJECT-PATCH: siehe Header-Kommentar bei der Deklaration und
// patches/README.md ("GroundedJoint/RigidGroupJoint verschwindet bei verschachtelter
// flexibler Baugruppe"). Laeuft unabhaengig von synchronizeJoints() - GroundedJoint
// ("ObjectToGround", App::PropertyLinkGlobal) und RigidGroupJoint ("ObjectsToRigidGroup",
// App::PropertyLinkList) haben kein Reference1/Reference2 und damit keine sinnvolle
// Positions-Korrespondenz zu synchronizeJoints()s index-basiertem Abgleich - stattdessen wird
// hier ueber den jeweiligen ZIEL-Objekten (nicht ueber den Joint-Objekten selbst) abgeglichen:
// fuer jedes Quell-Joint wird das/die Zielobjekt(e) auf die lokale Entsprechung abgebildet
// (mapToLocalComponent(), gleiche objLinkMap/findLocalAncestor()-Technik wie
// handleJointReference()); existiert lokal noch kein Joint mit exakt dieser Zielmenge, wird
// einer neu angelegt (per Python-Interpreter-Aufruf, wie schon in
// AssemblyObject::syncGroundedJoints() fuer GroundedJoint ueblich). Nicht mehr benoetigte
// lokale Joints (Ziel nicht mehr unter den aktuell gewuenschten) werden entfernt.
void AssemblyLink::synchronizeGroundedAndRigidJoints()
{
    App::Document* doc = getDocument();
    AssemblyObject* assembly = getLinkedAssembly();
    if (!assembly) {
        return;
    }

    JointGroup* sourceGroup = assembly->getJointGroup();
    if (!sourceGroup) {
        return;
    }
    JointGroup* jGroup = ensureJointGroup();

    // ---- GroundedJoint: ein Zielobjekt pro Joint ----
    std::vector<App::DocumentObject*> existingGroundedJoints;
    std::vector<App::DocumentObject*> existingGroundedTargets;
    for (auto* obj : jGroup->getObjects()) {
        auto* prop = obj ? dynamic_cast<App::PropertyLink*>(obj->getPropertyByName("ObjectToGround"))
                          : nullptr;
        if (!prop) {
            continue;
        }
        existingGroundedJoints.push_back(obj);
        existingGroundedTargets.push_back(prop->getValue());
    }

    std::vector<App::DocumentObject*> wantedGroundedTargets;
    for (auto* sourceJoint : sourceGroup->getObjects()) {
        auto* prop = sourceJoint
            ? dynamic_cast<App::PropertyLink*>(sourceJoint->getPropertyByName("ObjectToGround"))
            : nullptr;
        if (!prop || !prop->getValue()) {
            continue;
        }

        App::DocumentObject* localTarget = mapToLocalComponent(prop->getValue());
        if (!localTarget) {
            Base::Console().warning(
                "AssemblyLink: Could not map grounded component %s to a local link\n",
                prop->getValue()->getNameInDocument()
            );
            continue;
        }
        wantedGroundedTargets.push_back(localTarget);

        bool alreadyMirrored = std::find(
                                    existingGroundedTargets.begin(),
                                    existingGroundedTargets.end(),
                                    localTarget
                                )
            != existingGroundedTargets.end();
        if (alreadyMirrored) {
            continue;
        }

        // Neuen GroundedJoint anlegen - dieselbe Technik wie
        // AssemblyObject::syncGroundedJoints() (dort fuer den lokal-nicht-verschachtelten
        // Fall), nur mit fest bekanntem Zielobjekt statt "erstes Teil mit gesperrtem
        // Placement".
        Base::PyGILStateLocker lock;
        try {
            std::string docName = doc->getName();
            std::string asmLinkName = getNameInDocument();
            std::string partName = localTarget->getNameInDocument();
            std::string code = "import FreeCAD\n"
                                "try:\n"
                                "    import JointObject\n"
                                "    import UtilsAssembly\n"
                                "    doc = FreeCAD.getDocument('"
                + docName
                + "')\n"
                  "    asmLink = doc.getObject('"
                + asmLinkName
                + "')\n"
                  "    part = doc.getObject('"
                + partName
                + "')\n"
                  "    jg = UtilsAssembly.getJointGroup(asmLink)\n"
                  "    if jg:\n"
                  "        j = jg.newObject('App::FeaturePython', 'GroundedJoint')\n"
                  "        JointObject.GroundedJoint(j, part)\n"
                  "        if hasattr(JointObject, 'ViewProviderGroundedJoint') and getattr(j, "
                  "'ViewObject', None):\n"
                  "            JointObject.ViewProviderGroundedJoint(j.ViewObject)\n"
                  "        j.recompute()\n"
                  "except Exception as e:\n"
                  "    FreeCAD.Console.PrintError(str(e) + '\\n')\n";
            Base::Interpreter().runString(code.c_str());
        }
        catch (...) {
        }
    }

    for (size_t i = 0; i < existingGroundedJoints.size(); ++i) {
        App::DocumentObject* target = existingGroundedTargets[i];
        bool stillWanted = target
            && std::find(wantedGroundedTargets.begin(), wantedGroundedTargets.end(), target)
                != wantedGroundedTargets.end();
        if (!stillWanted) {
            doc->removeObject(existingGroundedJoints[i]->getNameInDocument());
        }
    }

    // ---- RigidGroupJoint: eine Zielobjekt-MENGE pro Joint ----
    std::vector<App::DocumentObject*> existingRigidJoints;
    std::vector<std::vector<App::DocumentObject*>> existingRigidTargetSets;
    for (auto* obj : jGroup->getObjects()) {
        auto* prop = obj
            ? dynamic_cast<App::PropertyLinkList*>(obj->getPropertyByName("ObjectsToRigidGroup"))
            : nullptr;
        if (!prop) {
            continue;
        }
        existingRigidJoints.push_back(obj);
        existingRigidTargetSets.push_back(prop->getValues());
    }

    std::vector<std::vector<App::DocumentObject*>> wantedRigidTargetSets;
    for (auto* sourceJoint : sourceGroup->getObjects()) {
        auto* prop = sourceJoint ? dynamic_cast<App::PropertyLinkList*>(
                         sourceJoint->getPropertyByName("ObjectsToRigidGroup")
                     )
                                  : nullptr;
        if (!prop || prop->getValues().empty()) {
            continue;
        }

        std::vector<App::DocumentObject*> localTargets;
        bool allMapped = true;
        for (auto* sourceTarget : prop->getValues()) {
            App::DocumentObject* localTarget = mapToLocalComponent(sourceTarget);
            if (!localTarget) {
                Base::Console().warning(
                    "AssemblyLink: Could not map rigid group component %s to a local link\n",
                    sourceTarget ? sourceTarget->getNameInDocument() : "?"
                );
                allMapped = false;
                break;
            }
            localTargets.push_back(localTarget);
        }
        if (!allMapped) {
            continue;
        }
        wantedRigidTargetSets.push_back(localTargets);

        // Als Menge vergleichen (Reihenfolge soll hier keine Rolle spielen).
        auto matchesSet = [&localTargets](const std::vector<App::DocumentObject*>& other) {
            if (localTargets.size() != other.size()) {
                return false;
            }
            for (auto* t : localTargets) {
                if (std::find(other.begin(), other.end(), t) == other.end()) {
                    return false;
                }
            }
            return true;
        };
        bool alreadyMirrored = std::any_of(
            existingRigidTargetSets.begin(),
            existingRigidTargetSets.end(),
            matchesSet
        );
        if (alreadyMirrored) {
            continue;
        }

        Base::PyGILStateLocker lock;
        try {
            std::string docName = doc->getName();
            std::string asmLinkName = getNameInDocument();
            std::string srcDocName = sourceJoint->getDocument()->getName();
            std::string srcJointName = sourceJoint->getNameInDocument();
            std::string partsList;
            for (auto* t : localTargets) {
                if (!partsList.empty()) {
                    partsList += ", ";
                }
                partsList += "doc.getObject('" + std::string(t->getNameInDocument()) + "')";
            }
            // FCPROJECT-PATCH: RigidGroupJoint.__init__() berechnet RigidPlacements (die
            // relativen Placements der Mitglieder zueinander) beim Anlegen SOFORT aus deren
            // AKTUELLEM Placement - bei einer frisch gespiegelten Kopie steht das aber noch auf
            // Identitaet (0,0,0), der eigentliche Solve, der die echte Position herstellen
            // wuerde, ist zu diesem Zeitpunkt noch nicht gelaufen. Live so beobachtet (siehe
            // patches/README.md): Erdung + Rigid-Group-Zuordnung waren beide bereits korrekt,
            // die Teile blieben trotzdem alle bei Placement=Identitaet, weil RigidPlacements
            // mit lauter Nullen eingefroren wurde. Fix: die bereits korrekten relativen
            // Placements direkt von der QUELLE (wo sie im Rahmen ihres eigenen, laengst
            // erfolgreich gelaufenen Solves richtig berechnet wurden) uebernehmen, statt sie
            // hier neu (und falsch) zu berechnen - die Reihenfolge von localTargets entspricht
            // 1:1 der Reihenfolge von sourceJoint.ObjectsToRigidGroup, ein direktes Kopieren
            // ist also gueltig.
            std::string code = "import FreeCAD\n"
                                "try:\n"
                                "    import JointObject\n"
                                "    import UtilsAssembly\n"
                                "    doc = FreeCAD.getDocument('"
                + docName
                + "')\n"
                  "    asmLink = doc.getObject('"
                + asmLinkName
                + "')\n"
                  "    jg = UtilsAssembly.getJointGroup(asmLink)\n"
                  "    if jg:\n"
                  "        rg = jg.newObject('App::FeaturePython', 'RigidGroupJoint')\n"
                  "        JointObject.RigidGroupJoint(rg, ["
                + partsList
                + "])\n"
                  "        if hasattr(JointObject, 'ViewProviderRigidGroupJoint') and getattr(rg, "
                  "'ViewObject', None):\n"
                  "            JointObject.ViewProviderRigidGroupJoint(rg.ViewObject)\n"
                  "        srcJoint = FreeCAD.getDocument('"
                + srcDocName
                + "').getObject('"
                + srcJointName
                + "')\n"
                  "        if srcJoint and hasattr(srcJoint, 'RigidPlacements'):\n"
                  "            rg.setPropertyStatus('RigidPlacements', '-ReadOnly')\n"
                  "            rg.RigidPlacements = list(srcJoint.RigidPlacements)\n"
                  "            rg.setPropertyStatus('RigidPlacements', 'ReadOnly')\n"
                  "        rg.recompute()\n"
                  "except Exception as e:\n"
                  "    FreeCAD.Console.PrintError(str(e) + '\\n')\n";
            Base::Interpreter().runString(code.c_str());
        }
        catch (...) {
        }
    }

    for (size_t i = 0; i < existingRigidJoints.size(); ++i) {
        const auto& targetSet = existingRigidTargetSets[i];
        bool stillWanted = false;
        for (const auto& wantedSet : wantedRigidTargetSets) {
            if (wantedSet.size() != targetSet.size()) {
                continue;
            }
            bool allFound = true;
            for (auto* t : targetSet) {
                if (std::find(wantedSet.begin(), wantedSet.end(), t) == wantedSet.end()) {
                    allFound = false;
                    break;
                }
            }
            if (allFound) {
                stillWanted = true;
                break;
            }
        }
        if (!stillWanted) {
            doc->removeObject(existingRigidJoints[i]->getNameInDocument());
        }
    }
}

// FCPROJECT-PATCH: siehe patches/README.md ("GroundedJoint/RigidGroupJoint verschwindet bei
// verschachtelter flexibler Baugruppe"). mapToLocalComponent() findet die lokale Entsprechung
// eines externen (Quell-)Objekts - anders als findLocalAncestor() (fuer Reference1/2 mit
// Sub-Element-Pfad gedacht) wird hier ueber Objekt-IDENTITAET verglichen (kein Sub-Pfad-Konzept
// fuer einen einfachen Objekt-Link wie ObjectToGround/ObjectsToRigidGroup): jede Mirror-Kopie
// eines PDM-Teils ist selbst wieder ein App::Link, dessen rekursiv aufgeloestes Linked-Objekt
// (DocumentObject::getLinkedObject(true)) IMMER auf dieselbe letztendliche Quelldatei zeigt,
// egal auf welcher Verschachtelungsebene die Kopie sitzt. Zwei Objekte mit gleichem
// getLinkedObject(true)-Ergebnis meinen also dasselbe reale Teil, auch wenn sie in
// verschiedenen Dokumenten liegen und unterschiedliche Objekt-Pointer haben.
//
// Ablauf: zuerst die eigenen direkten Mirror-Kinder (objLinkMap-Werte) auf Identitaets-Treffer
// pruefen; findet sich dort keiner, rekursiv in jedes Kind, das selbst eine AssemblyLink ist,
// absteigen (das entspricht genau dem Weg, den auch die tatsaechliche Verschachtelung nimmt -
// eine mehrfach verschachtelte Unterbaugruppe hat selbst wieder ihre eigene objLinkMap fuer
// ihre eigenen Kinder).
App::DocumentObject* AssemblyLink::mapToLocalComponent(App::DocumentObject* externalComponent)
{
    if (!externalComponent) {
        return nullptr;
    }

    App::DocumentObject* targetRoot = externalComponent->getLinkedObject(true);
    if (!targetRoot) {
        targetRoot = externalComponent;
    }

    for (auto& entry : objLinkMap) {
        App::DocumentObject* localMirror = entry.second;
        if (!localMirror) {
            continue;
        }
        App::DocumentObject* localRoot = localMirror->getLinkedObject(true);
        if (!localRoot) {
            localRoot = localMirror;
        }
        if (localRoot == targetRoot) {
            return localMirror;
        }
    }

    for (auto& entry : objLinkMap) {
        auto* nestedLink = freecad_cast<AssemblyLink*>(entry.second);
        if (!nestedLink) {
            continue;
        }
        if (auto* found = nestedLink->mapToLocalComponent(externalComponent)) {
            return found;
        }
    }

    return nullptr;
}


void AssemblyLink::handleJointReference(
    App::DocumentObject* joint,
    App::DocumentObject* lJoint,
    const char* refName
)
{
    auto prop1 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName(refName));
    auto prop2 = dynamic_cast<App::PropertyXLinkSub*>(lJoint->getPropertyByName(refName));
    if (!prop1 || !prop2) {
        return;
    }

    // 1. Get the external component prop1 is [ExternalPart, "Sub"]
    App::DocumentObject* externalComponent = prop1->getValue();
    if (!externalComponent) {
        return;
    }

    // 2. Map to local link
    App::DocumentObject* localLink = nullptr;
    std::string subPrefix;  // nur belegt, wenn ueber findLocalAncestor() gefunden
    auto it = objLinkMap.find(externalComponent);
    if (it != objLinkMap.end()) {
        localLink = it->second;
    }
    else {
        // FCPROJECT-PATCH: siehe patches/bugreport-rigid-nested-joint-reference/README.md -
        // externalComponent ist kein direktes Kind (z.B. ein Enkelkind aus einer
        // verschachtelten Rigid=False-Unterbaugruppe) - Eltern-Kette hochlaufen statt
        // sofort aufzugeben.
        localLink = findLocalAncestor(externalComponent, subPrefix);
    }
    if (!localLink) {
        Base::Console().warning(
            "AssemblyLink: Could not map external component %s to a local link for joint %s\n",
            externalComponent->getNameInDocument(),
            joint->getNameInDocument()
        );
        return;
    }

    // 3. Set the new reference
    // The local joint now points to the local link [LocalLink, "Sub"]
    if (prop2->getValue() != localLink) {
        prop2->setValue(localLink);
    }

    // 4. Sync sub-elements
    // The sub-elements (e.g. "Body.Face1") are relative to the component.
    // Since the LocalLink points to the ExternalPart, the relative path is identical - außer
    // wenn wir gerade ueber findLocalAncestor() umgeleitet haben: dann ist localLink nicht
    // mehr die ExternalPart selbst, sondern ein Vorfahre davon, und subPrefix traegt den
    // fehlenden Zwischenpfad (z.B. "Halter." bei Reference auf "Halter.Edge34" statt "Edge34").
    std::vector<std::string> subs1 = prop1->getSubValues();
    if (!subPrefix.empty()) {
        for (auto& sub : subs1) {
            sub = subPrefix + sub;
        }
    }
    std::vector<std::string> subs2 = prop2->getSubValues();

    bool changed = false;
    if (subs1.size() != subs2.size()) {
        changed = true;
    }
    else {
        for (size_t i = 0; i < subs1.size(); ++i) {
            if (subs1[i] != subs2[i]) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        prop2->setSubValues(std::move(subs1));
    }
}

// FCPROJECT-PATCH: siehe patches/bugreport-rigid-nested-joint-reference/README.md und der
// Kommentar bei der Deklaration im Header. Laeuft von obj aus die Struktur-Eltern-Kette hoch
// (nicht getInList() blind nehmen - das kann auch Joints/andere Referenzhalter liefern, nicht
// nur den strukturellen Container; stattdessen wird unter den getInList()-Kandidaten gezielt
// der gesucht, dessen "Group"-Property obj tatsaechlich enthaelt), bis ein in objLinkMap
// bekannter Vorfahre gefunden wird. outSubPrefix erhaelt dabei den durchlaufenen Pfad als
// Punkt-getrennten Praefix (z.B. "Halter." wenn obj das Enkelkind "Halter" ist und der
// gefundene Vorfahre dessen direkter Container war) - leer, wenn nichts gefunden wurde oder
// obj selbst schon der Treffer war.
App::DocumentObject* AssemblyLink::findLocalAncestor(
    App::DocumentObject* obj,
    std::string& outSubPrefix
)
{
    outSubPrefix.clear();

    App::DocumentObject* current = obj;
    std::string prefix;
    int maxDepth = 32;

    while (current && maxDepth-- > 0) {
        App::DocumentObject* parent = nullptr;
        for (auto* candidate : current->getInList()) {
            if (!candidate) {
                continue;
            }
            auto* groupProp
                = dynamic_cast<App::PropertyLinkList*>(candidate->getPropertyByName("Group"));
            if (!groupProp) {
                continue;
            }
            const auto& members = groupProp->getValues();
            if (std::find(members.begin(), members.end(), current) != members.end()) {
                parent = candidate;
                break;
            }
        }

        if (!parent) {
            return nullptr;
        }

        const char* name = current->getNameInDocument();
        prefix = std::string(name ? name : "?") + "." + prefix;

        auto it = objLinkMap.find(parent);
        if (it != objLinkMap.end()) {
            outSubPrefix = prefix;
            return it->second;
        }

        current = parent;
    }

    return nullptr;
}

void AssemblyLink::ensureNoJointGroup()
{
    // Make sure there is no joint group
    JointGroup* jGroup = getJointGroup(this);
    if (jGroup) {
        // If there is a joint group, we delete it and its content.
        jGroup->removeObjectsFromDocument();
        getDocument()->removeObject(jGroup->getNameInDocument());
    }
}
JointGroup* AssemblyLink::ensureJointGroup()
{
    // Make sure there is a jointGroup
    JointGroup* jGroup = getJointGroup(this);
    if (!jGroup) {
        jGroup = new JointGroup();
        getDocument()->addObject(jGroup, tr("Joints").toStdString().c_str());

        std::vector<DocumentObject*> grp = Group.getValues();
        grp.insert(grp.begin(), jGroup);
        Group.setValues(grp);
    }
    return jGroup;
}

App::DocumentObject* AssemblyLink::getLinkedObject2(bool recursive) const
{
    auto* obj = LinkedObject.getValue();
    auto* assembly = freecad_cast<AssemblyObject*>(obj);
    if (assembly) {
        return assembly;
    }
    else {
        auto* assemblyLink = freecad_cast<AssemblyLink*>(obj);
        if (assemblyLink) {
            if (recursive) {
                return assemblyLink->getLinkedObject2(recursive);
            }
            else {
                return assemblyLink;
            }
        }
    }

    return nullptr;
}

AssemblyObject* AssemblyLink::getLinkedAssembly() const
{
    return freecad_cast<AssemblyObject*>(getLinkedObject2());
}

AssemblyObject* AssemblyLink::getParentAssembly() const
{
    std::vector<App::DocumentObject*> inList = getInList();
    for (auto* obj : inList) {
        auto* assembly = freecad_cast<AssemblyObject*>(obj);
        if (assembly) {
            return assembly;
        }
    }

    return nullptr;
}

bool AssemblyLink::isRigid() const
{
    auto* prop = dynamic_cast<App::PropertyBool*>(getPropertyByName("Rigid"));
    if (!prop) {
        return true;
    }
    return prop->getValue();
}

std::vector<App::DocumentObject*> AssemblyLink::getJoints()
{
    JointGroup* jointGroup = getJointGroup(this);

    if (!jointGroup) {
        return {};
    }
    return jointGroup->getJoints();
}

bool AssemblyLink::allowDuplicateLabel() const
{
    return true;
}

int AssemblyLink::numberOfComponents() const
{
    return isRigid() ? 1 : getLinkedAssembly()->numberOfComponents();
}

bool AssemblyLink::isEmpty() const
{
    return numberOfComponents() == 0;
}
