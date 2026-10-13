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

namespace
{
// FCPROJECT-PATCH (2026-10-09): true, wenn ein AssemblyLink oberhalb von link (in dessen
// eigenem Dokument, ueber die Group-Kette) starr ist. Siehe synchronizeComponents().
bool hasRigidAncestorLink(const App::DocumentObject* link)
{
    for (auto* parent : link->getInList()) {
        auto* parentLink = freecad_cast<AssemblyLink*>(parent);
        if (!parentLink || !parentLink->hasObject(link)) {
            continue;
        }
        if (parentLink->isRigid() || hasRigidAncestorLink(parentLink)) {
            return true;
        }
    }
    return false;
}

// FCPROJECT-PATCH (2026-10-09): das Gegenstueck eines Spiegels eine Ebene tiefer in DERSELBEN
// Instanzkette. Beispiel BG22 > BG37 > BG43 > 067: der 067-Spiegel in BG22 verweist per
// LinkedObject direkt auf BG43s 067 und ueberspringt damit BG37. Sein Gegenstueck ist aber BG37s
// 067-Spiegel - nur der liegt im selben Koordinatenrahmen wie die uebrigen BG37-Spiegel, von denen
// die Nachbarn im BG22-Spiegel ihre Lage haben. Ermittelt ueber den Eltern-Spiegel: Gegenstueck
// des Eltern-Spiegels, darin der Spiegel derselben Quelle. Ohne Eltern-Spiegel (direkt in einer
// Baugruppe eingebaut) ist es das verlinkte Objekt selbst.
App::DocumentObject* counterpartOf(AssemblyLink* link)
{
    for (auto* parent : link->getInList()) {
        auto* parentLink = freecad_cast<AssemblyLink*>(parent);
        if (!parentLink || !parentLink->hasObject(link)) {
            continue;
        }
        auto* parentCounterpart = freecad_cast<AssemblyLink*>(counterpartOf(parentLink));
        App::DocumentObject* source = parentLink->getSourceForMirror(link);
        if (parentCounterpart && source) {
            auto it = parentCounterpart->objLinkMap.find(source);
            if (it != parentCounterpart->objLinkMap.end() && it->second) {
                return it->second;
            }
        }
        break;
    }
    return link->getLinkedObject2(false);
}

// FCPROJECT-PATCH (2026-10-07): verschiebt alle Teile unterhalb von container um plc (in
// Weltkoordinaten des Containers der obersten Ebene), ohne flexible Container-Spiegel selbst zu
// veraendern. chain ist die Placement-Kette der Container zwischen der obersten Ebene und
// container. Fuer ein Teil mit lokaler Placement L gilt danach: chain * L' = plc * chain * L.
// Teile, Link-Gruppen-Elemente und rigide verschachtelte BGs (selbst ein Solver-Koerper) werden
// verschoben, flexible verschachtelte BGs rekursiv durchlaufen.
void applyPlacementKeepingContainers(
    App::DocumentObject* container,
    const Base::Placement& plc,
    const Base::Placement& chain
)
{
    auto* groupExt = container->getExtensionByType<App::GroupExtension>(true);
    if (!groupExt) {
        return;
    }
    const Base::Placement delta = chain.inverse() * plc * chain;

    auto shift = [&delta](App::DocumentObject* obj) {
        auto* prop = dynamic_cast<App::PropertyPlacement*>(obj->getPropertyByName("Placement"));
        if (prop) {
            prop->setValue(delta * prop->getValue());
        }
    };

    for (auto* obj : groupExt->Group.getValues()) {
        if (!obj) {
            continue;
        }
        if (!obj->isDerivedFrom<App::Part>() && !obj->isDerivedFrom<PartApp::Feature>()
            && !obj->isDerivedFrom<App::Link>()) {
            continue;
        }

        if (auto* subLink = freecad_cast<AssemblyLink*>(obj)) {
            if (!subLink->isRigid()) {
                applyPlacementKeepingContainers(subLink, plc, chain * subLink->Placement.getValue());
                continue;
            }
        }

        if (obj->isLinkGroup()) {
            for (auto* elt : static_cast<App::Link*>(obj)->ElementList.getValues()) {
                if (elt) {
                    shift(elt);
                }
            }
        }
        else {
            shift(obj);
        }
    }
}

// FCPROJECT-PATCH (2026-10-11, live an BG22 gefunden: BG75 flexibel eingefuegt, Solve 60s,
// Teile fliegen): Quelle der Start-Placement eines NEU angelegten Spiegels. source stammt aus
// getLinkedAssembly()->Group, bei einem verschachtelten Spiegel also aus der INNERSTEN Baugruppe
// (z.B. BG47s Original-Buchse). Die gueltige Lage dieser Instanz hat aber das Gegenstueck eine
// Ebene tiefer (BG75s eigener Buchsen-Spiegel, von BG75 geloest) - dieselbe Quelle, die der
// Starr-Zweig von synchronizeComponents() nutzt. Ohne Gegenstueck (direkt in einer Baugruppe
// eingebaut) ist source selbst richtig. objLinkMap ist Laufzeitzustand und kann leer sein, wenn
// das Gegenstueck noch nicht synchronisiert wurde; dann wird dessen Group durchsucht.
App::DocumentObject* startPlacementSource(AssemblyLink* mirrorParent, App::DocumentObject* source)
{
    auto* counterpart = freecad_cast<AssemblyLink*>(counterpartOf(mirrorParent));
    if (!counterpart) {
        return source;
    }
    auto it = counterpart->objLinkMap.find(source);
    if (it != counterpart->objLinkMap.end() && it->second) {
        return it->second;
    }
    for (auto* obj : counterpart->Group.getValues()) {
        App::DocumentObject* linked = nullptr;
        if (auto* asmLink = freecad_cast<AssemblyLink*>(obj)) {
            linked = asmLink->getLinkedObject2(false);
        }
        else if (auto* link = dynamic_cast<App::Link*>(obj); link && !link->isLinkGroup()) {
            linked = link->getLinkedObject(false);
        }
        if (linked == source) {
            return obj;
        }
    }
    return source;
}
}  // namespace

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
        // FCPROJECT-PATCH (2026-10-09, Regel C1 in docs/CONSTRAINTS.md): ein verschachtelter
        // Spiegel (Eltern-Objekt ist ein AssemblyLink, keine AssemblyObject) uebernimmt Rigid nur
        // von seiner Quelle (synchronizeComponents()). Die Umschalt-Logik unten - Erdung im
        // Eltern-Assembly entfernen, Placement in die Kinder schieben, Eltern-Joints umhaengen -
        // gilt nur fuer eine direkt in einer Baugruppe eingebaute Instanz; fuer einen Spiegel gibt
        // es kein Eltern-Assembly (getParentAssembly() == nullptr, vorher Null-Dereferenzierung).
        if (!getParentAssembly()) {
            App::Part::onChanged(prop);
            return;
        }

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
                // FCPROJECT-PATCH (2026-10-07, live an BG37 mit starr eingebauter BG43 gefunden):
                // flexible Container-Spiegel folgen seit "Solver nur auf Spiegeln" immer ihrer
                // Quelle (synchronizeComponents()). Wurde die Link-Placement hier auch in einen
                // verschachtelten flexiblen Container geschoben, setzte der naechste Recompute ihn
                // auf die Quelle zurueck - Motor und Buchse sprangen weg, die BG kollabierte.
                // Deshalb geht die Placement an den Containern vorbei in deren Teile, so dass jede
                // Weltlage erhalten bleibt (siehe applyPlacementKeepingContainers()).
                applyPlacementKeepingContainers(this, plc, Base::Placement());

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

    // FCPROJECT-PATCH (2026-10-12, live an BG22 gefunden): Regel C1 (starr/flexibel legt das
    // einbauende Dokument fest) wendet ein verschachtelter Spiegel jetzt SELBST an, bevor er
    // entscheidet, wie er seine Teile behandelt. Bisher setzte erst der Eltern-Spiegel den Wert
    // (unten, Abschnitt "Regel C1") - der Spiegel selbst wird aber VOR seinem Eltern-Objekt
    // neu berechnet und arbeitete dann mit dem veralteten Wert aus der Datei: noch "starr",
    // kopierte er die Anzeige-Lagen der inzwischen flexiblen Quelle (BG75 nach dem Umschalten,
    // Container-Versatz schon in den Teilen) und verschob den Motor in BG22 um 472mm.
    // Nur fuer Spiegel (kein Eltern-Assembly); eine direkt eingebaute Instanz entscheidet selbst.
    if (!getParentAssembly()) {
        if (auto* source = freecad_cast<AssemblyLink*>(getLinkedObject2(false))) {
            if (source->Rigid.getValue() != Rigid.getValue()) {
                Rigid.setValue(source->Rigid.getValue());
                reinitFromCounterpart = true;
            }
        }
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
    // FCPROJECT-PATCH (2026-10-13): in diesem Durchlauf NEU angelegte Blatt-Spiegel (Quelle, Spiegel)
    // - siehe Nachlauf "neues Teil in bestehender Instanz" unten.
    std::vector<std::pair<App::DocumentObject*, App::DocumentObject*>> newlyCreatedLeaves;
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
                    syncPlacements(startPlacementSource(this, srcObj), newObj);

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

            // FCPROJECT-PATCH (2026-10-07, live an BG37 mit flexibel eingefuegter BG43 gefunden):
            // ein NEU angelegter Spiegel startet in der Lage seiner Quelle. Vorher blieb er auf der
            // Default-Placement; das fiel nicht auf, solange der Solver ins Original schrieb und
            // danach alle Spiegel nachzog. Seit "Solver nur auf Spiegeln" ist die Spiegel-Placement
            // die Startlage des Solvers - ohne diese Initialisierung kam die eingefuegte BG
            // zerwuerfelt an (Teile 20-60mm daneben) und der Solve scheiterte (iterNo > iterMax).
            // Nur beim Anlegen: danach ist die Placement Zustand dieser Instanz.
            // Verschachtelt: aus dem Gegenstueck, nicht aus dem innersten Original
            // (startPlacementSource(), 2026-10-11).
            syncPlacements(startPlacementSource(this, obj), link);
            auto* newAsmLink = freecad_cast<AssemblyLink*>(link);
            if (!link->isLinkGroup() && (!newAsmLink || newAsmLink->isRigid())) {
                newlyCreatedLeaves.emplace_back(obj, link);
            }
        }

        objLinkMap[obj] = link;
        mirrorToSourceMap[link] = obj;

        // FCPROJECT-PATCH (2026-10-09, Regel C1 in docs/CONSTRAINTS.md): ob eine Unterbaugruppe
        // starr oder flexibel ist, legt allein das einbauende Dokument fest. Ein verschachtelter
        // Spiegel uebernimmt den Wert deshalb immer von seiner Quelle, nicht nur beim Anlegen.
        // Live an BG22 gefunden: der BG43-Spiegel unter BG37 war aus einem alten Stand noch
        // starr, in BG37 ist BG43 inzwischen flexibel - Mischzustand, derselbe Motor wurde zwei
        // Solver-Koerper und BG43 fiel auseinander. Nach einem Wechsel wird der Spiegel sofort
        // (nicht erst beim naechsten Recompute - er wurde in diesem schon vor uns ausgefuehrt)
        // aus seinem Gegenstueck neu aufgesetzt; verschachtelte Spiegel darunter ebenso.
        if (auto* srcAsmLink = freecad_cast<AssemblyLink*>(obj)) {
            if (auto* mirrorAsmLink = freecad_cast<AssemblyLink*>(link)) {
                if (mirrorAsmLink->Rigid.getValue() != srcAsmLink->Rigid.getValue()
                    || reinitFromCounterpart) {
                    mirrorAsmLink->Rigid.setValue(srcAsmLink->Rigid.getValue());
                    mirrorAsmLink->reinitFromCounterpart = true;
                    mirrorAsmLink->synchronizeComponents();
                }
            }
        }

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

    // FCPROJECT-PATCH (2026-10-13, live an BG22 gefunden: in BG75 zwei Schrauben per Teiletausch
    // ersetzt, BG22 hing danach im Solve): ein NEUES Teil in einer schon eingebauten flexiblen
    // Instanz startete in der Lage seines Gegenstuecks - die gilt nur im Dokument des Gegenstuecks.
    // Steckt die Instanz hier versetzt (BG75 in BG22 ~430mm und 90 Grad anders), lag das neue
    // Teil um genau diesen Versatz neben seinen Nachbarn. Jetzt uebernimmt es den Versatz seines
    // naechsten schon vorhandenen Nachbarn (naechster nach Lage im Gegenstueck): Lage =
    // (Nachbar hier) * (Nachbar dort)^-1 * (Teil dort). Gibt es keinen vorhandenen Nachbarn (ganze
    // Baugruppe frisch eingefuegt), bleibt es bei der Lage des Gegenstuecks.
    if (!newlyCreatedLeaves.empty() && !isRigid() && !hasRigidAncestorLink(this)) {
        auto placementOf = [](App::DocumentObject* obj) -> const App::PropertyPlacement* {
            return obj ? dynamic_cast<App::PropertyPlacement*>(obj->getPropertyByName("Placement"))
                       : nullptr;
        };
        for (const auto& [newSource, newMirror] : newlyCreatedLeaves) {
            const auto* newCounterpartPlc = placementOf(startPlacementSource(this, newSource));
            auto* newMirrorPlc = dynamic_cast<App::PropertyPlacement*>(
                newMirror->getPropertyByName("Placement")
            );
            if (!newCounterpartPlc || !newMirrorPlc) {
                continue;
            }
            const Base::Vector3d newPos = newCounterpartPlc->getValue().getPosition();
            double bestDistance = -1.0;
            Base::Placement bestOffset;
            for (const auto& [source, mirror] : objLinkMap) {
                const bool isNew = std::any_of(
                    newlyCreatedLeaves.begin(),
                    newlyCreatedLeaves.end(),
                    [mirror = mirror](const auto& entry) { return entry.second == mirror; }
                );
                auto* mirrorAsmLink = freecad_cast<AssemblyLink*>(mirror);
                if (isNew || !mirror || mirror->isLinkGroup() || (mirrorAsmLink && !mirrorAsmLink->isRigid())) {
                    continue;
                }
                const auto* counterpartPlc = placementOf(startPlacementSource(this, source));
                const auto* mirrorPlc = placementOf(mirror);
                if (!counterpartPlc || !mirrorPlc) {
                    continue;
                }
                const double distance
                    = (counterpartPlc->getValue().getPosition() - newPos).Length();
                if (bestDistance < 0.0 || distance < bestDistance) {
                    bestDistance = distance;
                    bestOffset = mirrorPlc->getValue() * counterpartPlc->getValue().inverse();
                }
            }
            if (bestDistance >= 0.0 && !bestOffset.isIdentity()) {
                newMirrorPlc->setValue(bestOffset * newCounterpartPlc->getValue());
            }
        }
    }

    // If the assemblyLink is rigid, then we keep all placements synchronized.
    // FCPROJECT-PATCH (2026-10-09, live an BG22 gefunden: BG43 starr in BG37, BG37 flexibel in
    // BG22 - die Teile des flexiblen Motor-Containers 067 UNTER der starren BG43 lagen
    // zerwuerfelt): eine starre BG zeigt den Stand, den ihr verlinktes Objekt zeigt ("zeigt ja,
    // aendert nein"). Das gilt fuer alles darunter, auch fuer flexible Container - unter einem
    // starren Vorfahren loest niemand deren Teile, also muessen sie ebenfalls nachgefuehrt werden.
    // Quelle der Placement ist das Gegenstueck im DIREKT verlinkten Objekt: ist das selbst ein
    // AssemblyLink (z.B. BG43s eigener 067-Spiegel), dessen Spiegel - nicht das Original der
    // innersten Baugruppe, das nie in einem einbindenden Kontext geloest wurde.
    //
    // reinitFromCounterpart: einmalig dasselbe nach einem Starr/Flexibel-Wechsel, den
    // synchronizeComponents() des Eltern-Spiegels von der Quelle uebernommen hat (Regel C1) - die
    // bisherigen Spiegel-Lagen stammen aus dem anderen Modus und sind keine gueltige Startlage.
    //
    // FCPROJECT-PATCH (2026-10-12, live an BG22 gefunden: Motor 067 in BG75 von starr auf
    // flexibel, in BG22 landete er 400mm/90 Grad neben dem Rest der BG75): beim Wechsel ZU
    // flexibel wird NICHT mehr vom Gegenstueck kopiert. Im starren Zustand ist die
    // Container-Placement die in DIESEM Kontext geloeste Lage des starren Koerpers (BG75 steckt
    // in BG22 an anderer Stelle als in BG75 selbst), die Teile darunter seine aktuelle Struktur.
    // Das Gegenstueck kennt nur die Lage in SEINEM Dokument. Das weltlage-erhaltende Nachfuehren
    // des Containers (flexibler Zweig unten) behaelt genau diese Lage. Beim Wechsel zu starr
    // gilt weiter: anzeigen, was das Gegenstueck zeigt.
    if (reinitFromCounterpart && !isRigid() && !hasRigidAncestorLink(this)) {
        reinitFromCounterpart = false;
    }
    if (isRigid() || hasRigidAncestorLink(this) || reinitFromCounterpart) {
        for (const auto& [sourceObj, linkObj] : objLinkMap) {
            // startPlacementSource(): Gegenstueck ueber objLinkMap, sonst ueber dessen Group -
            // nie das innerste Original, wenn das Gegenstueck noch nicht synchronisiert ist.
            syncPlacements(startPlacementSource(this, sourceObj), linkObj);
        }
        reinitFromCounterpart = false;
    }
    else {
        // FCPROJECT-PATCH (2026-10-03, Plan plans/2026-10-03-solver-nur-auf-spiegeln.md): auch bei
        // einem flexiblen Link folgen die Spiegel verschachtelter CONTAINER (AssemblyLinks) immer
        // ihrer Quelle, nur in diese Richtung. Ein Container ist nie ein Solver-Koerper, seine
        // Placement gehoert zur Struktur der Unterbaugruppe; die Weltlage jedes Teils in diesem
        // Kontext ist lokale Placement mal genau diese Spiegel-Kette (IdentityGraph::
        // containerChainPlacement()). Weicht ein Container-Spiegel ab (live an BG37 gefunden:
        // Motor-Container um 142 Grad verdreht), zeigen GUI und Solver verschiedene Lagen. Die
        // Spiegel der Blatt-Teile bleiben dagegen Zustand dieser Instanz (der Solver schreibt sie).
        //
        // Nur FLEXIBLE Container: ein rigider AssemblyLink ist selbst ein Solver-Koerper, seine
        // Placement ist Zustand dieser Instanz wie bei jedem Blatt-Teil (live an BG22 gefunden: ein
        // rigider Halter innerhalb einer flexiblen Fuehrungsbaugruppe wurde sonst um 34mm auf die
        // Quelle zurueckgesetzt, der Solve scheiterte).
        //
        // FCPROJECT-PATCH (2026-10-12, live an BG22 gefunden: Motor 067 in BG75 von starr auf
        // flexibel umgeschaltet, in BG22 flog er 374mm/90 Grad weg, Solve 58-166s, gescheitert):
        // das Nachfuehren des Containers darf die Teile darin in der Welt NICHT bewegen - ein
        // Container ist reine Struktur. Beim Umschalten schiebt BG75 seine Container-Placement in
        // die Teile und setzt den Container auf Identitaet (onChanged(Rigid),
        // applyPlacementKeepingContainers()); der BG22-Spiegel hatte seine Teile aber schon VORHER
        // vom Gegenstueck uebernommen (reinitFromCounterpart, Regel C1) und folgte danach nur noch
        // dem Container. Allgemein: aendert sich die Container-Placement von alt auf neu, bekommen
        // die Teile die Gegenverschiebung neu^-1 * alt.
        for (const auto& [sourceObj, linkObj] : objLinkMap) {
            auto* sourceLink = freecad_cast<AssemblyLink*>(sourceObj);
            auto* mirrorLink = freecad_cast<AssemblyLink*>(linkObj);
            if (sourceLink && mirrorLink && !mirrorLink->isRigid()) {
                const Base::Placement oldPlc = mirrorLink->Placement.getValue();
                const Base::Placement newPlc = sourceLink->Placement.getValue();
                if (!oldPlc.isSame(newPlc)) {
                    syncPlacements(sourceObj, linkObj);
                    applyPlacementKeepingContainers(mirrorLink, newPlc.inverse() * oldPlc, Base::Placement());
                }
            }
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
