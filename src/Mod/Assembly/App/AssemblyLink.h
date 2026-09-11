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


#pragma once

#include <unordered_map>

#include <Mod/Assembly/AssemblyGlobal.h>

#include <App/FeaturePython.h>
#include <App/Part.h>
#include <App/PropertyLinks.h>


namespace Assembly
{
class AssemblyObject;
class JointGroup;

class AssemblyExport AssemblyLink: public App::Part
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::AssemblyLink);

public:
    AssemblyLink();
    ~AssemblyLink() override;

    PyObject* getPyObject() override;

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderAssemblyLink";
    }

    App::DocumentObjectExecReturn* execute() override;

    // The linked assembly is the AssemblyObject that this AssemblyLink pseudo-links to recursively.
    AssemblyObject* getLinkedAssembly() const;
    // The parent assembly is the main assembly in which the linked assembly is contained
    AssemblyObject* getParentAssembly() const;

    // Overriding DocumentObject::getLinkedObject is giving bugs
    // This function returns the linked object, either an AssemblyObject or an AssemblyLink
    App::DocumentObject* getLinkedObject2(bool recurse = true) const;

    bool isRigid() const;

    /**
     * Update all of the components and joints from the Assembly
     */
    void updateContents();
    void updateParentJoints();

    void synchronizeComponents();
    void synchronizeJoints();
    void handleJointReference(
        App::DocumentObject* joint,
        App::DocumentObject* lJoint,
        const char* refName
    );
    // FCPROJECT-PATCH: siehe patches/bugreport-rigid-nested-joint-reference/README.md.
    // handleJointReference()'s objLinkMap-Lookup kennt nur direkte Kinder der gespiegelten
    // Baugruppe. Zeigt eine Referenz auf ein Enkelkind (moeglich, wenn eine verschachtelte
    // Unterbaugruppe Rigid=False ist - siehe UtilsAssembly.getComponentReference()), laeuft
    // diese Hilfsfunktion die Eltern-Kette hoch, bis sie einen in objLinkMap bekannten
    // Vorfahren findet, und liefert den dabei durchlaufenen Pfad als Sub-Praefix zurueck -
    // exakt das Gegenstueck zu getComponentReference()s eigener Kodierung beim Joint-Anlegen.
    App::DocumentObject* findLocalAncestor(App::DocumentObject* obj, std::string& outSubPrefix);
    // FCPROJECT-PATCH: siehe patches/README.md ("GroundedJoint/RigidGroupJoint verschwindet
    // bei verschachtelter flexibler Baugruppe"). synchronizeJoints() spiegelt nur Joints mit
    // Reference1/Reference2 (echte Assembly::JointObject-Typen, gefiltert ueber getJoints()).
    // GroundedJoint (Property "ObjectToGround") und RigidGroupJoint (Property
    // "ObjectsToRigidGroup") werden dabei absichtlich ausgeschlossen (siehe Kommentar "Filter
    // grounded joints..." in AssemblyObject::getJoints()) und daher NIE in verschachtelte
    // AssemblyLink-Kopien uebernommen - die Erdung/Starrkoerper-Zugehoerigkeit geht dadurch
    // beim Verschachteln verloren, betroffene Teile bleiben im Solve komplett unbeschraenkt.
    // Diese Funktion spiegelt beide Jointtypen zusaetzlich, unabhaengig von synchronizeJoints()s
    // positionsbasiertem Abgleich (der fuer diese Typen nicht anwendbar ist, da sie keine
    // Reference1/2 haben).
    //
    // AKTUELL DEAKTIVIERT (Aufruf in updateContents() auskommentiert, siehe dort) - hat live
    // einen FreeCAD-Absturz ausgeloest (Reentrancy-Kaskade beim Loeschen einer verschachtelten
    // AssemblyLink). Bleibt als toter Code stehen, bis das Reentrancy-Problem geloest ist -
    // siehe patches/README.md fuer den vollen Befund.
    void synchronizeGroundedAndRigidJoints();
    App::DocumentObject* mapToLocalComponent(App::DocumentObject* externalComponent);
    void ensureNoJointGroup();
    JointGroup* ensureJointGroup();
    std::vector<App::DocumentObject*> getJoints();

    bool allowDuplicateLabel() const override;

    bool isEmpty() const;
    int numberOfComponents() const;

    App::PropertyXLink LinkedObject;
    App::PropertyBool Rigid;

    std::unordered_map<App::DocumentObject*, App::DocumentObject*> objLinkMap;

protected:
    /// get called by the container whenever a property has been changed
    void onChanged(const App::Property* prop) override;
    void onDocumentRestored() override;

private:
    // Reentrancy guard for updateContents(). synchronizeComponents() adds/removes objects in
    // this AssemblyLink's own Group, which synchronously re-enters onChanged(&Group) ->
    // updateContents(). That re-entry is not limited to the same instance: onChanged(&Group)
    // walks getInList() and calls updateContents() on *every* AssemblyLink that references this
    // one, which can in turn reference the first one back, i.e. the cascade can ping-pong across
    // several distinct AssemblyLink instances rather than just recursing into itself. A
    // per-instance flag only stops the single-instance case, so this needs to be shared by all
    // instances. Normally the cascade settles after one or two passes because everything is
    // already in sync. But if a component of the *source* assembly is being deleted while some
    // AssemblyLink still mirrors it (e.g. the same sub-assembly is linked more than once - see
    // FCProject bugreport "loeschfehler"), synchronizeComponents() can never find a stable match
    // for the vanishing source object and keeps recreating a mirror for it on every re-entry,
    // which never terminates (FreeCAD hangs, 100% CPU on the main thread, no further Report View
    // output). This flag makes any nested re-entry - same instance or not - a no-op; the
    // outermost call still runs synchronizeComponents()/synchronizeJoints() to completion, and
    // any sync that a nested call would have performed happens on the next legitimate
    // execute()/recompute instead.
    static bool updatingContents;
};


}  // namespace Assembly
