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
#include <vector>


#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObjectGroup.h>
#include <App/FeaturePythonPyImp.h>
#include <App/Link.h>
#include <App/PropertyPythonObject.h>
#include <Base/Console.h>
#include <Base/Placement.h>
#include <Base/Rotation.h>
#include <Base/Tools.h>
#include <Base/Interpreter.h>

#include <Mod/Part/App/LinkArray.h>
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

namespace
{
void syncSuppressedState(App::DocumentObject* source, App::DocumentObject* target)
{
    auto* sourceExt = source ? source->getExtension<App::SuppressibleExtension>() : nullptr;
    auto* targetExt = target ? target->getExtension<App::SuppressibleExtension>() : nullptr;
    if (sourceExt && targetExt
        && sourceExt->Suppressed.getValue() != targetExt->Suppressed.getValue()) {
        targetExt->Suppressed.setValue(sourceExt->Suppressed.getValue());
    }
}
}  // namespace

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

                // FCPROJECT-PATCH (Migrationsschritt 4.1+4.3 "Adressieren statt Kopieren", siehe
                // docs/ARCHITECTURE.md Abschnitt 5): vorher redrawJointPlacements(getJoints())
                // mit dem impliziten this->getJoints() = AssemblyLink::getJoints() - das las die
                // LOKALEN Joint-KOPIEN, deren Reference1/2 auf genau die Spiegel-Kopien zeigen,
                // die im Loop oben gerade verschoben wurden (Rigid->Flexibel-Uebergang). Seit
                // Migrationsschritt 4.3 legt die flexible updateContents() keine lokalen
                // Joint-Kopien mehr an (ensureNoJointGroup() statt synchronizeJoints()) -
                // AssemblyLink::getJoints() liefert seitdem immer leer, dieser Aufruf waere
                // sonst ein stiller, folgenloser No-Op geworden. Jetzt auf die ECHTEN Joints der
                // verlinkten AssemblyObject umgestellt (redrawJointPlacement() liest pro Joint
                // ohnehin unabhaengig die aktuellen Reference1/2-Platzierungen neu ein, ist also
                // unabhaengig davon korrekt, ob die Referenz auf eine lokale Kopie oder das echte
                // Objekt zeigt - der einzige Unterschied ist, WELCHE Joint-Menge ueberhaupt
                // durchlaufen wird).
                if (AssemblyObject* linked = getLinkedAssembly()) {
                    AssemblyObject::redrawJointPlacements(
                        extractJointObjects(linked->getJoints(false, false))
                    );
                }
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
    for (auto& jr : parent->getJoints(false, false)) {
        auto* joint = jr.joint;
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

    // FCPROJECT-PATCH (Migrationsschritt 4.3+4.5 "Adressieren statt Kopieren", siehe
    // docs/ARCHITECTURE.md Abschnitt 5, "Torwaechter-Schritt"): der Flexibel-Zweig rief hier
    // bisher synchronizeJoints() auf - legte in der EIGENEN JointGroup Kopien der Joints der
    // verlinkten Unterbaugruppe an. Seit Migrationsschritt 2 arbeitet der komplette
    // Solve-/Drag-/Sichtbarkeits-Pfad bereits direkt mit den ECHTEN Joint-Objekten
    // (canonicalizeForMbD()) - die lokalen Kopien wurden vom Solver schon lange nicht mehr
    // gelesen, nur noch fuer die Baumansicht angezeigt. Seit Migrationsschritt 4.2 zeigt
    // ViewProviderAssemblyLink::claimChildren() bei fehlender lokaler JointGroup automatisch die
    // ECHTE JointGroup der verlinkten AssemblyObject an - die Kopien sind damit auch fuer die
    // Anzeige nicht mehr noetig. Rigid- und Flexibel-Zweig tun jetzt dasselbe (vorher: nur
    // Rigid). Die frueher hier zusaetzlich aufgerufene, bereits deaktivierte
    // synchronizeGroundedAndRigidJoints() (GroundedJoint/RigidGroupJoint-Spiegelung, hatte einen
    // Reentrancy-Absturz beim Loeschen einer verschachtelten AssemblyLink ausgeloest) ist
    // Migrationsschritt 4.5 zum Opfer gefallen (ohne verbleibende Aufrufer entfernt) - siehe
    // docs/JOURNAL.md fuer die volle Absturz-Herleitung.
    ensureNoJointGroup();
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
    mirrorToSourceMap.clear();
    objSubPrefixMap.clear();

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
        if (isSuppressedLinkElement(obj)) {
            continue;
        }
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
                            if (i >= newElements.size() || !srcElements[i] || !newElements[i]) {
                                continue;
                            }
                            syncSuppressedState(srcElements[i], newElements[i]);
                            if (isSuppressedLinkElement(srcElements[i])) {
                                continue;
                            }
                            objLinkMap[srcElements[i]] = newElements[i];
                            mirrorToSourceMap[newElements[i]] = srcElements[i];
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
                    if (i >= newElements.size()) {
                        continue;
                    }
                    auto* newObj = newElements[i];
                    auto* srcObj = srcElements[i];

                    if (!newObj || !srcObj) {
                        continue;
                    }

                    syncSuppressedState(srcObj, newObj);
                    syncPlacements(srcObj, newObj);

                    if (isSuppressedLinkElement(srcObj)) {
                        continue;
                    }
                    objLinkMap[srcObj] = newObj;
                    mirrorToSourceMap[newObj] = srcObj;
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
        mirrorToSourceMap[link] = obj;

        if (auto* srcLinkArray = freecad_cast<PartApp::LinkArray*>(obj)) {
            if (srcLinkArray->ShowElement.getValue()) {
                const auto srcElements = srcLinkArray->ElementList.getValues();
                for (size_t i = 0; i < srcElements.size(); ++i) {
                    if (!srcElements[i] || isSuppressedLinkElement(srcElements[i])) {
                        continue;
                    }
                    objSubPrefixMap[srcElements[i]] = std::to_string(i) + ".";
                }
            }
        }
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

App::DocumentObject* AssemblyLink::getSourceForMirror(App::DocumentObject* mirror) const
{
    auto it = mirrorToSourceMap.find(mirror);
    return it == mirrorToSourceMap.end() ? nullptr : it->second;
}

namespace
{
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
