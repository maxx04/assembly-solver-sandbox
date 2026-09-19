// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
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

#include <boost/core/ignore_unused.hpp>
#include <cmath>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <ranges>


#include <App/Application.h>
#include <App/Datums.h>
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

#include <Mod/Part/App/TopoShape.h>
#include <Mod/Part/App/AttachExtension.h>

#include <OndselSolver/CREATE.h>
#include <OndselSolver/ASMTSimulationParameters.h>
#include <OndselSolver/ASMTAssembly.h>
#include <OndselSolver/ASMTMarker.h>
#include <OndselSolver/ASMTPart.h>
#include <OndselSolver/ASMTJoint.h>
#include <OndselSolver/ASMTAngleJoint.h>
#include <OndselSolver/ASMTFixedJoint.h>
#include <OndselSolver/ASMTGearJoint.h>
#include <OndselSolver/ASMTRevoluteJoint.h>
#include <OndselSolver/ASMTCylindricalJoint.h>
#include <OndselSolver/ASMTTranslationalJoint.h>
#include <OndselSolver/ASMTSphericalJoint.h>
#include <OndselSolver/ASMTParallelAxesJoint.h>
#include <OndselSolver/ASMTPerpendicularJoint.h>
#include <OndselSolver/ASMTPointInPlaneJoint.h>
#include <OndselSolver/ASMTPointInLineJoint.h>
#include <OndselSolver/ASMTLineInPlaneJoint.h>
#include <OndselSolver/ASMTPlanarJoint.h>
#include <OndselSolver/ASMTRevCylJoint.h>
#include <OndselSolver/ASMTCylSphJoint.h>
#include <OndselSolver/ASMTRackPinionJoint.h>
#include <OndselSolver/ASMTRotationLimit.h>
#include <OndselSolver/ASMTTranslationLimit.h>
#include <OndselSolver/ASMTRotationalMotion.h>
#include <OndselSolver/ASMTTranslationalMotion.h>
#include <OndselSolver/ASMTGeneralMotion.h>
#include <OndselSolver/ASMTScrewJoint.h>
#include <OndselSolver/ASMTSphSphJoint.h>
#include <OndselSolver/ASMTTime.h>
#include <OndselSolver/ASMTConstantGravity.h>
#include <OndselSolver/ExternalSystem.h>
#include <OndselSolver/enum.h>

#include "AssemblyLink.h"
#include "AssemblyObject.h"
#include "AssemblyObjectPy.h"
#include "AssemblyUtils.h"
#include "Groups.h"

FC_LOG_LEVEL_INIT("Assembly", true, true, true)

using namespace Assembly;
using namespace MbD;


namespace PartApp = Part;

namespace
{

struct RotationJointSide
{
    App::DocumentObject* joint = nullptr;
    const char* carrierRefName = nullptr;
    const char* carrierPlcName = nullptr;
};

Base::Placement getJointSideGlobalPlacement(
    App::DocumentObject* joint,
    const char* refName,
    const char* plcName
)
{
    if (!joint) {
        return {};
    }

    auto* ref = joint->getPropertyByName<App::PropertyXLinkSub>(refName);
    if (!ref) {
        return {};
    }

    return App::GeoFeature::getGlobalPlacement(nullptr, ref)
        * App::GeoFeature::getPlacementFromProp(joint, plcName);
}

bool axesAreCoaxial(const Base::Placement& plc1, const Base::Placement& plc2)
{
    Base::Vector3d axis1 = plc1.getRotation().multVec(Base::Vector3d::UnitZ);
    Base::Vector3d axis2 = plc2.getRotation().multVec(Base::Vector3d::UnitZ);
    if (axis1.Length() <= Precision::Confusion() || axis2.Length() <= Precision::Confusion()) {
        return false;
    }
    axis1.Normalize();
    axis2.Normalize();

    if (1.0 - std::abs(axis1 * axis2) > Precision::Confusion()) {
        return false;
    }

    const Base::Vector3d delta = plc2.getPosition() - plc1.getPosition();
    return delta.Cross(axis1).Length() <= Precision::Confusion();
}

bool gearJointNeedsCarrierMarker(App::DocumentObject* joint)
{
    const auto plc1 = getJointSideGlobalPlacement(joint, "Reference1", "Placement1");
    const auto plc2 = getJointSideGlobalPlacement(joint, "Reference2", "Placement2");
    return axesAreCoaxial(plc1, plc2);
}

bool isRotationCarrierJoint(JointType type)
{
    return type == JointType::Revolute || type == JointType::Cylindrical;
}

bool findRotationCarrierForGearSide(
    AssemblyObject* assembly,
    App::DocumentObject* gearJoint,
    const char* gearRefName,
    const char* gearPlcName,
    RotationJointSide& carrierSide
)
{
    auto* gearPart = getMovingPartFromRef(gearJoint, gearRefName);
    if (!gearPart) {
        return false;
    }

    const auto gearPlc = getJointSideGlobalPlacement(gearJoint, gearRefName, gearPlcName);
    bool foundCarrier = false;
    bool ambiguousCarrier = false;

    for (auto& jr : assembly->getJoints(false, true)) {
        auto* joint = jr.joint;
        if (!joint || joint == gearJoint || !getJointActivated(joint)) {
            continue;
        }
        if (!isRotationCarrierJoint(getJointType(joint))) {
            continue;
        }

        auto matchesSide = [&](const char* movingRefName,
                               const char* movingPlcName,
                               const char* carrierRefName,
                               const char* carrierPlcName) {
            if (getMovingPartFromRef(joint, movingRefName) != gearPart) {
                return false;
            }

            const auto rotationPlc = getJointSideGlobalPlacement(joint, movingRefName, movingPlcName);
            if (!axesAreCoaxial(gearPlc, rotationPlc)) {
                return false;
            }

            if (foundCarrier) {
                ambiguousCarrier = true;
                return false;
            }
            foundCarrier = true;
            carrierSide.joint = joint;
            carrierSide.carrierRefName = carrierRefName;
            carrierSide.carrierPlcName = carrierPlcName;
            return true;
        };

        matchesSide("Reference1", "Placement1", "Reference2", "Placement2");
        matchesSide("Reference2", "Placement2", "Reference1", "Placement1");
    }

    return foundCarrier && !ambiguousCarrier;
}

void setGearJointCarrierMarkerIfAvailable(
    AssemblyObject* assembly,
    App::DocumentObject* joint,
    const std::shared_ptr<ASMTJoint>& mbdJoint
)
{
    auto gearJoint = std::dynamic_pointer_cast<ASMTGearJoint>(mbdJoint);
    if (!gearJoint) {
        return;
    }

    RotationJointSide side1;
    RotationJointSide side2;
    const bool hasCarrier1
        = findRotationCarrierForGearSide(assembly, joint, "Reference1", "Placement1", side1);
    const bool hasCarrier2
        = findRotationCarrierForGearSide(assembly, joint, "Reference2", "Placement2", side2);
    if (!hasCarrier1 || !hasCarrier2) {
        return;
    }

    auto* carrierPart1 = getMovingPartFromRef(side1.joint, side1.carrierRefName);
    auto* carrierPart2 = getMovingPartFromRef(side2.joint, side2.carrierRefName);
    if (!carrierPart1 || !carrierPart2
        || assembly->getMbDPart(carrierPart1) != assembly->getMbDPart(carrierPart2)) {
        return;
    }

    const std::string carrierMarkerName = joint->getFullName() + "-Carrier";
    std::string fullMarkerNameK = assembly->handleOneSideOfJoint(
        side1.joint,
        side1.carrierRefName,
        side1.carrierPlcName,
        carrierMarkerName
    );
    if (!fullMarkerNameK.empty()) {
        gearJoint->setMarkerK(fullMarkerNameK);
    }
}

}  // namespace


// ================================ Assembly Object ============================

PROPERTY_SOURCE(Assembly::AssemblyObject, App::Part)

AssemblyObject::AssemblyObject()
    : mbdAssembly(std::make_shared<ASMTAssembly>())
    , bundleFixed(false)
    , lastDoF(0)
    , lastHasConflict(false)
    , lastHasRedundancies(false)
    , lastHasPartialRedundancies(false)
    , lastHasMalformedConstraints(false)
    , lastSolverStatus(0)
{
    mbdAssembly->externalSystem->freecadAssemblyObject = this;

    lastDoF = numberOfComponents() * 6;
    signalSolverUpdate();
}

AssemblyObject::~AssemblyObject() = default;

PyObject* AssemblyObject::getPyObject()
{
    if (PythonObject.is(Py::_None())) {
        // ref counter is set to 1
        PythonObject = Py::Object(new AssemblyObjectPy(this), true);
    }
    return Py::new_reference_to(PythonObject);
}

App::DocumentObjectExecReturn* AssemblyObject::execute()
{
    App::DocumentObjectExecReturn* ret = App::Part::execute();

    ParameterGrp::handle hGrp = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );
    if (hGrp->GetBool("SolveOnRecompute", true)) {
        // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix,
        // 2026-09-03, Nutzerentscheidung "oben nach unten"): eine Instanz, die strukturell unter
        // einer flexiblen AssemblyLink haengt, loest sich hier bewusst NICHT mehr selbst - die
        // aeussere, umfassendere Instanz erreicht ihre Joints/geerdeten Teile ohnehin bereits
        // rekursiv (Teilschritt 2/2e) und schreibt direkt auf ihre ECHTEN Placement-Properties
        // (canonicalizeForMbD()). Ohne diese Sperre loesen mehrere voneinander unabhaengige
        // Instanzen denselben physischen Teilbaum, jede mit ihrer eigenen, unvollstaendigen Sicht,
        // und ueberschreiben sich gegenseitig je nach Dokument-Abhaengigkeits-Reihenfolge - das
        // war die eigentliche Ursache von Befund 3 (siehe assembly-architecture-overview.md,
        // Abschnitt "NEUER, noch offener Befund"). Betrifft NUR diesen Recompute-Pfad -
        // interaktives Ziehen (preDrag()/doDragStep()) ruft solve() weiterhin direkt und
        // unbedingt auf der gerade aktiven Instanz auf.
        if (isNestedUnderFlexibleParent()) {
            Base::Console().message(
                "Assembly: '%s' skipped its own solve() - nested under a flexible parent "
                "assembly, which solves it as part of its own recompute.\n",
                getFullName()
            );
        }
        else {
            solve(false);
        }
    }
    return ret;
}

void AssemblyObject::onChanged(const App::Property* prop)
{
    if (prop == &Group) {
        for (auto* obj : getInList()) {
            if (auto* assemblyLink = freecad_cast<AssemblyLink*>(obj)) {
                assemblyLink->updateContents();
            }
        }
        updateSolveStatus();
    }
    App::Part::onChanged(prop);
}

namespace
{
// FCPROJECT-PATCH (10, nachgebessert): Aequivalent zu JointObject.py's getContext() - baut den
// vollen Pfad ueber die Eltern-Kette (InList, jeweils erster Eintrag) auf, z.B.
// "Halterbaugruppe.Joint005" statt nur "Joint005". Ein blosser Name reicht bei verschachtelten
// Baugruppen (PDM-Standardfall: jede Unterbaugruppe hat ihre EIGENE, lokal bei 0 beginnende
// Joint-Nummerierung) nicht aus, um den Joint ohne Suchen im Baum wiederzufinden - man muss
// wissen, in welcher Unterbaugruppe er sitzt. maxDepth als simple Zyklen-Bremse statt eines
// zusaetzlichen std::set-Includes. Trotz des Namens nicht joint-spezifisch - funktioniert fuer
// jedes DocumentObject (seit Fix 13 auch fuer geerdete Teile und Rigid-Group-Mitglieder
// verwendet), daher hier vor solve() statt erst vor isMbDJointValid().
std::string getJointContextName(App::DocumentObject* obj)
{
    std::vector<std::string> parts;
    App::DocumentObject* current = obj;
    int maxDepth = 32;
    while (current && maxDepth-- > 0) {
        const char* name = current->getNameInDocument();
        parts.insert(parts.begin(), name ? name : "?");
        const auto& inList = current->getInList();
        current = inList.empty() ? nullptr : inList.front();
    }

    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            result += ".";
        }
        result += parts[i];
    }

    if (obj && obj->getDocument()) {
        return std::string(obj->getDocument()->Label.getStrValue()) + "#" + result;
    }
    return result;
}

// FCPROJECT-PATCH (13): Nutzerwunsch - Namen statt nur Anzahl ausgeben, siehe solve() unten.
// Nur noch fuer die Rigid-Group-Mitgliederliste gebraucht (Vector) - die geerdeten Teile
// bekommen seit Fix 15 je eine eigene Zeile statt einer komma-getrennten Liste.
std::string joinContextNames(const std::vector<App::DocumentObject*>& objs)
{
    std::string result;
    for (auto* obj : objs) {
        if (!obj) {
            continue;
        }
        if (!result.empty()) {
            result += ", ";
        }
        result += getJointContextName(obj);
    }
    return result;
}

// FCPROJECT-PATCH (Mehrfachinstanz-Fix - siehe docs/ARCHITECTURE.md
// Abschnitt 4/5): ASMT-Joint-/Marker-Namen wurden bisher ausschliesslich aus joint->getFullName()
// gebildet - kollidiert, sobald DERSELBE reale Joint ueber ZWEI AssemblyLink-Instanzen desselben
// verlinkten Dokuments erreicht wird (getJoints() liefert dann zwei JointRef-Eintraege mit
// gleichem joint-Zeiger, aber unterschiedlichem nestingPrefix, z.B. "SubLink."/"SubLink001.").
// nestingPrefix ist bereits punkt-terminiert (siehe getJoints()), daher reicht eine einfache
// Verkettung. Bei leerem Prefix (unveraendert der nicht verschachtelte Fall) byte-identisch zum
// bisherigen Namen - keine Verhaltensaenderung fuer die gesamte bestehende Testmatrix.
std::string jointInstanceName(App::DocumentObject* joint, const std::string& nestingPrefix)
{
    return nestingPrefix.empty() ? joint->getFullName() : nestingPrefix + joint->getFullName();
}
}  // namespace

int AssemblyObject::solve(bool enableRedo)
{
    // FCPROJECT-PATCH (11): mehr Rueckmeldung ueber den Solver-Ablauf. Bisher blieb sowohl ein
    // erfolgreicher Solve als auch ein mangels geerdetem Teil komplett uebersprungener Solve
    // (groundedObjs.empty() -> return -6) OHNE JEDE Konsolen-Ausgabe - nur echte Exceptions
    // wurden gemeldet (siehe catch-Bloecke unten). Im PDM-Alltag mit vielen verschachtelten
    // Baugruppen ist dadurch oft unklar, ob/wann der Solver ueberhaupt gelaufen ist und was er
    // dabei festgestellt hat (z.B. redundante Joints, siehe Fix 10) - die drei Meldungen unten
    // decken die Fragen "1. gestartet? 2. berechnet? 3. mit welchem Ergebnis?" ab.
    Base::Console().message("Assembly: Solving '%s'...\n", getFullName());

    ensureIdentityPlacements();

    syncGroundedJoints();

    mbdAssembly = makeMbdAssembly();
    objectPartMap.clear();
    rebuildRigidClusters();
    syncActiveRigidGroupPlacements();
    motions.clear();

    auto groundedObjs = fixGroundedParts();
    if (groundedObjs.empty()) {
        // If no part fixed we can't solve.
        Base::Console().warning(
            "Assembly: Solve of '%s' skipped - no grounded part found.\n",
            getFullName()
        );
        return -6;
    }

    // verboseLog=true nur hier: dies ist der EINE echte, seltene solve()-Aufruf - nicht die
    // haeufigen internen getJoints()-Aufrufe aus isPartConnected()/getJointsOfPart() waehrend
    // einer interaktiven Zieh-Bewegung (siehe FCPROJECT-PATCH 16 in getJoints()).
    auto jointRefs = getJoints(false, true, true);

    removeUnconnectedJoints(jointRefs, groundedObjs);

    jointParts(jointRefs);
    std::vector<App::DocumentObject*> joints = extractJointObjects(jointRefs);

    if (enableRedo) {
        savePlacementsForUndo();
    }

    try {
        mbdAssembly->runPreDrag();
        lastSolverStatus = 0;
    }
    catch (const std::exception& e) {
        FC_ERR("Solve failed: " << e.what());
        lastSolverStatus = -1;
        updateSolveStatus();
        return -1;
    }
    catch (...) {
        FC_ERR("Solve failed: unhandled exception");
        lastSolverStatus = -1;
        updateSolveStatus();
        return -1;
    }

    // FCPROJECT-PATCH (13): Nutzerwunsch - nicht nur die ANZAHL der geerdeten Teile ausgeben,
    // sondern auch deren Namen (z.B. um zu erkennen, dass Origin IMMER automatisch mitgezaehlt
    // wird, auch ohne eigenen GroundedJoint - siehe getGroundedParts()), sowie die Mitglieder
    // etwaiger Rigid Groups (siehe rebuildRigidClusters() oben, neues RigidGroup-Feature).
    // FCPROJECT-PATCH (15): Nutzerwunsch - jedes geerdete Teil in einer eigenen Zeile statt
    // komma-getrennt in einer Zeile, damit lange Listen im Report View nicht mehr horizontal
    // gescrollt werden muessen.
    Base::Console().message(
        "Assembly: '%s' computed (%zu joint(s), %zu grounded part(s)):\n",
        getFullName(),
        joints.size(),
        groundedObjs.size()
    );
    for (auto* obj : groundedObjs) {
        if (!obj) {
            continue;
        }
        Base::Console().message("Assembly:   grounded part: %s\n", getJointContextName(obj));
    }

    for (const auto& [rep, members] : rigidMembersByRep) {
        Base::Console().message(
            "Assembly: rigid group '%s' (%zu Teil(e)): %s.\n",
            getJointContextName(rep),
            members.size(),
            joinContextNames(members)
        );
    }

    setNewPlacements();
    updateRigidPlacementCache();

    redrawJointPlacements(joints);

    updateSolveStatus();

    if (lastHasRedundancies) {
        std::string names;
        for (const auto& n : lastRedundantJoints) {
            if (!names.empty()) {
                names += ", ";
            }
            names += n;
        }
        Base::Console().warning(
            "Assembly: Solve of '%s' finished with %zu redundant joint(s): %s.\n",
            getFullName(),
            lastRedundantJoints.size(),
            names
        );
    }
    else {
        Base::Console().message("Assembly: Solve of '%s' finished successfully.\n", getFullName());
    }

    return 0;
}

void AssemblyObject::updateSolveStatus()
{
    lastRedundantJoints.clear();
    lastHasRedundancies = false;

    int numberOfSolverBodies = numberOfComponents();
    if (!objectPartMap.empty()) {
        std::unordered_set<MbD::ASMTPart*> uniqueParts;
        for (const auto& entry : objectPartMap) {
            if (entry.second.part) {
                uniqueParts.insert(entry.second.part.get());
            }
        }

        const int bundledParts = static_cast<int>(objectPartMap.size() - uniqueParts.size());
        numberOfSolverBodies -= bundledParts;
    }

    // +1 because the assembly origin is also represented by a solver body.
    lastDoF = (1 + numberOfSolverBodies) * 6;

    if (!mbdAssembly || !mbdAssembly->mbdSystem) {
        solve();
    }

    if (!mbdAssembly || !mbdAssembly->mbdSystem) {
        return;
    }

    // Helper lambda to clean up the joint name from the solver
    auto cleanJointName = [](const std::string& rawName) -> std::string {
        // rawName is like : /OndselAssembly/ground_moves#Joint001
        size_t hashPos = rawName.find_last_of('#');
        if (hashPos != std::string::npos) {
            // Return the substring after the '#'
            return rawName.substr(hashPos + 1);
        }
        return rawName;
    };


    // Iterate through all joints and motions in the MBD system
    mbdAssembly->mbdSystem->jointsMotionsDo([&](std::shared_ptr<MbD::Joint> jm) {
        if (!jm) {
            return;
        }
        // Base::Console().warning("jm->name %s\n", jm->name);
        bool isJointRedundant = false;

        jm->constraintsDo([&](std::shared_ptr<MbD::Constraint> con) {
            if (!con) {
                return;
            }

            std::string spec = con->constraintSpec();
            // A constraint is redundant if its spec starts with "Redundant"
            if (spec.rfind("Redundant", 0) == 0) {
                isJointRedundant = true;
            }
            // Base::Console().warning("    - %s\n", spec);
            --lastDoF;
        });

        const std::string fullName = cleanJointName(jm->name);
        App::DocumentObject* docObj = getDocument()->getObject(fullName.c_str());

        // We only care about objects that are actual joints in the FreeCAD document.
        // This effectively filters out the grounding joints, which are named after parts.
        if (!docObj || !docObj->getPropertyByName("Reference1")) {
            return;
        }

        if (isJointRedundant) {
            // Check if this joint is already in the list to avoid duplicates
            std::string objName = docObj->getNameInDocument();
            if (std::find(lastRedundantJoints.begin(), lastRedundantJoints.end(), objName)
                == lastRedundantJoints.end()) {
                lastRedundantJoints.push_back(objName);
            }
        }
    });

    // Update the summary boolean flag
    if (!lastRedundantJoints.empty()) {
        lastHasRedundancies = true;
    }

    signalSolverUpdate();
}

int AssemblyObject::generateSimulation(App::DocumentObject* sim)
{
    mbdAssembly = makeMbdAssembly();
    objectPartMap.clear();

    motions = getMotionsFromSimulation(sim);

    auto groundedObjs = fixGroundedParts();
    if (groundedObjs.empty()) {
        // If no part fixed we can't solve.
        return -6;
    }

    auto jointRefs = getJoints();

    removeUnconnectedJoints(jointRefs, groundedObjs);

    jointParts(jointRefs);

    create_mbdSimulationParameters(sim);

    try {
        mbdAssembly->runKINEMATIC();
    }
    catch (...) {
        Base::Console().error("Generation of simulation failed\n");
        motions.clear();
        return -1;
    }

    motions.clear();

    return 0;
}

std::vector<App::DocumentObject*> AssemblyObject::getMotionsFromSimulation(App::DocumentObject* sim)
{
    if (!sim) {
        return {};
    }

    auto* prop = dynamic_cast<App::PropertyLinkList*>(sim->getPropertyByName("Group"));
    if (!prop) {
        return {};
    }

    return prop->getValue();
}

int Assembly::AssemblyObject::updateForFrame(size_t index)
{
    if (!mbdAssembly) {
        return -1;
    }

    auto nfrms = mbdAssembly->numberOfFrames();
    if (index >= nfrms) {
        return -1;
    }

    mbdAssembly->updateForFrame(index);
    setNewPlacements();
    auto jointDocs = extractJointObjects(getJoints());
    redrawJointPlacements(jointDocs);
    return 0;
}

size_t Assembly::AssemblyObject::numberOfFrames()
{
    return mbdAssembly->numberOfFrames();
}

bool AssemblyObject::requiresRigidSolveForMove(const std::vector<App::DocumentObject*>& movedParts)
{
    rebuildRigidClusters();

    return std::ranges::any_of(movedParts, [&](App::DocumentObject* part) {
        return getRigidRepresentative(part) != nullptr;
    });
}

void AssemblyObject::preDrag(std::vector<App::DocumentObject*> dragParts)
{
    bundleFixed = true;
    solve();
    bundleFixed = false;

    draggedParts.clear();
    for (auto part : dragParts) {
        const bool isRigidClustered = getRigidRepresentative(part) != nullptr;

        // make sure no duplicate
        if (std::ranges::find(draggedParts, part) != draggedParts.end()) {
            continue;
        }

        // Active rigid-cluster members are solver-connected through the shared MbD part.
        if (!isRigidClustered && !isPartConnected(part)) {
            continue;
        }

        // Rigid-cluster members stay draggable because they share one MbD part.
        if (isRigidClustered) {
            draggedParts.push_back(part);
            continue;
        }

        Base::Placement plc;
        for (auto& pair : objectPartMap) {
            App::DocumentObject* parti = pair.first;
            if (parti != part) {
                continue;
            }
            plc = pair.second.offsetPlc;
        }
        if (!plc.isIdentity()) {
            // If not identity, then it's a bundled object. Some bundled objects may
            // have identity placement if they have the same position as the main object of
            // the bundle. But they're not going to be a problem.
            continue;
        }

        draggedParts.push_back(part);
    }
}

void AssemblyObject::doDragStep()
{
    try {
        std::vector<std::shared_ptr<MbD::ASMTPart>> dragMbdParts;
        std::unordered_set<ASMTPart*> seenMbdParts;

        for (auto& part : draggedParts) {
            if (!part) {
                continue;
            }

            auto mbdPart = getMbDPart(part);
            if (!mbdPart) {
                continue;
            }

            if (!seenMbdParts.insert(mbdPart.get()).second) {
                continue;
            }

            dragMbdParts.push_back(mbdPart);

            Base::Placement plc = getPlacementFromProp(part, "Placement");
            if (auto it = objectPartMap.find(part);
                it != objectPartMap.end() && !it->second.offsetPlc.isIdentity()) {
                plc = plc * it->second.offsetPlc.inverse();
            }
            Base::Vector3d pos = plc.getPosition();
            mbdPart->updateMbDFromPosition3D(
                std::make_shared<FullColumn<double>>(ListD {pos.x, pos.y, pos.z})
            );

            Base::Rotation rot = plc.getRotation();
            Base::Matrix4D mat;
            rot.getValue(mat);
            Base::Vector3d r0 = mat.getRow(0);
            Base::Vector3d r1 = mat.getRow(1);
            Base::Vector3d r2 = mat.getRow(2);
            mbdPart->updateMbDFromRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);
        }

        // Timing mbdAssembly->runDragStep()
        auto dragPartsVec = std::make_shared<std::vector<std::shared_ptr<ASMTPart>>>(dragMbdParts);
        mbdAssembly->runDragStep(dragPartsVec);

        // Timing the validation and placement setting
        if (validateNewPlacements()) {
            setNewPlacements();
            updateRigidPlacementCache();

            auto joints = extractJointObjects(getJoints());
            for (auto* joint : joints) {
                if (joint->Visibility.getValue()) {
                    // redraw only the moving joint as its quite slow as its python code.
                    redrawJointPlacement(joint);
                }
            }
        }
    }
    catch (...) {
        // We do nothing if a solve step fails.
    }
}

Base::Placement AssemblyObject::getMbdPlacement(std::shared_ptr<ASMTPart> mbdPart)
{
    if (!mbdPart) {
        return Base::Placement();
    }

    double x, y, z;
    mbdPart->getPosition3D(x, y, z);
    Base::Vector3d pos = Base::Vector3d(x, y, z);

    double q0, q1, q2, q3;
    mbdPart->getQuarternions(q3, q0, q1, q2);
    Base::Rotation rot = Base::Rotation(q0, q1, q2, q3);

    return Base::Placement(pos, rot);
}

bool AssemblyObject::validateNewPlacements()
{
    // First we check if a grounded object has moved. It can happen that they flip.
    auto groundedParts = getGroundedParts();
    for (auto* obj : groundedParts) {
        auto* propPlacement = obj->getPlacementProperty();
        if (propPlacement) {
            Base::Placement oldPlc = propPlacement->getValue();

            auto it = objectPartMap.find(obj);
            if (it != objectPartMap.end()) {
                std::shared_ptr<MbD::ASMTPart> mbdPart = it->second.part;
                Base::Placement newPlacement = getMbdPlacement(mbdPart);
                if (!it->second.offsetPlc.isIdentity()) {
                    newPlacement = newPlacement * it->second.offsetPlc;
                }

                if (!oldPlc.isSame(newPlacement, Precision::Confusion())) {
                    Base::Console().warning(
                        "Assembly : Ignoring bad solve, a grounded object (%s) moved.\n",
                        obj->getFullLabel()
                    );
                    return false;
                }
            }
        }
    }

    // TODO: We could do further tests
    // For example check if the joints connectors are correctly aligned.
    return true;
}

void AssemblyObject::postDrag()
{
    mbdAssembly->runPostDrag();  // Do this after last drag
    purgeTouched();
}

void AssemblyObject::savePlacementsForUndo()
{
    previousPositions.clear();

    for (auto& pair : objectPartMap) {
        App::DocumentObject* obj = pair.first;
        if (!obj) {
            continue;
        }

        std::pair<App::DocumentObject*, Base::Placement> savePair;
        savePair.first = obj;

        // Check if the object has a "Placement" property
        auto* propPlc = obj->getPlacementProperty();
        if (!propPlc) {
            continue;
        }
        savePair.second = propPlc->getValue();

        previousPositions.push_back(savePair);
    }
}

void AssemblyObject::undoSolve()
{
    if (previousPositions.size() == 0) {
        return;
    }

    for (auto& pair : previousPositions) {
        App::DocumentObject* obj = pair.first;
        if (!obj) {
            continue;
        }

        // Check if the object has a "Placement" property
        auto* propPlacement = obj->getPlacementProperty();
        if (!propPlacement) {
            continue;
        }

        propPlacement->setValue(pair.second);
    }
    previousPositions.clear();

    // update joint placements:
    getJoints();
}

void AssemblyObject::clearUndo()
{
    previousPositions.clear();
}

void AssemblyObject::exportAsASMT(std::string fileName)
{
    mbdAssembly = makeMbdAssembly();
    objectPartMap.clear();
    rebuildRigidClusters();
    fixGroundedParts();

    auto jointRefs = getJoints();

    jointParts(jointRefs);

    mbdAssembly->outputFile(fileName);
}

void AssemblyObject::rebuildRigidClusters()
{
    rigidRepByPart.clear();
    rigidMembersByRep.clear();

    std::unordered_map<App::DocumentObject*, App::DocumentObject*> parent;

    auto findRoot = [&](App::DocumentObject* node, auto& self) -> App::DocumentObject* {
        auto [it, inserted] = parent.emplace(node, node);
        if (inserted || it->second == node) {
            return it->second;
        }

        it->second = self(it->second, self);
        return it->second;
    };

    auto unite = [&](App::DocumentObject* a, App::DocumentObject* b) {
        if (!a || !b) {
            return;
        }

        auto* const rootA = findRoot(a, findRoot);
        auto* const rootB = findRoot(b, findRoot);

        if (rootA != rootB) {
            parent[rootB] = rootA;
        }
    };

    for (auto* const rigidGroup : getRigidGroups()) {
        if (!rigidGroup) {
            continue;
        }

        auto* const prop = dynamic_cast<App::PropertyLinkList*>(
            rigidGroup->getPropertyByName("ObjectsToRigidGroup")
        );
        if (!prop) {
            continue;
        }

        const auto members = prop->getValues();
        if (members.size() < 2) {
            continue;
        }

        // FCPROJECT-PATCH (2026-09-09, "Starre Verbindung"/RigidGroupJoint in verschachtelter
        // flexibler Baugruppe hebt kein 1-DOF-Gelenk richtig auf): ObjectsToRigidGroup liefert
        // die ROHEN, lokal ausgewaehlten Objekte (z.B. den Spiegel 'mirror_BoxA' hier in
        // GrandTop) - der Rest dieser Klasse (getConnectedParts()/removeUnconnectedJoints() via
        // resolvePartForMbD(), traverseAndMarkConnectedParts()) arbeitet aber durchgaengig mit
        // KANONISCHEN Identitaeten (canonicalizeForMbD() - das ECHTE, tiefste Objekt in der
        // verschachtelten Unterbaugruppe, nicht dessen lokale Spiegelkopie). Ohne Kanonisierung
        // HIER landen rigidRepByPart/rigidMembersByRep auf dem rohen Spiegel als Schluessel -
        // getConnectedParts()s Rigid-Cluster-Kante (s.u.) liefert dann ebenfalls den rohen
        // Spiegel als naechsten Traversal-Schritt, und der naechste Traversal-Schritt (ein
        // ECHTER Joint in der Unterbaugruppe, dessen Reference1/2 bereits kanonisch aufgeloest
        // wird) erkennt diesen rohen Spiegel nie als Uebereinstimmung - die Traversal-Kette
        // bricht GENAU an der Rigid-Cluster-Kante ab. Live reproduziert: eine "Starre
        // Verbindung" zwischen GrandTops BoxC und Subs (gespiegeltem) BoxB verhinderte NICHT,
        // dass Subs eigene interne Erdung von BoxA redundant bestehen blieb - beide Enden des
        // inneren Slider-Joints wurden dadurch unabhaengig voneinander fixiert, das Gelenk war
        // komplett eingefroren statt seinen 1 Freiheitsgrad zu behalten. Fix: Mitglieder vor dem
        // Verschmelzen kanonisieren, damit rigidRepByPart/rigidMembersByRep im SELBEN
        // Identitaetsraum liegen wie der Rest der Traversal-Logik.
        auto* const first = canonicalizeForMbD(members.front());
        for (auto* const rawMember : members | std::views::drop(1)) {
            auto* const member = canonicalizeForMbD(rawMember);
            unite(first, member);
        }
    }

    std::unordered_map<App::DocumentObject*, std::vector<App::DocumentObject*>> clusters;

    for (const auto& [node, _] : parent) {
        boost::ignore_unused(_);
        const auto root = findRoot(node, findRoot);
        clusters[root].push_back(node);
    }

    for (auto& [_, members] : clusters) {
        boost::ignore_unused(_);

        if (members.size() < 2) {
            continue;
        }

        const auto repIt = std::ranges::min_element(members, {}, [](App::DocumentObject* obj) {
            return obj ? std::string(obj->getNameInDocument()) : std::string();
        });

        if (repIt == members.end() || !*repIt) {
            continue;
        }

        auto* const rep = *repIt;

        for (auto* const member : members) {
            rigidRepByPart[member] = rep;
        }

        rigidMembersByRep[rep] = std::move(members);
    }
}

App::DocumentObject* AssemblyObject::getRigidRepresentative(App::DocumentObject* part) const
{
    if (!part) {
        return nullptr;
    }

    if (auto it = rigidRepByPart.find(part); it != rigidRepByPart.end()) {
        return it->second;
    }

    return nullptr;
}

const std::vector<App::DocumentObject*>* AssemblyObject::getRigidMembers(App::DocumentObject* part) const
{
    if (auto* rep = getRigidRepresentative(part); rep) {
        if (auto it = rigidMembersByRep.find(rep); it != rigidMembersByRep.end()) {
            return &it->second;
        }
    }

    return nullptr;
}

void AssemblyObject::syncActiveRigidGroupPlacements()
{
    for (const auto& [rep, members] : rigidMembersByRep) {
        if (!rep || members.size() < 2) {
            continue;
        }

        bool hasCompleteCache = true;
        std::vector<App::DocumentObject*> movedMembers;

        for (auto* member : members) {
            if (!member) {
                hasCompleteCache = false;
                break;
            }

            auto cacheIt = rigidPlacementCache.find(member);
            if (cacheIt == rigidPlacementCache.end()) {
                hasCompleteCache = false;
                break;
            }

            Base::Placement currentPlc = getPlacementFromProp(member, "Placement");
            if (!cacheIt->second.isSame(currentPlc)) {
                movedMembers.push_back(member);
            }
        }

        if (!hasCompleteCache) {
            for (auto* member : members) {
                if (!member) {
                    continue;
                }
                rigidPlacementCache[member] = getPlacementFromProp(member, "Placement");
            }
            continue;
        }

        if (movedMembers.empty()) {
            continue;
        }

        App::DocumentObject* driver = movedMembers.front();
        const Base::Placement oldDriverPlc = rigidPlacementCache.at(driver);
        const Base::Placement newDriverPlc = getPlacementFromProp(driver, "Placement");
        const Base::Placement delta = newDriverPlc * oldDriverPlc.inverse();

        for (auto* member : members) {
            if (!member || member == driver) {
                continue;
            }

            if (auto cacheIt = rigidPlacementCache.find(member);
                cacheIt != rigidPlacementCache.end()) {
                Base::Placement targetPlc = delta * cacheIt->second;
                auto* propPlacement = member->getPlacementProperty();
                if (propPlacement && !propPlacement->getValue().isSame(targetPlc)) {
                    propPlacement->setValue(targetPlc);
                    member->purgeTouched();
                }
            }
        }

        for (auto* member : members) {
            if (!member) {
                continue;
            }
            rigidPlacementCache[member] = getPlacementFromProp(member, "Placement");
        }
    }
}

void AssemblyObject::updateRigidPlacementCache()
{
    std::unordered_set<App::DocumentObject*> activeMembers;

    for (const auto& [rep, members] : rigidMembersByRep) {
        boost::ignore_unused(rep);
        for (auto* member : members) {
            if (!member) {
                continue;
            }
            activeMembers.insert(member);
            rigidPlacementCache[member] = getPlacementFromProp(member, "Placement");
        }
    }

    std::erase_if(rigidPlacementCache, [&](const auto& entry) {
        return !activeMembers.contains(entry.first);
    });
}

Base::Placement AssemblyObject::computeGroundCorrection()
{
    // FCPROJECT-PATCH (2026-09-13, "Erdung driftet trotz Solve-Erfolg" - siehe
    // docs/ARCHITECTURE.md §2.1a, Nutzerkorrektur "nach dem Solver in Ruhe lassen und alle
    // bewegenden Teile relativ zur Erdung berechnen"): OndselSolvers Redundanz-Elimination
    // (generische Gauss-Elimination mit Voll-Pivotisierung, GESpMatFullPvPosIC.cpp - waehlt
    // rein nach Betragsgroesse des Pivot-Elements, kennt keine Objekt-Identitaet) kann auch die
    // Erdungs-Constraints selbst als redundant verwerfen, wenn andere Fixed-Joints im System
    // nur ANNAEHERND (nicht exakt) konsistent sind - der in fixGroundedParts() als Ziel
    // vorgegebene Wert wird dann schlicht nicht durchgesetzt, das verbundene Teile-Cluster
    // bleibt als Ganzes um die fehlenden Freiheitsgrade frei verschiebbar/verdrehbar. Statt den
    // geteilten Solver-Kern anzufassen: die tatsaechlich geloeste Placement des (per Definition
    // genau EINEN) explizit geerdeten Teils dieser Ebene gegen den in fixGroundedParts()
    // gemerkten Zielwert (groundedTargetObj/groundedTargetPlc) vergleichen und die Differenz
    // als EINE starre Koordinaten-Umrechnung zurueckliefern, die auf ALLE geloesten
    // Placements dieser Ebene angewendet wird (siehe setNewPlacements()) - macht die Erdung
    // dadurch UNBEDINGT exakt, unabhaengig davon, was die Redundanz-Elimination intern
    // verworfen hat. Braucht dafuer KEINEN persistenten Zustand ueber solve()-Aufrufe hinweg:
    // da jeder solve()-Durchlauf die Erdung exakt auf den Wert zurueckbiegt, den
    // fixGroundedParts() fuer GENAU DIESEN Durchlauf live eingelesen hat, liest der naechste
    // solve()-Aufruf denselben (jetzt korrekten) Wert wieder als sein eigenes Ziel ein - ein
    // stabiler Fixpunkt, ganz ohne eingefrorene Property.
    if (!groundedTargetObj) {
        return Base::Placement();
    }

    auto it = objectPartMap.find(groundedTargetObj);
    if (it == objectPartMap.end() || !it->second.part) {
        return Base::Placement();
    }

    Base::Placement solvedActual = getMbdPlacement(it->second.part);
    if (!it->second.offsetPlc.isIdentity()) {
        solvedActual = solvedActual * it->second.offsetPlc;
    }

    return groundedTargetPlc * solvedActual.inverse();
}

void AssemblyObject::setNewPlacements()
{
    Base::Placement groundCorrection = computeGroundCorrection();

    for (auto& pair : objectPartMap) {
        App::DocumentObject* obj = pair.first;
        std::shared_ptr<ASMTPart> mbdPart = pair.second.part;

        if (!obj || !mbdPart) {
            continue;
        }

        // Check if the object has a "Placement" property
        auto* propPlacement = obj->getPlacementProperty();
        if (!propPlacement) {
            continue;
        }


        Base::Placement newPlacement = getMbdPlacement(mbdPart);
        if (!pair.second.offsetPlc.isIdentity()) {
            newPlacement = newPlacement * pair.second.offsetPlc;
        }
        if (!groundCorrection.isIdentity()) {
            // Global-starre Korrektur (siehe computeGroundCorrection()) - aendert keine
            // Relativgeometrie zwischen den Teilen, biegt nur das Gesamtergebnis so zurecht,
            // dass das geerdete Teil exakt an seinem eingefrorenen Platz landet.
            newPlacement = groundCorrection * newPlacement;
        }
        if (!propPlacement->getValue().isSame(newPlacement)) {
            propPlacement->setValue(newPlacement);
            obj->purgeTouched();
        }

        // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix,
        // 2026-09-03, erweitert 2026-09-04 - live durch Nutzer aufgedeckt: "Boxen nach dem
        // Laden werden bei Taste Z nicht linear/korrekt platziert"): bewusst AUSSERHALB des
        // obigen isSame()-Checks aufgerufen, nicht nur wenn sich das kanonische (echte) Objekt
        // selbst aendert. 'obj' ist hier der kanonische, ECHTE Pointer (dank
        // canonicalizeForMbD() in getMbDData()) - die lokale Spiegel-Kopie, die tatsaechlich in
        // DIESER Baugruppe gerendert/gezogen wird, ist ein SEPARATES Dokumentobjekt mit ihrem
        // EIGENEN, unabhaengig gespeicherten Placement-Wert. Steht das echte Objekt (z.B. weil
        // geerdet) schon VOR diesem solve() korrekt an seiner Zielposition, aendert sich
        // 'propPlacement' hier gar nicht - der alte Code rief syncLocalMirrorPlacement() dann
        // NIE auf, wodurch eine veraltete, direkt aus der Datei geladene lokale Spiegel-Kopie
        // (z.B. vom letzten Speichern VOR einer Bearbeitung an der echten Baugruppe) dauerhaft
        // falsch stehen blieb - sichtbar als "Box springt beim ersten Z auf unerwartete
        // Position", weil eine ANDERE, tatsaechlich verschobene Box (hier: BoxD) ueber ihren
        // Fixed-Joint an die inzwischen korrekt/aber-nie-synchronisierte lokale Kopie gekoppelt
        // wird. syncLocalMirrorPlacement() hat selbst bereits einen eigenen isSame()-Check,
        // unnoetige Schreibvorgaenge auf bereits korrekte lokale Kopien bleiben also trotzdem
        // aus - nur der FEHLENDE erste Sync-Versuch wird hier nachgeholt.
        syncLocalMirrorPlacement(obj, newPlacement);
    }
}

namespace
{
// FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix, 2026-09-03):
// sammelt alle lokal gespiegelten "Blatt"-Objekte (Leaf-Kandidaten fuer syncLocalMirrorPlacement())
// unterhalb von 'objects' ein - steigt dabei in FLEXIBLE AssemblyLinks und normale Gruppen ab,
// nimmt eine RIGIDE AssemblyLink dagegen selbst als Kandidat (sie ist die atomare Spiegel-Einheit
// fuer den Solver, siehe canonicalizeForMbD()). Bewusst identisches Abstiegsmuster wie
// collectComponentsRecursively() (AssemblyUtils.cpp) - nur ohne dessen GeoFeature-Filter, da hier
// auch reine App::Link-Blaetter als Kandidaten in Frage kommen.
void collectLocalMirrorCandidates(
    const std::vector<App::DocumentObject*>& objects,
    std::vector<App::DocumentObject*>& out
)
{
    for (auto* obj : objects) {
        if (!obj) {
            continue;
        }
        if (auto* asmLink = freecad_cast<Assembly::AssemblyLink*>(obj)) {
            if (asmLink->isRigid()) {
                out.push_back(obj);
            }
            else {
                collectLocalMirrorCandidates(asmLink->Group.getValues(), out);
            }
            continue;
        }
        if (auto* group = freecad_cast<App::DocumentObjectGroup*>(obj)) {
            collectLocalMirrorCandidates(group->Group.getValues(), out);
            continue;
        }
        out.push_back(obj);
    }
}
}  // namespace

// FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix, 2026-09-03,
// live durch Nutzer-Maus-Drag aufgedeckt): schliesst die zuletzt gefundene Luecke -
// AssemblyLink::synchronizeComponents() spiegelt die Placement eines lokalen Spiegel-Objekts
// (das, was tatsaechlich in der 3D-Ansicht gerendert und mit der Maus gezogen wird) nur bei
// Rigid=true vom echten Quellobjekt zurueck; bei flexiblen Unterbaugruppen fehlt dieser Ruecksync
// komplett. Ein Versuch, das direkt in AssemblyLink.cpp analog zu ergaenzen, scheitert an der
// Ausfuehrungsreihenfolge: AssemblyLink::execute() (das updateContents()/
// synchronizeComponents() aufruft) laeuft INNERHALB DESSELBEN Recompute-Durchlaufs VOR dem
// solve() der Baugruppe, deren Ergebnis gespiegelt werden muesste - live per Debug-Logging
// bestaetigt. Ein Sync dort wuerde also immer die VERALTETEN Werte kopieren.
//
// Stattdessen hier, direkt im Anschluss an das Schreiben des kanonischen (echten) Placement-
// Werts in setNewPlacements() - zu diesem Zeitpunkt ist der frisch geloeste Wert garantiert
// aktuell, und alles laeuft synchron innerhalb DESSELBEN solve()-Aufrufs, ganz ohne Abhaengigkeit
// von einer Recompute-Reihenfolge zwischen verschiedenen Objekten. Sucht unter allen lokal
// gespiegelten Kandidaten (collectLocalMirrorCandidates()) denjenigen, der laut
// canonicalizeForMbD() - derselben, bereits verifizierten Aufloesung, die auch die Joints/
// Erdung auf 'realObj' abgebildet hat - genau 'realObj' entspricht, und schreibt dessen
// Placement ebenfalls. Bewusst als Brute-Force-Suche ueber ALLE Kandidaten implementiert statt
// als eigenstaendige "Rueckwaerts"-Aufloesung, um keine zweite, potenziell abweichende
// Namenspfad-Logik zu pflegen - canonicalizeForMbD() bleibt die einzige Quelle der Wahrheit fuer
// "was ist das echte Objekt hinter diesem lokalen Kandidaten".
void AssemblyObject::syncLocalMirrorPlacement(App::DocumentObject* realObj, const Base::Placement& plc)
{
    if (!realObj) {
        return;
    }

    std::vector<App::DocumentObject*> candidates;
    collectLocalMirrorCandidates(Group.getValues(), candidates);

    for (auto* candidate : candidates) {
        if (!candidate || candidate == realObj) {
            continue;
        }
        if (canonicalizeForMbD(candidate) != realObj) {
            continue;
        }

        auto* propPlc = candidate->getPlacementProperty();
        if (!propPlc) {
            continue;
        }
        if (!propPlc->getValue().isSame(plc)) {
            propPlc->setValue(plc);
            candidate->purgeTouched();
        }
    }
}

// FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix, 2026-09-03):
// siehe ausfuehrliche Begruendung am Deklarationsort in AssemblyObject.h.
bool AssemblyObject::hasRealObject(App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }
    if (hasObject(obj, true)) {
        return true;
    }

    std::vector<App::DocumentObject*> candidates;
    collectLocalMirrorCandidates(Group.getValues(), candidates);

    for (auto* candidate : candidates) {
        if (candidate && canonicalizeForMbD(candidate) == obj) {
            return true;
        }
    }
    return false;
}

void AssemblyObject::redrawJointPlacements(std::vector<App::DocumentObject*> joints)
{
    // Notify the joint objects that the transform of the coin object changed.
    for (auto* joint : joints) {
        if (!joint) {
            continue;
        }
        redrawJointPlacement(joint);
    }
}

void AssemblyObject::redrawJointPlacement(App::DocumentObject* joint)
{
    if (!joint) {
        return;
    }

    Base::PyGILStateLocker lock;

    App::PropertyPythonObject* proxy = joint
        ? dynamic_cast<App::PropertyPythonObject*>(joint->getPropertyByName("Proxy"))
        : nullptr;

    if (!proxy) {
        return;
    }

    Py::Object jointPy = proxy->getValue();

    if (!jointPy.hasAttr("redrawJointPlacements")) {
        return;
    }

    Py::Object attr = jointPy.getAttr("redrawJointPlacements");
    if (attr.ptr() && attr.isCallable()) {
        Py::Tuple args(1);
        args.setItem(0, Py::asObject(joint->getPyObject()));
        Py::Callable(attr).apply(args);
    }
}

std::shared_ptr<ASMTAssembly> AssemblyObject::makeMbdAssembly()
{
    auto assembly = CREATE<ASMTAssembly>::With();
    assembly->externalSystem->freecadAssemblyObject = this;
    assembly->setName("OndselAssembly");

    ParameterGrp::handle hPgr = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );

    assembly->setDebug(hPgr->GetBool("LogSolverDebug", false));
    return assembly;
}

App::DocumentObject* AssemblyObject::getJointOfPartConnectingToGround(
    App::DocumentObject* part,
    std::string& name,
    const std::vector<App::DocumentObject*>& excludeJoints
)
{
    if (!part) {
        return nullptr;
    }

    std::vector<App::DocumentObject*> joints = getJointsOfPart(part);

    for (auto joint : joints) {
        if (!joint) {
            continue;
        }

        if (std::ranges::find(excludeJoints, joint) != excludeJoints.end()) {
            continue;
        }

        App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
        App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
        if (!part1 || !part2) {
            continue;
        }

        if (part == part1 && isJointConnectingPartToGround(joint, "Reference1")) {
            name = "Reference1";
            return joint;
        }
        if (part == part2 && isJointConnectingPartToGround(joint, "Reference2")) {
            name = "Reference2";
            return joint;
        }
    }
    return nullptr;
}

template<typename T>
T* AssemblyObject::getGroup()
{
    App::Document* doc = getDocument();

    std::vector<DocumentObject*> groups = doc->getObjectsOfType(T::getClassTypeId());
    if (groups.empty()) {
        return nullptr;
    }
    for (auto group : groups) {
        if (hasObject(group)) {
            return freecad_cast<T*>(group);
        }
    }
    return nullptr;
}

JointGroup* AssemblyObject::getJointGroup() const
{
    return Assembly::getJointGroup(this);
}

ViewGroup* AssemblyObject::getExplodedViewGroup() const
{
    App::Document* doc = getDocument();

    std::vector<DocumentObject*> viewGroups = doc->getObjectsOfType(ViewGroup::getClassTypeId());
    if (viewGroups.empty()) {
        return nullptr;
    }
    for (auto viewGroup : viewGroups) {
        if (hasObject(viewGroup)) {
            return freecad_cast<ViewGroup*>(viewGroup);
        }
    }
    return nullptr;
}

std::vector<App::DocumentObject*> Assembly::extractJointObjects(const std::vector<JointRef>& refs)
{
    std::vector<App::DocumentObject*> result;
    result.reserve(refs.size());
    for (auto& r : refs) {
        result.push_back(r.joint);
    }
    return result;
}

std::vector<JointRef> AssemblyObject::getJoints(
    bool delBadJoints,
    bool subJoints,
    bool verboseLog,
    const std::string& nestingPrefix
)
{
    std::vector<JointRef> joints = {};

    JointGroup* jointGroup = getJointGroup();
    if (!jointGroup) {
        return {};
    }

    // FCPROJECT-PATCH (16): Nutzerwunsch - sichtbar machen, WELCHE Joints hier pro Solve
    // uebernommen und welche warum uebersprungen werden. Hintergrund: bei verschachtelten
    // Baugruppen kann ein Joint STILLSCHWEIGEND rausfallen, wenn beide Referenzen ueber
    // getMovingPartFromRef() auf DASSELBE aeussere Bauteil abgebildet werden (z.B. weil Rotor UND
    // Gehaeuse beide innerhalb DERSELBEN als "Rigid" markierten Unter-Baugruppe liegen - dann
    // sieht dieser Joint von hier aus wie ein selbst-verweisender/inkohaerenter Joint aus, obwohl
    // er weiter unten in der eigentlich zustaendigen inneren Baugruppe voellig normal waere).
    // Ohne diese Meldung ist das von aussen nicht von einem echten "Teil geloescht"-Leichenjoint
    // zu unterscheiden - beide Faelle liefen bisher identisch lautlos durch.
    // FCPROJECT-PATCH (16, nachgebessert): NUR noch hinter verboseLog, siehe Header-Kommentar -
    // isPartConnected()/getJointsOfPart() rufen diese Funktion waehrend einer interaktiven
    // Zieh-Bewegung (preDrag()) potenziell auf jedem Mausereignis auf; unbedingtes Logging hat dort
    // spuerbar CPU gekostet (Report-View-Textausgabe ist pro Aufruf nicht billig).
    Base::PyGILStateLocker lock;
    for (auto joint : jointGroup->getObjects()) {
        if (!joint) {
            continue;
        }

        auto* prop = dynamic_cast<App::PropertyBool*>(joint->getPropertyByName("Suppressed"));
        // FCPROJECT-PATCH (Teilschritt 3.1b): der urspruengliche intermittierende
        // isPartConnected()-Ausfall vom 2026-09-04 ist geklaert (siehe
        // reference-nested-grounding-leak-bugfix.md) - das Log muss dafuer nicht mehr unbedingt
        // laufen. Hinter verboseLog, aus demselben CPU-Grund wie oben (preDrag()-Heisspfad).
        if (verboseLog) {
            Base::Console().log(
                "FCPROJECT-DEBUG getJoints: joint='%s' isError=%d Suppressed=%d\n",
                joint->getNameInDocument(),
                joint->isError() ? 1 : 0,
                (prop && prop->getValue()) ? 1 : 0
            );
        }
        if (joint->isError() || !prop || prop->getValue()) {
            // Filter grounded joints and deactivated joints.
            if (verboseLog) {
                Base::Console().message(
                    "Assembly: getJoints('%s') - Joint '%s' uebersprungen (isError=%d, "
                    "Suppressed=%d).\n",
                    getFullName(),
                    joint->getNameInDocument(),
                    joint->isError() ? 1 : 0,
                    (prop && prop->getValue()) ? 1 : 0
                );
            }
            continue;
        }

        auto* part1 = getMovingPartFromRef(joint, "Reference1");
        auto* part2 = getMovingPartFromRef(joint, "Reference2");
        if (verboseLog) {
            Base::Console().log(
                "FCPROJECT-DEBUG getJoints: joint='%s' part1='%s' part2='%s'\n",
                joint->getNameInDocument(),
                part1 ? part1->getFullName().c_str() : "<null>",
                part2 ? part2->getFullName().c_str() : "<null>"
            );
        }
        if (!part1 || !part2 || part1->getFullName() == part2->getFullName()) {
            // Remove incomplete joints. Left-over when the user deletes a part.
            // Remove incoherent joints (self-pointing joints)
            if (verboseLog) {
                Base::Console().message(
                    "Assembly: getJoints('%s') - Joint '%s' uebersprungen: part1='%s', "
                    "part2='%s' (unvollstaendig oder BEIDE Referenzen zeigen auf dasselbe "
                    "aeussere Bauteil - z.B. weil beide innerhalb derselben Rigid-Unterbaugruppe "
                    "liegen).\n",
                    getFullName(),
                    joint->getNameInDocument(),
                    part1 ? part1->getFullName().c_str() : "<null>",
                    part2 ? part2->getFullName().c_str() : "<null>"
                );
            }
            if (delBadJoints) {
                getDocument()->removeObject(joint->getNameInDocument());
            }
            continue;
        }

        auto proxy = dynamic_cast<App::PropertyPythonObject*>(joint->getPropertyByName("Proxy"));
        if (proxy) {
            if (proxy->getValue().hasAttr("setJointConnectors")) {
                if (verboseLog) {
                    Base::Console().message(
                        "Assembly: getJoints('%s') - Joint '%s' UEBERNOMMEN: part1='%s', "
                        "part2='%s'.\n",
                        getFullName(),
                        joint->getNameInDocument(),
                        part1->getFullName().c_str(),
                        part2->getFullName().c_str()
                    );
                }
                // FCPROJECT-PATCH (Migrationsschritt 3 "Adressieren statt Kopieren"): das
                // nestingPrefix wird jetzt direkt im JointRef mitgefuehrt (frueher zusaetzlich in
                // jointNestingPrefixMap geschrieben, siehe Deklarationsort in AssemblyObject.h) -
                // fuer einen Joint, der direkt in dieser Instanz liegt (der unveraenderte
                // Normalfall), ist nestingPrefix "".
                joints.push_back({joint, nestingPrefix});
            }
            else if (verboseLog) {
                Base::Console().message(
                    "Assembly: getJoints('%s') - Joint '%s' uebersprungen: Proxy hat keine "
                    "setJointConnectors-Methode (kein normaler beweglicher Joint, z.B. Grounded/"
                    "RigidGroup).\n",
                    getFullName(),
                    joint->getNameInDocument()
                );
            }
        }
    }

    // add sub assemblies joints.
    //
    // FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix, siehe
    // patches/assembly-architecture-overview.md, Abschnitte "Teilschritt 2 - Konkreter
    // Detailplan"/"Teilschritt 2 - Umsetzung"): liest nicht mehr die bereits KOPIERTEN Joints aus
    // der AssemblyLink-eigenen JointGroup (assembly->getJoints(), die parameterlose
    // AssemblyLink-Methode - reiner JointGroup-Zugriff), sondern steigt in die ECHTE, verlinkte
    // AssemblyObject-Instanz ab (getLinkedAssembly()) und liest deren ORIGINAL-Joints direkt,
    // rekursiv, mit korrekt mitgefuehrtem nestingPrefix.
    //
    // Rigide (Rigid=true) Unter-AssemblyLinks werden EXPLIZIT uebersprungen: eine rigide
    // Unterbaugruppe verhaelt sich fuer den Solver wie ein einziges starres Teil, ihre INTERNEN
    // Joints duerfen NICHT zusaetzlich in den Solve der aeusseren, rigide einbindenden Baugruppe
    // einfliessen.
    if (subJoints) {
        for (auto* assembly : getSubAssemblies()) {
            if (!assembly || assembly->isRigid()) {
                continue;
            }

            AssemblyObject* nestedAssembly = assembly->getLinkedAssembly();
            if (!nestedAssembly || nestedAssembly == this) {
                // Defensiv: kaputter Link, oder eine (sollte nie vorkommen) Selbstreferenz - nie
                // eine Endlosrekursion riskieren.
                continue;
            }

            std::string nestedPrefix = nestingPrefix + assembly->getNameInDocument() + ".";
            auto nestedJoints
                = nestedAssembly->getJoints(delBadJoints, subJoints, verboseLog, nestedPrefix);
            // FCPROJECT-PATCH (Migrationsschritt 3 "Adressieren statt Kopieren"): jedes
            // zurueckgelieferte JointRef traegt sein nestingPrefix schon korrekt (voll
            // akkumuliert, siehe nestedPrefix oben) direkt mit sich - das vorherige manuelle
            // Nachziehen ueber die jointNestingPrefixMap der rekursiv aufgerufenen Instanz
            // entfaellt ersatzlos.
            joints.insert(joints.end(), nestedJoints.begin(), nestedJoints.end());
        }
    }

    return joints;
}

std::vector<App::DocumentObject*> AssemblyObject::getGroundedJoints()
{
    std::vector<App::DocumentObject*> joints = {};

    JointGroup* jointGroup = getJointGroup();
    if (!jointGroup) {
        return {};
    }

    Base::PyGILStateLocker lock;
    for (auto obj : jointGroup->getObjects()) {
        if (!obj) {
            continue;
        }

        auto* propObj = dynamic_cast<App::PropertyLink*>(obj->getPropertyByName("ObjectToGround"));

        if (propObj) {
            joints.push_back(obj);
        }
    }

    return joints;
}

std::vector<App::DocumentObject*> AssemblyObject::getRigidGroups()
{
    std::vector<App::DocumentObject*> rigid_groups {};

    JointGroup* jointGroup = getJointGroup();
    if (!jointGroup) {
        return {};
    }

    Base::PyGILStateLocker lock;
    for (auto const obj : jointGroup->getObjects()) {
        if (!obj || obj->isError()) {
            continue;
        }

        if (auto* prop = dynamic_cast<App::PropertyBool*>(obj->getPropertyByName("Suppressed"));
            prop == nullptr || prop->getValue()) {
            continue;
        }

        if (auto* prop
            = dynamic_cast<App::PropertyLinkList*>(obj->getPropertyByName("ObjectsToRigidGroup"));
            prop) {
            const std::vector<App::DocumentObject*> rawMembers = prop->getValues();
            std::vector<App::DocumentObject*> validMembers;
            validMembers.reserve(rawMembers.size());
            std::unordered_set<App::DocumentObject*> seen;

            for (auto* const member : rawMembers) {
                if (!member || member->isError()) {
                    continue;
                }

                // Keep only parts that belong to this assembly and have placement.
                if (!hasObject(member) || member->getPropertyByName("Placement") == nullptr) {
                    continue;
                }

                // Ignore duplicates.
                if (!seen.insert(member).second) {
                    continue;
                }

                validMembers.push_back(member);
            }

            // Ignore entire rigid group if it has less than 2 members remaining.
            if (validMembers.size() < 2) {
                continue;
            }

            if (validMembers.size() != rawMembers.size()) {
                prop->setValue(validMembers);
            }

            rigid_groups.emplace_back(obj);
        }
    }

    return rigid_groups;
}

std::vector<App::DocumentObject*> AssemblyObject::getJointsOfObj(App::DocumentObject* obj)
{
    if (!obj) {
        return {};
    }

    std::vector<App::DocumentObject*> joints = extractJointObjects(getJoints());
    std::vector<App::DocumentObject*> jointsOf;

    for (auto joint : joints) {
        App::DocumentObject* obj1 = getObjFromJointRef(joint, "Reference1");
        App::DocumentObject* obj2 = getObjFromJointRef(joint, "Reference2");
        if (obj == obj1 || obj == obj2) {
            jointsOf.push_back(joint);
        }
    }

    return jointsOf;
}

std::vector<App::DocumentObject*> AssemblyObject::getJointsOfPart(App::DocumentObject* part)
{
    if (!part) {
        return {};
    }

    // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix, 2026-09-03):
    // 'part' auf denselben kanonischen Identitaetsraum gebracht wie die Joint-Endpunkte unten -
    // sonst findet der Vergleich fuer ein bereits kanonisiertes 'part' (z.B. aus der Fixed-Joint-
    // Rekursion in getMbDData(), siehe dortiger Kommentar) einen rein lokal referenzierenden
    // Joint nie, selbst wenn beide dasselbe reale Teil meinen.
    App::DocumentObject* canonicalPart = canonicalizeForMbD(part);

    std::vector<App::DocumentObject*> joints = extractJointObjects(getJoints());
    std::vector<App::DocumentObject*> jointsOf;

    for (auto joint : joints) {
        App::DocumentObject* part1 = canonicalizeForMbD(getMovingPartFromRef(joint, "Reference1"));
        App::DocumentObject* part2 = canonicalizeForMbD(getMovingPartFromRef(joint, "Reference2"));
        if (canonicalPart == part1 || canonicalPart == part2) {
            jointsOf.push_back(joint);
        }
    }
    return jointsOf;
}

std::unordered_set<App::DocumentObject*> AssemblyObject::getGroundedParts()
{
    std::unordered_set<App::DocumentObject*> groundedSet;
    std::vector<App::DocumentObject*> allParts = getAssemblyComponents(this);
    for (auto part : allParts) {
        if (part) {
            auto propPlc = part->getPlacementProperty();
            if (propPlc && propPlc->isReadOnly()) {
                groundedSet.insert(part);
            }
        }
    }

    // We also need to add all the root-level datums objects that are not attached.
    std::vector<App::DocumentObject*> objs = Group.getValues();
    for (auto* obj : objs) {
        if (obj->isDerivedFrom<App::LocalCoordinateSystem>()
            || obj->isDerivedFrom<App::DatumElement>()) {
            auto* pcAttach = obj->getExtensionByType<PartApp::AttachExtension>();
            if (pcAttach) {
                // If it's a Part datums, we check if it's attached. If yes then we ignore it.
                std::string mode = pcAttach->MapMode.getValueAsString();
                if (mode != "Deactivated") {
                    continue;
                }
            }
            groundedSet.insert(obj);
        }
    }

    // Origin is not in Group so we add it separately.
    // FCPROJECT-PATCH (14): nur einfuegen, wenn tatsaechlich gesetzt - direkt beim Laden (bevor
    // der Origin des Assembly-Objekts initialisiert ist) liefert Origin.getValue() nullptr,
    // was bisher ungeprueft in groundedSet landete. Zaehlte dann als "1 grounded part", obwohl
    // gar kein echtes Teil gemeint war - fiel erst mit Fix 13 auf, weil die Namensausgabe fuer
    // diesen Phantom-Eintrag leer blieb (Nullpointer wird beim Benennen korrekt uebersprungen,
    // die Anzahl blieb davon aber unberuehrt).
    if (auto* origin = Origin.getValue()) {
        groundedSet.insert(origin);
    }

    // FCPROJECT-PATCH (Befund 3, Teilschritt 2e "adressieren statt kopieren", solver-root-cause-
    // fix): geerdete Teile aus verschachtelten FLEXIBLEN AssemblyLinks zusaetzlich rekursiv
    // einsammeln - direkt bei der ECHTEN, verlinkten AssemblyObject-Instanz erfragt
    // (getLinkedAssembly()->getGroundedParts()), NICHT ueber das oben bereits ausgewertete
    // isReadOnly()-Flag der LOKALEN Spiegel-Kopie. Grund: dieses Flag wird durch die bestehende
    // Kopier-Pipeline nachweislich NUR EINE Ebene tief zuverlaessig synchronisiert (live per
    // Debug-Logging bestaetigt: ein 2 Ebenen tief geerdetes Teil taucht in getGroundedParts()
    // dieser AssemblyObject-Instanz bislang UEBERHAUPT NICHT auf) - ohne diese Ergaenzung bleibt
    // ein Joint, dessen einzige Verbindung zum Rest der Baugruppe ueber ein 2+ Ebenen tief
    // geerdetes Teil laeuft, in removeUnconnectedJoints() faelschlich fuer immer "nicht
    // erreichbar", selbst nachdem canonicalizeForMbD() (s.o.) die Identitaetsraeume angeglichen
    // hat. Gleiches Rigid-Skip-Prinzip wie bei getJoints()' subJoints-Rekursion: eine rigide
    // Unter-Baugruppe verhaelt sich wie ein einziges starres Teil, ihre INTERNE Erdung ist fuer
    // den Solver dieser Ebene irrelevant. Die von der rekursiv aufgerufenen Instanz gelieferten
    // Objekte liegen bereits in DEREN eigenem, "echten" Identitaetsraum - exakt das, was
    // canonicalizeForMbD() fuer denselben Namenspfad ohnehin liefern wuerde - und werden daher
    // unveraendert uebernommen.
    // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix, 2026-09-04,
    // Nutzerkorrektur): die obige Rekursion allein war zu aggressiv - sie zieht die Erdung eines
    // verschachtelten Teils IMMER mit rein, selbst wenn dieses Teil bereits ueber eine ganz
    // normale Joint-Kette von der AEUSSEREN Erdung dieser Ebene aus erreichbar ist (z.B. BoxC
    // hier -[Slider]- BoxD -[Fixed]- BoxA, wobei BoxA IN SUB zusaetzlich sein eigenes,
    // eigenstaendiges GroundedJoint hat). Ergebnis: zwei widerspruechliche Vorschriften fuer
    // dasselbe Teil ("bleib hier fest" vs. "beweg dich starr mit BoxD mit"), vom Solver als
    // "redundant" gemeldet und mit chaotischem, nicht-linearem Ergebnis aufgeloest. Im
    // unveraenderten FreeCAD (vor Teilschritt 2e) war dieses Verhalten nicht sichtbar, weil
    // getGroundedParts() gar nicht rekursiv war - Subs eigene Erdung war fuer diese Ebene
    // schlicht unsichtbar, die Erdung "wanderte" effektiv komplett zur aeusseren Kette (BoxC).
    // Fix: vor dem Uebernehmen einer verschachtelten Erdung pruefen, ob das betroffene Teil
    // bereits ueber die (bereits adressierungsbewusste) Joint-Kette von der LOKALEN, nicht-
    // rekursiven Erdung dieser Ebene aus erreichbar ist - wenn ja, hat die AEUSSERE Kette
    // Vorrang und die verschachtelte Erdung wird NICHT zusaetzlich uebernommen (kein doppelter
    // Eintrag, keine Redundanz). Nur wenn ein verschachteltes Teil auf KEINE andere Weise
    // erreichbar waere, zaehlt seine eigene Erdung weiterhin (z.B. wenn die betroffene
    // Unterbaugruppe gar nicht an die aeussere Kette angebunden ist).
    //
    // FCPROJECT-PATCH (Migrationsschritt 4.5 "Adressieren statt Kopieren", siehe
    // docs/ARCHITECTURE.md Abschnitt 5): die Berechnung von reachableFromLocalGrounding/
    // reachabilityJoints, die hier bis 2026-09-11 stand, war bereits seit der 2026-09-09-
    // Entscheidung direkt darunter (s.u., "ENTFERNT statt weiter ausgeflickt") toter Code -
    // berechnet, aber nie gelesen (die Funktion gibt weiter unten unveraendert groundedSet
    // zurueck). Als Aufraeum-Rest jetzt entfernt.
    //
    // FCPROJECT-PATCH (2026-09-09, ENTFERNT statt weiter ausgeflickt - siehe Befund-3-Kommentar
    // oben fuer die Vorgeschichte dieses Blocks): die rekursive Uebernahme einer verschachtelten
    // Unterbaugruppe eigener interner Erdung (nestedAssembly->getGroundedParts()) erwies sich
    // als grundsaetzlich falsch dosiert, egal wie fein die Ausnahme dafuer gefasst wurde - live
    // reproduziert in ZWEI verschiedenen, gegensaetzlich unmoeglich gleichzeitig zu loesenden
    // Faellen:
    // 1. Eine frisch eingefuegte, KOMPLETT unverbundene flexible Unterbaugruppe (kein aeusserer
    //    Joint) wurde dadurch sofort komplett unbeweglich, statt frei verschiebbar zu bleiben.
    // 2. Selbst mit einem aeusseren Joint (z.B. GrandTop-BoxC zu Subs BoxB) blieb Subs davon
    //    UNABHAENGIGE eigene Erdung (BoxA) bestehen, sobald der INNERE Joint (BoxA-BoxB)
    //    geloescht wurde - obwohl BoxA dann eine eigene, vom aeusseren Anschlusspunkt komplett
    //    GETRENNTE Insel ist, fror das die ganze eingebettete Baugruppe grundlos ein.
    // Ein Versuch, Fall 1 durch eine dokumentweite Erreichbarkeitspruefung zu loesen ("ist
    // UEBERHAUPT etwas aus diesem Dokument erreichbar") behob Fall 1, machte aber Fall 2 nicht
    // besser (BoxB IST erreichbar, die Erdung von BoxA - einer davon komplett getrennten Insel
    // im selben Dokument - wurde trotzdem weiter reingezogen). Eine wirklich korrekte Loesung
    // braeuchte eine Pro-Insel- statt Pro-Dokument-Erreichbarkeitspruefung (ueber Subs EIGENE
    // interne Joints, unabhaengig vom aeusseren Anschluss) - dafuer besteht aber der begruendete
    // Verdacht, dass sie sich mit der urspruenglichen Absicht dieses Blocks (ein 2+ Ebenen tief
    // NUR ueber sein eigenes GroundedJoint erreichbares Teil, dessen NORMALE Joint-Kette zur
    // Wurzel aus einem NUR HIER liegenden Sync-Bug der alten Kopier-Pipeline heraus nicht
    // erkannt wurde) nicht sauber unterscheiden laesst: beide Situationen sehen aus reiner
    // Joint-Graph-Sicht identisch aus ("Teil X ist ueber keinen Reference1/2-Joint-Pfad von der
    // lokalen Erdung dieser Ebene aus erreichbar"). Nutzerentscheidung (2026-09-09): diesen
    // gesamten Rekursionsblock ERSATZLOS entfernen, um die BEIDEN reproduzierten, aktuellen
    // Regressionsfaelle zu beheben - reine Joint-Graph-Erreichbarkeit (die dank getJoints()'
    // eigener subJoints-Rekursion bereits echte, tief verschachtelte Joint-Ketten korrekt mit
    // einschliesst, siehe removeUnconnectedJoints()/getConnectedParts()) reicht als alleinige
    // Grundlage. Falls die
    // urspruengliche Befund-3-Situation (Sync-Bug der alten Kopier-Pipeline bei isReadOnly())
    // dadurch wieder auftritt, ist das ein bekanntes, akzeptiertes Risiko dieser Entscheidung -
    // noch nicht erneut getestet, siehe [[reference-nested-grounding-leak-bugfix]].

    // Propagate grounding through active rigid clusters.
    std::vector<App::DocumentObject*> groundedSnapshot(groundedSet.begin(), groundedSet.end());
    for (auto* groundedObj : groundedSnapshot) {
        if (!groundedObj) {
            continue;
        }

        if (const auto* members = getRigidMembers(groundedObj)) {
            for (auto* member : *members) {
                if (member) {
                    groundedSet.insert(member);
                }
            }
        }
    }

    return groundedSet;
}

std::unordered_set<App::DocumentObject*> AssemblyObject::fixGroundedParts()
{
    auto groundedParts = getGroundedParts();

    // FCPROJECT-PATCH (2026-09-13, "geerdetes Teil springt beim Anlegen weiterer Joints" -
    // siehe docs/ARCHITECTURE.md §2.1a): Live-Placement-Lesen bleibt hier UNVERAENDERT (jeder
    // solve()-Durchlauf nimmt weiterhin den aktuellen Ist-Wert als Ziel fuer den Solver) - das
    // eigentliche Problem war nie DAS Lesen selbst, sondern dass OndselSolvers
    // Redundanz-Elimination die Erdungs-Gleichungen anschliessend verwerfen und den
    // vorgegebenen Zielwert dadurch stillschweigend IGNORIEREN kann (siehe
    // computeGroundCorrection()). Merkt sich hier deshalb zusaetzlich, WELCHES Objekt per
    // echtem GroundedJoint (nicht Origin/Datums/Rigid-Gruppen-Mitglieder) geerdet ist und WAS
    // sein vorgegebenes Ziel fuer DIESEN Durchlauf war (groundedTargetObj/groundedTargetPlc,
    // AssemblyObject.h) - computeGroundCorrection() biegt danach alle geloesten Placements
    // exakt darauf zurueck, egal was der Solver intern verworfen hat. Nur EINE Erdung pro
    // Assembly-Ebene (Nutzerkorrektur: "egal wieviele Teile dazu gehoeren") - die erste
    // gefundene gewinnt.
    groundedTargetObj = nullptr;
    std::unordered_set<App::DocumentObject*> explicitlyGroundedTargets;
    for (auto* jointObj : getGroundedJoints()) {
        if (!jointObj) {
            continue;
        }
        auto* propObj
            = dynamic_cast<App::PropertyLink*>(jointObj->getPropertyByName("ObjectToGround"));
        if (propObj && propObj->getValue()) {
            explicitlyGroundedTargets.insert(propObj->getValue());
        }
    }

    for (auto obj : groundedParts) {
        if (!obj) {
            continue;
        }

        Base::Placement plc = getPlacementFromProp(obj, "Placement");
        if (!groundedTargetObj && explicitlyGroundedTargets.count(obj)) {
            groundedTargetObj = obj;
            groundedTargetPlc = plc;
        }
        std::string str = obj->getFullName();
        fixGroundedPart(obj, plc, str);
    }
    return groundedParts;
}

void AssemblyObject::fixGroundedPart(App::DocumentObject* obj, Base::Placement& plc, std::string& name)
{
    if (!obj) {
        return;
    }

    std::string markerName1 = "marker-" + obj->getFullName();
    auto mbdMarker1 = makeMbdMarker(markerName1, plc);
    mbdAssembly->addMarker(mbdMarker1);

    std::shared_ptr<ASMTPart> mbdPart = getMbDPart(obj);

    std::string markerName2 = "FixingMarker";
    Base::Placement basePlc = Base::Placement();
    auto mbdMarker2 = makeMbdMarker(markerName2, basePlc);
    mbdPart->addMarker(mbdMarker2);

    markerName1 = "/OndselAssembly/" + mbdMarker1->name;
    markerName2 = "/OndselAssembly/" + mbdPart->name + "/" + mbdMarker2->name;

    auto mbdJoint = CREATE<ASMTFixedJoint>::With();
    mbdJoint->setName(name);
    mbdJoint->setMarkerI(markerName1);
    mbdJoint->setMarkerJ(markerName2);

    mbdAssembly->addJoint(mbdJoint);
}

bool AssemblyObject::isJointConnectingPartToGround(App::DocumentObject* joint, const char* propname)
{
    if (!joint || !isJointTypeConnecting(joint)) {
        return false;
    }

    App::DocumentObject* part = getMovingPartFromRef(joint, propname);
    if (!part) {
        return false;
    }

    // Check if the part is grounded.
    bool isGrounded = isPartGrounded(part);
    if (isGrounded) {
        return false;
    }

    // Check if the part is disconnected even with the joint
    bool isConnected = isPartConnected(part);
    if (!isConnected) {
        return false;
    }

    // to know if a joint is connecting to ground we disable all the other joints
    std::vector<App::DocumentObject*> jointsOfPart = getJointsOfPart(part);
    std::vector<bool> activatedStates;

    for (auto jointi : jointsOfPart) {
        if (jointi->getFullName() == joint->getFullName()) {
            continue;
        }

        activatedStates.push_back(getJointActivated(jointi));
        setJointActivated(jointi, false);
    }

    isConnected = isPartConnected(part);

    // restore activation states
    for (auto jointi : jointsOfPart) {
        if (jointi->getFullName() == joint->getFullName() || activatedStates.empty()) {
            continue;
        }

        setJointActivated(jointi, activatedStates[0]);
        activatedStates.erase(activatedStates.begin());
    }

    return isConnected;
}

bool AssemblyObject::isJointTypeConnecting(App::DocumentObject* joint)
{
    if (!joint) {
        return false;
    }

    JointType jointType = getJointType(joint);
    return jointType != JointType::RackPinion && jointType != JointType::Screw
        && jointType != JointType::Gears && jointType != JointType::Belt;
}


bool AssemblyObject::isObjInSetOfObjRefs(App::DocumentObject* obj, const std::vector<ObjRef>& set)
{
    if (!obj) {
        return false;
    }

    for (const auto& pair : set) {
        if (pair.obj == obj) {
            return true;
        }
    }
    return false;
}

void AssemblyObject::removeUnconnectedJoints(
    std::vector<JointRef>& joints,
    std::unordered_set<App::DocumentObject*> groundedObjs
)
{
    // FCPROJECT-PATCH (Befund 3, Teilschritt 2e "adressieren statt kopieren", solver-root-cause-
    // fix): groundedObjs kommt aus getGroundedParts() und liegt damit im LOKALEN Spiegel-
    // Identitaetsraum (siehe canonicalizeForMbD() fuer die ausfuehrliche Begruendung) - fuer die
    // Erreichbarkeits-Traversal unten (getConnectedParts() vergleicht gegen
    // resolvePartForMbD()-Ergebnisse, also ECHTE Objekte) muss der Startpunkt im SELBEN,
    // kanonischen Identitaetsraum liegen wie die Joint-Endpunkte weiter unten - sonst bricht eine
    // tatsaechlich erreichbare Kette schon am allerersten Schritt ab und der komplette
    // dahinterliegende, verschachtelte Zweig wird faelschlich als "nicht erreichbar" entfernt.
    std::unordered_set<App::DocumentObject*> canonicalGroundedObjs;
    for (auto* groundedObj : groundedObjs) {
        if (auto* canonical = canonicalizeForMbD(groundedObj)) {
            canonicalGroundedObjs.insert(canonical);
        }
    }

    std::vector<ObjRef> connectedParts;

    // Initialize connectedParts with groundedObjs
    for (auto* groundedObj : canonicalGroundedObjs) {
        connectedParts.push_back({groundedObj, nullptr});
    }

    // Perform a traversal from each grounded object
    for (auto* groundedObj : canonicalGroundedObjs) {
        traverseAndMarkConnectedParts(groundedObj, connectedParts, joints);
    }

    // FCPROJECT-PATCH (16): siehe getJoints() weiter oben - gleiches Prinzip, zweite Filterstufe.
    // Hier faellt ein Joint raus, wenn eine seiner beiden Seiten NICHT ueber eine Kette von
    // Joints von einem geerdeten Teil aus erreichbar ist (traverseAndMarkConnectedParts()).
    Base::Console().message(
        "Assembly: removeUnconnectedJoints('%s') - %zu geerdete(s) Teil(e), %zu erreichbare(s) "
        "Teil(e), pruefe %zu Joint(s).\n",
        getFullName(),
        canonicalGroundedObjs.size(),
        connectedParts.size(),
        joints.size()
    );

    // Filter out unconnected joints
    joints.erase(
        std::remove_if(
            joints.begin(),
            joints.end(),
            [&](const JointRef& jr) {
                App::DocumentObject* joint = jr.joint;
                // FCPROJECT-PATCH (Teilschritt 2c "adressieren statt kopieren", solver-root-
                // cause-fix): auf resolvePartForMbD() umgestellt, aus demselben Grund wie bei
                // isMbDJointValid()/handleOneSideOfJoint() - getGroundedParts() liefert bereits
                // lokale, ueber verschachtelte flexible AssemblyLinks aufgeloeste Spiegel-
                // Identitaeten, waehrend die alte getMovingPartFromRef() fuer einen tief
                // verschachtelten Joint das rohe, joint-lokale (ggf. cross-document) Original
                // liefert - beide Seiten muessen im SELBEN Identitaetsraum liegen, sonst erkennt
                // dieser Vergleich eine tatsaechlich erreichbare Kette faelschlich als nicht
                // erreichbar.
                //
                // FCPROJECT-PATCH (Mehrfachinstanz-Fix): jr.nestingPrefix
                // statt eines Nachschlagens in der (entfernten) jointNestingPrefixMap - siehe deren
                // vormaligen Deklarationsort in AssemblyObject.h fuer die Begruendung.
                App::DocumentObject* obj1 = resolvePartForMbD(joint, "Reference1", jr.nestingPrefix);
                App::DocumentObject* obj2 = resolvePartForMbD(joint, "Reference2", jr.nestingPrefix);
                bool obj1Connected = isObjInSetOfObjRefs(obj1, connectedParts);
                bool obj2Connected = isObjInSetOfObjRefs(obj2, connectedParts);
                if (!obj1Connected || !obj2Connected) {
                    Base::Console().message(
                        "Assembly: removeUnconnectedJoints('%s') - Joint '%s' entfernt: "
                        "part1='%s' (erreichbar=%d), part2='%s' (erreichbar=%d).\n",
                        getFullName(),
                        joint->getNameInDocument(),
                        obj1 ? obj1->getFullName().c_str() : "<null>",
                        obj1Connected ? 1 : 0,
                        obj2 ? obj2->getFullName().c_str() : "<null>",
                        obj2Connected ? 1 : 0
                    );
                    return true;
                }
                return false;
            }
        ),
        joints.end()
    );
}

void AssemblyObject::traverseAndMarkConnectedParts(
    App::DocumentObject* currentObj,
    std::vector<ObjRef>& connectedParts,
    const std::vector<JointRef>& joints
)
{
    // getConnectedParts returns the objs connected to the currentObj by any joint
    auto connectedObjs = getConnectedParts(currentObj, joints);
    for (auto& nextObjRef : connectedObjs) {
        if (!isObjInSetOfObjRefs(nextObjRef.obj, connectedParts)) {
            // Create a new ObjRef with the nextObj and a nullptr for PropertyXLinkSub*
            connectedParts.push_back(nextObjRef);
            traverseAndMarkConnectedParts(nextObjRef.obj, connectedParts, joints);
        }
    }
}

std::vector<ObjRef> AssemblyObject::getConnectedParts(
    App::DocumentObject* part,
    const std::vector<JointRef>& joints
)
{
    if (!part) {
        return {};
    }

    std::vector<ObjRef> connectedParts;

    for (auto& jr : joints) {
        App::DocumentObject* joint = jr.joint;
        if (!isJointTypeConnecting(joint)) {
            continue;
        }

        // FCPROJECT-PATCH (Teilschritt 2c "adressieren statt kopieren", solver-root-cause-fix):
        // auf resolvePartForMbD() umgestellt - siehe Begruendung beim Schwester-Aufruf in
        // removeUnconnectedJoints() (identisches Muster: 'part' kommt hier bereits als lokal
        // aufgeloeste Identitaet herein, obj1/obj2 muessen daher im SELBEN Identitaetsraum liegen).
        //
        // FCPROJECT-PATCH (Mehrfachinstanz-Fix): jr.nestingPrefix
        // statt eines Nachschlagens in der (entfernten) jointNestingPrefixMap.
        App::DocumentObject* obj1 = resolvePartForMbD(joint, "Reference1", jr.nestingPrefix);
        App::DocumentObject* obj2 = resolvePartForMbD(joint, "Reference2", jr.nestingPrefix);

        if (!obj1 || !obj2) {
            continue;
        }

        if (obj1 == part) {
            auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
            if (!ref) {
                continue;
            }
            connectedParts.push_back({obj2, ref});
        }
        else if (obj2 == part) {
            auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
            if (!ref) {
                continue;
            }
            connectedParts.push_back({obj1, ref});
        }
    }

    // Add rigid-cluster neighbors as fixed-like connectivity edges.
    if (const auto* members = getRigidMembers(part)) {
        for (auto* member : *members) {
            if (!member || member == part || isObjInSetOfObjRefs(member, connectedParts)) {
                continue;
            }
            connectedParts.push_back({member, nullptr});
        }
    }

    return connectedParts;
}

bool AssemblyObject::isPartGrounded(App::DocumentObject* obj)
{
    if (!obj) {
        return false;
    }

    auto groundedObjs = getGroundedParts();

    for (auto* groundedObj : groundedObjs) {
        if (groundedObj->getFullName() == obj->getFullName()) {
            return true;
        }
    }

    return false;
}

bool AssemblyObject::isPartConnected(App::DocumentObject* obj, bool verboseLog)
{
    if (!obj) {
        return false;
    }

    // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix, 2026-09-03,
    // live durch Nutzer-Maus-Drag aufgedeckt): dieselbe Identitaetsraum-Luecke wie in
    // removeUnconnectedJoints() (siehe dortiger Kommentar) - groundedObjs liegt im LOKALEN
    // Spiegel-Identitaetsraum, waehrend 'obj' seit dem getMovingPartFromSel()-Fix (s.o.) jetzt
    // bereits das ECHTE, tief verschachtelte Objekt sein kann. Ohne Kanonisierung findet die
    // Traversal unten nie eine Uebereinstimmung fuer ein solches Teil - preDrag() haelt es
    // faelschlich fuer "nicht verbunden" und laesst es komplett unbeschraenkt, frei in jede
    // Richtung ziehen (genau das vom Nutzer beobachtete Symptom, trotz korrekt aufgeloestem
    // getMovingPartFromSel()-Ergebnis).
    App::DocumentObject* canonicalObj = canonicalizeForMbD(obj);

    auto groundedObjs = getGroundedParts();
    // FCPROJECT-PATCH (Mehrfachinstanz-Fix): direkt der
    // JointRef-Vektor statt extractJointObjects() - traverseAndMarkConnectedParts() braucht das
    // nestingPrefix jedes Joints jetzt selbst (siehe dortiger Kommentar), zusaetzlich entfaellt
    // dadurch das bisherige stille Nachschlagen in der (entfernten) jointNestingPrefixMap, die
    // hier ohnehin nur den Stand des letzten solve()-Aufrufs widerspiegelte, nicht den gerade
    // frisch geholten.
    std::vector<JointRef> joints = getJoints();

    std::vector<ObjRef> connectedParts;

    std::unordered_set<App::DocumentObject*> canonicalGroundedObjs;
    for (auto* groundedObj : groundedObjs) {
        if (auto* canonical = canonicalizeForMbD(groundedObj)) {
            canonicalGroundedObjs.insert(canonical);
        }
    }

    // Initialize connectedParts with groundedObjs
    for (auto* groundedObj : canonicalGroundedObjs) {
        connectedParts.push_back({groundedObj, nullptr});
    }

    // Perform a traversal from each grounded object
    for (auto* groundedObj : canonicalGroundedObjs) {
        traverseAndMarkConnectedParts(groundedObj, connectedParts, joints);
    }

    // FCPROJECT-PATCH (Teilschritt 3.1b): der Drag-Pfad aus der urspruenglichen Befund-3-Diagnose
    // ist inzwischen nachvollzogen (siehe Kommentar oben an canonicalizeForMbD()) - Logs bleiben
    // fuer kuenftige Diagnosen erhalten, laufen aber nur noch hinter verboseLog, da
    // isPartConnected() auch aus dem preDrag()-Heisspfad heraus pro Mausereignis aufgerufen wird.
    if (verboseLog) {
        std::string names;
        for (auto& objRef : connectedParts) {
            if (!objRef.obj) {
                continue;
            }
            if (!names.empty()) {
                names += ", ";
            }
            names += objRef.obj->getFullName();
        }
        Base::Console().log(
            "FCPROJECT-DEBUG isPartConnected: obj='%s' canonicalObj='%s' -> connectedParts=[%s]\n",
            obj->getFullName().c_str(),
            canonicalObj ? canonicalObj->getFullName().c_str() : "<null>",
            names.c_str()
        );
    }

    for (auto& objRef : connectedParts) {
        if (canonicalObj == objRef.obj) {
            if (verboseLog) {
                Base::Console().log("FCPROJECT-DEBUG isPartConnected: -> TRUE\n");
            }
            return true;
        }
    }

    if (verboseLog) {
        Base::Console().log("FCPROJECT-DEBUG isPartConnected: -> FALSE\n");
    }
    return false;
}

// FCPROJECT-PATCH (Fix-Ansatz D, "flexible Unterbaugruppe als Ganzes ziehbar, wenn unverbunden",
// Nutzerauftrag 2026-09-16/17): siehe Deklaration in AssemblyObject.h fuer die volle Begruendung -
// isPartConnected(containerObj) allein erkennt ein bereits an EIN Kind angeschlossenes Kind
// nicht, weil ein Joint nie den Container selbst referenziert. Steigt rekursiv durch jede eigene
// flexible AssemblyLink ab, behandelt eine rigide wie ein Blatt (atomare Solver-Einheit, direkt
// per isPartConnected() geprueft).
bool AssemblyObject::isSubAssemblyFullyUnconnected(App::DocumentObject* obj)
{
    if (!obj) {
        return true;
    }

    if (auto* asmLink = freecad_cast<Assembly::AssemblyLink*>(obj)) {
        if (!asmLink->isRigid()) {
            for (auto* child : asmLink->Group.getValues()) {
                if (!child) {
                    continue;
                }
                if (child->isDerivedFrom<App::Link>() && child->isLinkGroup()) {
                    auto* link = static_cast<App::Link*>(child);
                    for (auto* elt : link->ElementList.getValues()) {
                        if (!isSubAssemblyFullyUnconnected(elt)) {
                            return false;
                        }
                    }
                    continue;
                }
                if (!isSubAssemblyFullyUnconnected(child)) {
                    return false;
                }
            }
            return true;
        }
    }

    // Blatt (oder rigide AssemblyLink, fuer den Solver bereits atomar) - echte Konnektivitaets-
    // pruefung.
    return !isPartConnected(obj);
}

void AssemblyObject::jointParts(const std::vector<JointRef>& joints)
{
    for (auto& jr : joints) {
        if (!jr.joint) {
            continue;
        }

        std::vector<std::shared_ptr<MbD::ASMTJoint>> mbdJoints = makeMbdJoint(jr.joint, jr.nestingPrefix);
        for (auto& mbdJoint : mbdJoints) {
            mbdAssembly->addJoint(mbdJoint);
        }
    }
}

void Assembly::AssemblyObject::create_mbdSimulationParameters(App::DocumentObject* sim)
{
    auto mbdSim = mbdAssembly->simulationParameters;
    if (!sim) {
        return;
    }
    auto valueOf = [](DocumentObject* docObj, const char* propName) {
        auto* prop = dynamic_cast<App::PropertyFloat*>(docObj->getPropertyByName(propName));
        if (!prop) {
            return 0.0;
        }
        return prop->getValue();
    };
    mbdSim->settstart(valueOf(sim, "aTimeStart"));
    mbdSim->settend(valueOf(sim, "bTimeEnd"));
    mbdSim->sethout(valueOf(sim, "cTimeStepOutput"));
    mbdSim->sethmin(1.0e-9);
    mbdSim->sethmax(1.0);
    mbdSim->seterrorTol(valueOf(sim, "fGlobalErrorTolerance"));
}

std::shared_ptr<ASMTJoint> AssemblyObject::makeMbdJointOfType(App::DocumentObject* joint, JointType type)
{
    switch (type) {
        case JointType::Fixed:
            if (bundleFixed) {
                return nullptr;
            }
            return CREATE<ASMTFixedJoint>::With();

        case JointType::Revolute:
            return CREATE<ASMTRevoluteJoint>::With();

        case JointType::Cylindrical:
            return CREATE<ASMTCylindricalJoint>::With();

        case JointType::Slider:
            return CREATE<ASMTTranslationalJoint>::With();

        case JointType::Ball:
            return CREATE<ASMTSphericalJoint>::With();

        case JointType::Distance:
            return makeMbdJointDistance(joint);

        case JointType::Parallel:
            return CREATE<ASMTParallelAxesJoint>::With();

        case JointType::Perpendicular:
            return CREATE<ASMTPerpendicularJoint>::With();

        case JointType::Angle: {
            double angle = fabs(Base::toRadians(getJointAngle(joint)));
            if (fmod(angle, 2 * std::numbers::pi) < Precision::Confusion()) {
                return CREATE<ASMTParallelAxesJoint>::With();
            }
            auto mbdJoint = CREATE<ASMTAngleJoint>::With();
            mbdJoint->theIzJz = angle;
            return mbdJoint;
        }

        case JointType::RackPinion: {
            auto mbdJoint = CREATE<ASMTRackPinionJoint>::With();
            mbdJoint->pitchRadius = getJointDistance(joint);
            return mbdJoint;
        }

        case JointType::Screw: {
            int slidingIndex = slidingPartIndex(joint);
            if (slidingIndex == 0) {  // invalid this joint needs a slider
                return nullptr;
            }

            if (slidingIndex != 1) {
                swapJCS(joint);  // make sure that sliding is first.
            }

            auto mbdJoint = CREATE<ASMTScrewJoint>::With();
            mbdJoint->pitch = getJointDistance(joint);
            return mbdJoint;
        }

        case JointType::Gears: {
            auto mbdJoint = CREATE<ASMTGearJoint>::With();
            mbdJoint->radiusI = getJointDistance(joint);
            mbdJoint->radiusJ = getJointDistance2(joint);
            return mbdJoint;
        }

        case JointType::Belt: {
            auto mbdJoint = CREATE<ASMTGearJoint>::With();
            mbdJoint->radiusI = getJointDistance(joint);
            mbdJoint->radiusJ = -getJointDistance2(joint);
            return mbdJoint;
        }

        default:
            return nullptr;
    }
}

std::shared_ptr<ASMTJoint> AssemblyObject::makeMbdJointDistance(App::DocumentObject* joint)
{
    DistanceType type = getDistanceType(joint);

    std::string elt1 = getElementFromProp(joint, "Reference1");
    std::string elt2 = getElementFromProp(joint, "Reference2");
    auto* obj1 = getLinkedObjFromRef(joint, "Reference1");
    auto* obj2 = getLinkedObjFromRef(joint, "Reference2");

    switch (type) {
        case DistanceType::PointPoint: {
            // Point to point distance, or ball joint if distance=0.
            double distance = getJointDistance(joint);
            if (distance < Precision::Confusion()) {
                return CREATE<ASMTSphericalJoint>::With();
            }
            auto mbdJoint = CREATE<ASMTSphSphJoint>::With();
            mbdJoint->distanceIJ = distance;
            return mbdJoint;
        }

        // Edge - edge cases
        case DistanceType::LineLine: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::LineCircle: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getEdgeRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CircleCircle: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getEdgeRadius(obj1, elt1)
                + getEdgeRadius(obj2, elt2);
            return mbdJoint;
        }

        // Face - Face cases
        case DistanceType::PlanePlane: {
            auto mbdJoint = CREATE<ASMTPlanarJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::PlaneCylinder: {
            auto mbdJoint = CREATE<ASMTLineInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint) + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::PlaneSphere: {
            auto mbdJoint = CREATE<ASMTPointInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint) + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::PlaneTorus: {
            auto mbdJoint = CREATE<ASMTPlanarJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::CylinderCylinder: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CylinderSphere: {
            auto mbdJoint = CREATE<ASMTCylSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::CylinderTorus: {
            auto mbdJoint = CREATE<ASMTRevCylJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::TorusTorus: {
            auto mbdJoint = CREATE<ASMTPlanarJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::TorusSphere: {
            auto mbdJoint = CREATE<ASMTCylSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        case DistanceType::SphereSphere: {
            auto mbdJoint = CREATE<ASMTSphSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1)
                + getFaceRadius(obj2, elt2);
            return mbdJoint;
        }

        // Point - Face cases
        case DistanceType::PointPlane: {
            auto mbdJoint = CREATE<ASMTPointInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::PointCylinder: {
            auto mbdJoint = CREATE<ASMTCylSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1);
            return mbdJoint;
        }

        case DistanceType::PointSphere: {
            auto mbdJoint = CREATE<ASMTSphSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint) + getFaceRadius(obj1, elt1);
            return mbdJoint;
        }

        // Edge - Face cases
        case DistanceType::LinePlane: {
            auto mbdJoint = CREATE<ASMTLineInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        // Point - Edge cases
        case DistanceType::PointLine: {
            auto mbdJoint = CREATE<ASMTCylSphJoint>::With();
            mbdJoint->distanceIJ = getJointDistance(joint);
            return mbdJoint;
        }

        case DistanceType::PointCurve: {
            // For other curves we do a point in plane-of-the-curve.
            // Maybe it would be best tangent / distance to the conic?
            // For arcs and circles we could use ASMTRevSphJoint. But is it better than
            // pointInPlane?
            auto mbdJoint = CREATE<ASMTPointInPlaneJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }

        default: {
            // by default we make a planar joint.
            auto mbdJoint = CREATE<ASMTPlanarJoint>::With();
            mbdJoint->offset = getJointDistance(joint);
            return mbdJoint;
        }
    }
}

std::vector<std::shared_ptr<MbD::ASMTJoint>> AssemblyObject::makeMbdJoint(
    App::DocumentObject* joint,
    const std::string& nestingPrefix
)
{
    if (!joint) {
        return {};
    }

    JointType jointType = getJointType(joint);

    std::shared_ptr<ASMTJoint> mbdJoint = makeMbdJointOfType(joint, jointType);
    if (!mbdJoint || !isMbDJointValid(joint, nestingPrefix)) {
        return {};
    }
    if ((jointType == JointType::Gears || jointType == JointType::Belt)
        && gearJointNeedsCarrierMarker(joint)) {
        setGearJointCarrierMarkerIfAvailable(this, joint, mbdJoint);
    }

    std::string fullMarkerNameI, fullMarkerNameJ;
    if (jointType == JointType::RackPinion) {
        getRackPinionMarkers(joint, fullMarkerNameI, fullMarkerNameJ, nestingPrefix);
    }
    else {
        fullMarkerNameI = handleOneSideOfJoint(joint, "Reference1", "Placement1", std::string(), nestingPrefix);
        fullMarkerNameJ = handleOneSideOfJoint(joint, "Reference2", "Placement2", std::string(), nestingPrefix);
    }
    if (fullMarkerNameI == "" || fullMarkerNameJ == "") {
        return {};
    }

    // FCPROJECT-PATCH (Mehrfachinstanz-Fix): jointInstanceName()
    // statt joint->getFullName() - siehe deren Definition fuer die Begruendung (Namenskollision,
    // sobald derselbe reale Joint ueber zwei AssemblyLink-Instanzen erreicht wird).
    mbdJoint->setName(jointInstanceName(joint, nestingPrefix));
    mbdJoint->setMarkerI(fullMarkerNameI);
    mbdJoint->setMarkerJ(fullMarkerNameJ);

    // Add limits if needed. We do not add if this is a simulation or their might clash.
    if (motions.empty()) {
        if (jointType == JointType::Slider || jointType == JointType::Cylindrical) {
            auto* pLenMin = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("LengthMin"));
            auto* pLenMax = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("LengthMax"));
            auto* pMinEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableLengthMin")
            );
            auto* pMaxEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableLengthMax")
            );

            if (pLenMin && pLenMax && pMinEnabled && pMaxEnabled) {  // Make sure properties do exist
                // Swap the values if necessary.
                bool minEnabled = pMinEnabled->getValue();
                bool maxEnabled = pMaxEnabled->getValue();
                double minLength = pLenMin->getValue();
                double maxLength = pLenMax->getValue();

                if ((minLength > maxLength) && minEnabled && maxEnabled) {
                    pLenMin->setValue(maxLength);
                    pLenMax->setValue(minLength);
                    minLength = maxLength;
                    maxLength = pLenMax->getValue();

                    pMinEnabled->setValue(maxEnabled);
                    pMaxEnabled->setValue(minEnabled);
                    minEnabled = maxEnabled;
                    maxEnabled = pMaxEnabled->getValue();
                }

                if (minEnabled) {
                    auto limit = ASMTTranslationLimit::With();
                    limit->setName(jointInstanceName(joint, nestingPrefix) + "-LimitLenMin");
                    limit->setMarkerI(fullMarkerNameI);
                    limit->setMarkerJ(fullMarkerNameJ);
                    limit->settype("=>");
                    limit->setlimit(std::to_string(minLength));
                    limit->settol("1.0e-9");
                    mbdAssembly->addLimit(limit);
                }

                if (maxEnabled) {
                    auto limit2 = ASMTTranslationLimit::With();
                    limit2->setName(jointInstanceName(joint, nestingPrefix) + "-LimitLenMax");
                    limit2->setMarkerI(fullMarkerNameI);
                    limit2->setMarkerJ(fullMarkerNameJ);
                    limit2->settype("=<");
                    limit2->setlimit(std::to_string(maxLength));
                    limit2->settol("1.0e-9");
                    mbdAssembly->addLimit(limit2);
                }
            }
        }
        if (jointType == JointType::Revolute || jointType == JointType::Cylindrical) {
            auto* pRotMin = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("AngleMin"));
            auto* pRotMax = dynamic_cast<App::PropertyFloat*>(joint->getPropertyByName("AngleMax"));
            auto* pMinEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableAngleMin")
            );
            auto* pMaxEnabled = dynamic_cast<App::PropertyBool*>(
                joint->getPropertyByName("EnableAngleMax")
            );

            if (pRotMin && pRotMax && pMinEnabled && pMaxEnabled) {  // Make sure properties do exist
                // Swap the values if necessary.
                bool minEnabled = pMinEnabled->getValue();
                bool maxEnabled = pMaxEnabled->getValue();
                double minAngle = pRotMin->getValue();
                double maxAngle = pRotMax->getValue();
                if ((minAngle > maxAngle) && minEnabled && maxEnabled) {
                    pRotMin->setValue(maxAngle);
                    pRotMax->setValue(minAngle);
                    minAngle = maxAngle;
                    maxAngle = pRotMax->getValue();

                    pMinEnabled->setValue(maxEnabled);
                    pMaxEnabled->setValue(minEnabled);
                    minEnabled = maxEnabled;
                    maxEnabled = pMaxEnabled->getValue();
                }

                if (minEnabled) {
                    auto limit = ASMTRotationLimit::With();
                    limit->setName(jointInstanceName(joint, nestingPrefix) + "-LimitRotMin");
                    limit->setMarkerI(fullMarkerNameI);
                    limit->setMarkerJ(fullMarkerNameJ);
                    limit->settype("=>");
                    limit->setlimit(std::to_string(minAngle) + "*pi/180.0");
                    limit->settol("1.0e-9");
                    mbdAssembly->addLimit(limit);
                }

                if (maxEnabled) {
                    auto limit2 = ASMTRotationLimit::With();
                    limit2->setName(jointInstanceName(joint, nestingPrefix) + "-LimitRotMax");
                    limit2->setMarkerI(fullMarkerNameI);
                    limit2->setMarkerJ(fullMarkerNameJ);
                    limit2->settype("=<");
                    limit2->setlimit(std::to_string(maxAngle) + "*pi/180.0");
                    limit2->settol("1.0e-9");
                    mbdAssembly->addLimit(limit2);
                }
            }
        }
    }
    std::vector<App::DocumentObject*> done;

    auto replaceInitialValue =
        [](std::string& form, App::DocumentObject* jnt, const std::string& mType) {
            if (form.find("initialValue") != std::string::npos) {
                double val = getJointCurrentValue(jnt, mType == "Angular");

                std::ostringstream out;
                out.precision(10);
                out << val;
                std::string valStr = out.str();

                size_t pos;
                while ((pos = form.find("initialValue")) != std::string::npos) {
                    form.replace(pos, 12, valStr);
                }
            }
        };

    // Add motions if needed
    for (auto* motion : motions) {
        if (std::ranges::find(done, motion) != done.end()) {
            continue;  // don't process twice (can happen in case of cylindrical)
        }

        auto* pJoint = dynamic_cast<App::PropertyXLinkSub*>(motion->getPropertyByName("Joint"));
        if (!pJoint) {
            continue;
        }
        App::DocumentObject* motionJoint = pJoint->getValue();
        if (joint != motionJoint) {
            continue;
        }

        auto* pType = dynamic_cast<App::PropertyEnumeration*>(motion->getPropertyByName("MotionType"));
        auto* pFormula = dynamic_cast<App::PropertyString*>(motion->getPropertyByName("Formula"));
        if (!pType || !pFormula) {
            continue;
        }
        std::string formula = pFormula->getValue();
        if (formula == "") {
            continue;
        }
        std::string motionType = pType->getValueAsString();

        replaceInitialValue(formula, joint, motionType);

        // check if there is a second motion as cylindrical can have both,
        // in which case the solver needs a general motion.
        for (auto* motion2 : motions) {
            pJoint = dynamic_cast<App::PropertyXLinkSub*>(motion2->getPropertyByName("Joint"));
            if (!pJoint) {
                continue;
            }
            motionJoint = pJoint->getValue();
            if (joint != motionJoint || motion2 == motion) {
                continue;
            }

            auto* pType2 = dynamic_cast<App::PropertyEnumeration*>(
                motion2->getPropertyByName("MotionType")
            );
            auto* pFormula2 = dynamic_cast<App::PropertyString*>(motion2->getPropertyByName("Formula"));
            if (!pType2 || !pFormula2) {
                continue;
            }
            std::string formula2 = pFormula2->getValue();
            if (formula2 == "") {
                continue;
            }
            std::string motionType2 = pType2->getValueAsString();
            if (motionType2 == motionType) {
                continue;  // only if both motions are different. ie one angular and one linear.
            }

            replaceInitialValue(formula2, joint, motionType2);

            auto ASMTmotion = CREATE<ASMTGeneralMotion>::With();
            ASMTmotion->setName(jointInstanceName(joint, nestingPrefix) + "-ScrewMotion");
            ASMTmotion->setMarkerI(fullMarkerNameI);
            ASMTmotion->setMarkerJ(fullMarkerNameJ);
            ASMTmotion->rIJI->atiput(2, motionType == "Angular" ? formula2 : formula);
            ASMTmotion->angIJJ->atiput(2, motionType == "Angular" ? formula : formula2);
            mbdAssembly->addMotion(ASMTmotion);

            done.push_back(motion2);
        }

        if (motionType == "Angular") {
            auto ASMTmotion = CREATE<ASMTRotationalMotion>::With();
            ASMTmotion->setName(jointInstanceName(joint, nestingPrefix) + "-AngularMotion");
            ASMTmotion->setMarkerI(fullMarkerNameI);
            ASMTmotion->setMarkerJ(fullMarkerNameJ);
            ASMTmotion->setRotationZ(formula);
            mbdAssembly->addMotion(ASMTmotion);
        }
        else if (motionType == "Linear") {
            auto ASMTmotion = CREATE<ASMTTranslationalMotion>::With();
            ASMTmotion->setName(jointInstanceName(joint, nestingPrefix) + "-LinearMotion");
            ASMTmotion->setMarkerI(fullMarkerNameI);
            ASMTmotion->setMarkerJ(fullMarkerNameJ);
            ASMTmotion->setTranslationZ(formula);
            mbdAssembly->addMotion(ASMTmotion);
        }
    }

    return {mbdJoint};
}

std::string AssemblyObject::handleOneSideOfJoint(
    App::DocumentObject* joint,
    const char* propRefName,
    const char* propPlcName,
    const std::string& markerName,
    const std::string& nestingPrefix
)
{
    // FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix): auf
    // resolvePartForMbD() umgestellt - DAS ist die Stelle, an der ein Joint tatsaechlich einen
    // MbD-Marker an einen konkreten starren Koerper haengt (siehe AssemblyObject.h fuer die
    // Herleitung). 'part' wird unten nur fuer getMbDData(part) (welcher MbD-Koerper) und fuer
    // getGlobalPlacement(part, ref) (targetObj-Parameter) verwendet - beide bleiben fuer jeden
    // nicht verschachtelten Joint (nestingPrefix "") unveraendert, weil resolvePartForMbD() dort
    // exakt dasselbe Objekt liefert wie die alte getMovingPartFromRef().
    App::DocumentObject* part = resolvePartForMbD(joint, propRefName, nestingPrefix);
    App::DocumentObject* obj = getObjFromJointRef(joint, propRefName);

    if (!part || !obj) {
        Base::Console()
            .warning("The property %s of Joint %s is bad.\n", propRefName, joint->getFullName());
        return "";
    }

    // alreadyResolved=true: 'part' kommt direkt aus resolvePartForMbD() (s.o.), ist also bereits
    // vollstaendig aufgeloest (siehe Bug-C-Kommentar an getMbDData()'s Deklaration).
    MbDPartData data = getMbDData(part, /*alreadyResolved=*/true);
    std::shared_ptr<ASMTPart> mbdPart = data.part;
    Base::Placement plc = getPlacementFromProp(joint, propPlcName);
    // Now we have plc which is the JCS placement, but its relative to the Object, not to the
    // containing Part.
    auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName(propRefName));
    if (!ref) {
        return "";
    }

    // This plc adjustment should be necessary only if obj != part. But for some objects like
    // draft links, we can have obj == part and still need to get global placement to adjust
    // by the element placement.
    Base::Placement obj_global_plc = getGlobalPlacement(nullptr, ref);
    plc = obj_global_plc * plc;
    // Note part is supposed to be root of ref, so we could use part.Placement directly.
    Base::Placement part_global_plc = getGlobalPlacement(part, ref);
    plc = part_global_plc.inverse() * plc;

    // check if we need to add an offset in case of bundled parts.
    if (!data.offsetPlc.isIdentity()) {
        plc = data.offsetPlc * plc;
    }

    std::string markerNameCopy = markerName.empty() ? jointInstanceName(joint, nestingPrefix) : markerName;
    auto mbdMarker = makeMbdMarker(markerNameCopy, plc);
    mbdPart->addMarker(mbdMarker);

    return "/OndselAssembly/" + mbdPart->name + "/" + markerNameCopy;
}

void AssemblyObject::getRackPinionMarkers(
    App::DocumentObject* joint,
    std::string& markerNameI,
    std::string& markerNameJ,
    const std::string& nestingPrefix
)
{
    // ASMT rack pinion joint must get the rack as I and pinion as J.
    // - rack marker has to have Z axis parallel to pinion Z axis.
    // - rack marker has to have X axis parallel to the sliding axis.
    // The user will have selected the sliding marker so we need to transform it.
    // And we need to detect which marker is the rack.

    int slidingIndex = slidingPartIndex(joint);
    if (slidingIndex == 0) {
        return;
    }

    if (slidingIndex != 1) {
        swapJCS(joint);  // make sure that rack is first.
    }

    // FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix): part1
    // (das einzige hier fuer getMbDData()/den MbD-Koerper verwendete Ergebnis, s.u.) auf
    // resolvePartForMbD() umgestellt - siehe handleOneSideOfJoint() fuer die Begruendung. obj1/ref1
    // bleiben bewusst bei der alten, joint-lokalen Aufloesung (nur fuer die Rack/Pinion-spezifische
    // Placement-Anpassung unten gebraucht).
    App::DocumentObject* part1 = resolvePartForMbD(joint, "Reference1", nestingPrefix);
    App::DocumentObject* obj1 = getObjFromJointRef(joint, "Reference1");
    Base::Placement plc1 = getPlacementFromProp(joint, "Placement1");

    App::DocumentObject* obj2 = getObjFromJointRef(joint, "Reference2");
    Base::Placement plc2 = getPlacementFromProp(joint, "Placement2");

    if (!part1 || !obj1) {
        Base::Console().warning("Reference1 of Joint %s is bad.\n", joint->getFullName());
        return;
    }

    // For the pinion nothing special needed :
    markerNameJ = handleOneSideOfJoint(joint, "Reference2", "Placement2", std::string(), nestingPrefix);

    // For the rack we need to change the placement :
    // make the pinion plc relative to the rack placement.
    auto* ref1 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
    auto* ref2 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
    if (!ref1 || !ref2) {
        return;
    }
    Base::Placement pinion_global_plc = getGlobalPlacement(obj2, ref2);
    plc2 = pinion_global_plc * plc2;
    Base::Placement rack_global_plc = getGlobalPlacement(obj1, ref1);
    plc2 = rack_global_plc.inverse() * plc2;

    // The rot of the rack placement should be the same as the pinion, but with X axis along the
    // slider axis.
    Base::Rotation rot = plc2.getRotation();
    // the yaw of rot has to be the same as plc1
    Base::Vector3d currentZAxis = rot.multVec(Base::Vector3d(0, 0, 1));
    Base::Vector3d currentXAxis = rot.multVec(Base::Vector3d(1, 0, 0));
    Base::Vector3d targetXAxis = plc1.getRotation().multVec(Base::Vector3d(0, 0, 1));

    // Calculate the angle between the current X axis and the target X axis
    double yawAdjustment = currentXAxis.GetAngle(targetXAxis);

    // Determine the direction of the yaw adjustment using cross product
    Base::Vector3d crossProd = currentXAxis.Cross(targetXAxis);
    if (currentZAxis * crossProd < 0) {  // If cross product is in opposite direction to Z axis
        yawAdjustment = -yawAdjustment;
    }

    // Create a yaw rotation around the Z axis
    Base::Rotation yawRotation(currentZAxis, yawAdjustment);

    // Combine the initial rotation with the yaw adjustment
    Base::Rotation adjustedRotation = rot * yawRotation;
    plc1.setRotation(adjustedRotation);

    // Then end of processing similar to handleOneSideOfJoint :
    // alreadyResolved=true: part1 kommt aus resolvePartForMbD() (s.o.), siehe Bug-C-Kommentar an
    // getMbDData()'s Deklaration.
    MbDPartData data1 = getMbDData(part1, /*alreadyResolved=*/true);
    std::shared_ptr<ASMTPart> mbdPart = data1.part;
    if (obj1->getNameInDocument() != part1->getNameInDocument()) {
        plc1 = rack_global_plc * plc1;

        Base::Placement part_global_plc = getGlobalPlacement(part1, ref1);
        plc1 = part_global_plc.inverse() * plc1;
    }
    // check if we need to add an offset in case of bundled parts.
    if (!data1.offsetPlc.isIdentity()) {
        plc1 = data1.offsetPlc * plc1;
    }

    std::string markerName = jointInstanceName(joint, nestingPrefix);
    auto mbdMarker = makeMbdMarker(markerName, plc1);
    mbdPart->addMarker(mbdMarker);

    markerNameI = "/OndselAssembly/" + mbdPart->name + "/" + markerName;
}

int AssemblyObject::slidingPartIndex(App::DocumentObject* joint)
{
    App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
    App::DocumentObject* obj1 = getObjFromJointRef(joint, "Reference1");
    boost::ignore_unused(obj1);
    Base::Placement plc1 = getPlacementFromProp(joint, "Placement1");

    App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
    App::DocumentObject* obj2 = getObjFromJointRef(joint, "Reference2");
    boost::ignore_unused(obj2);
    Base::Placement plc2 = getPlacementFromProp(joint, "Placement2");

    int slidingFound = 0;
    for (auto& jr : getJoints()) {
        auto* jt = jr.joint;
        if (getJointType(jt) == JointType::Slider) {
            App::DocumentObject* jpart1 = getMovingPartFromRef(jt, "Reference1");
            App::DocumentObject* jpart2 = getMovingPartFromRef(jt, "Reference2");
            int found = 0;
            Base::Placement plcjt, plci;
            if (jpart1 == part1 || jpart1 == part2) {
                found = (jpart1 == part1) ? 1 : 2;
                plci = (jpart1 == part1) ? plc1 : plc2;
                plcjt = getPlacementFromProp(jt, "Placement1");
            }
            else if (jpart2 == part1 || jpart2 == part2) {
                found = (jpart2 == part1) ? 1 : 2;
                plci = (jpart2 == part1) ? plc1 : plc2;
                plcjt = getPlacementFromProp(jt, "Placement2");
            }

            if (found != 0) {
                // check the placements plcjt and (jcs1 or jcs2 depending on found value) Z axis are
                // colinear ie if their pitch and roll are the same.
                double y1, p1, r1, y2, p2, r2;
                plcjt.getRotation().getYawPitchRoll(y1, p1, r1);
                plci.getRotation().getYawPitchRoll(y2, p2, r2);
                if (fabs(p1 - p2) < Precision::Confusion() && fabs(r1 - r2) < Precision::Confusion()) {
                    slidingFound = found;
                }
            }
        }
    }
    return slidingFound;
}

namespace
{
// FCPROJECT-PATCH (Befund 3, Teilschritt 2e): Top-Down-Suche nach 'target' innerhalb von
// 'objects' (und rekursiv in AssemblyLink::Group / App::DocumentObjectGroup::Group), sammelt
// dabei alle durchquerten AssemblyLink-Container in 'outPath'. Bewusst ueber die
// GROUP-Mitgliedschaft gesucht (eindeutig), NICHT ueber einen InList-Aufstieg von 'target' aus -
// InList enthaelt JEDES Objekt, das 'target' per IRGENDEINER Property referenziert (z.B. auch ein
// Joint via Reference1/Reference2), und dessen erster Eintrag ist keineswegs garantiert der
// semantische Group-Elternknoten. Ein frueherer Versuch mit InList-Aufstieg griff dadurch
// tatsaechlich einen referenzierenden Joint statt des Group-Containers und loeste komplett falsch
// auf (live per FCPROJECT-DEBUG-Logging beobachtet: 'GrandTop#BoxC' -> 'Top#Joint').
//
// FCPROJECT-PATCH (Mehrfachinstanz-Fix, siehe docs/ARCHITECTURE.md Abschnitt 4/5): outPath sammelt
// seit diesem Fix die durchquerten AssemblyLink-ZEIGER selbst statt ihrer Namen. Ein Name allein
// ist keine stabile Identitaet: fuegt man dieselbe verlinkte Baugruppe ein zweites Mal ein, haengt
// FreeCAD dem zweiten Satz Spiegelobjekte automatisch "001" an - der ursprachige, namensbasierte
// canonicalizeForMbD() suchte diesen (im inneren Dokument nie existierenden) Namen dann erfolglos
// weiter unten und fiel faelschlich auf "Spiegel bleibt Spiegel" zurueck. Mit echten Zeigern aus
// der strukturellen Group-Traversierung entfaellt diese Fehlerquelle von vornherein.
bool findLocalGroupPath(
    const std::vector<App::DocumentObject*>& objects,
    App::DocumentObject* target,
    std::vector<Assembly::AssemblyLink*>& outPath
)
{
    for (auto* candidate : objects) {
        if (!candidate) {
            continue;
        }
        if (candidate == target) {
            return true;
        }
        if (auto* asmLink = freecad_cast<Assembly::AssemblyLink*>(candidate)) {
            outPath.push_back(asmLink);
            if (findLocalGroupPath(asmLink->Group.getValues(), target, outPath)) {
                return true;
            }
            outPath.pop_back();
            continue;
        }
        if (auto* group = freecad_cast<App::DocumentObjectGroup*>(candidate)) {
            if (findLocalGroupPath(group->Group.getValues(), target, outPath)) {
                return true;
            }
        }
    }
    return false;
}
}  // namespace

// FCPROJECT-PATCH (Befund 3, Teilschritt 2e "adressieren statt kopieren", solver-root-cause-fix):
// siehe ausfuehrliche Erklaerung am Deklarationsort in AssemblyObject.h.
//
// FCPROJECT-PATCH (Mehrfachinstanz-Fix, siehe docs/ARCHITECTURE.md Abschnitt 4/5): der bisherige
// Mechanismus loeste jede Verschachtelungsebene per NAME auf (dieselbe Kette von
// getDocument()->getObject(name), nur an unterschiedlichen Dokumenten entlanggehangelt) - das
// funktionierte nur, weil eine lokale Spiegelkopie beim erstmaligen Anlegen denselben Namen wie
// ihr echtes Quellobjekt bekommt (AssemblyLink::synchronizeComponents() fragt bewusst denselben
// Wunschnamen an). Wird dieselbe verlinkte Baugruppe ein zweites Mal eingefuegt, vergibt FreeCAD
// dem zweiten Spiegelsatz automatisch einen Namenszusatz ("BoxB" -> "BoxB001") - im inneren,
// verlinkten Dokument existiert dieser Zusatz-Name nie, die Namenssuche schlaegt fehl, und die
// Funktion faellt faelschlich auf "Spiegel bleibt Spiegel" zurueck statt das echte Objekt zu
// liefern. Fix: AssemblyLink::objLinkMap/getSourceForMirror() (identitaetsbasiert, pro
// AssemblyLink-Instanz eigenstaendig von synchronizeComponents() gepflegt) ersetzt jede
// Namenssuche durch eine Zeiger-Aufloesung - immun gegen Namenskollisionen, weil sie nie einen
// String, sondern immer das tatsaechlich gefundene Zeiger-Objekt weiterreicht.
App::DocumentObject* AssemblyObject::canonicalizeForMbD(App::DocumentObject* obj)
{
    if (!obj) {
        return obj;
    }

    // Schritt 1: den lokalen Pfad von 'this' (Kette der durchquerten AssemblyLink-Container,
    // OHNE 'obj' selbst) per eindeutiger Top-Down-Gruppensuche einsammeln. Wird 'obj' dabei gar
    // nicht gefunden, liegt es NICHT im lokalen Baum dieser AssemblyObject-Instanz (z.B. schon
    // ein von resolveJointReference() geliefertes echtes, fremddokument-Objekt, oder ein
    // root-level Datum wie Origin/LCS) - dann ist 'obj' bereits kanonisch und wird unveraendert
    // zurueckgegeben.
    std::vector<Assembly::AssemblyLink*> path;
    if (!findLocalGroupPath(Group.getValues(), obj, path)) {
        // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix,
        // 2026-09-04, GrandTop-Zwischenebenen-Luecke): 'obj' liegt nicht im EIGENEN lokalen Baum
        // dieser Instanz - bisher wurde daraus geschlossen, 'obj' sei bereits kanonisch. Das
        // stimmt aber NICHT, wenn 'obj' die lokale Spiegel-Kopie einer VERSCHACHTELTEN
        // Unterbaugruppe ist (z.B. 'Top#BoxA', aufgerufen von GrandTop aus): dieses Objekt lebt
        // im DOKUMENT der Unterbaugruppe, GrandTop sieht in seinem EIGENEN Group nur deren
        // AssemblyLink-Container ('Assembly001'), niemals 'Top#BoxA' selbst - findLocalGroupPath()
        // findet es folgerichtig nie. Ohne diesen Fallback blieb GrandTops solve() bei einem
        // NICHT verschachtelten Joint, dessen Referenz zufaellig auf eine solche fremde lokale
        // Spiegel-Kopie zeigt (kein 'nestingPrefix', siehe resolvePartForMbD()), auf DIESER
        // Zwischen-Kopie stehen, anstatt bis zum ECHTEN, tiefsten Objekt (hier: 'Sub#BoxA')
        // durchzuloesen - live beobachtet: die lokale Kopie 'Top#BoxA' wurde korrekt bewegt,
        // das ECHTE 'Sub#BoxA' blieb unveraendert am Ursprung stehen, im 3D-View ueberlappten sich
        // dadurch mehrere Boxen statt sauber auf der Schubgelenk-Achse zu liegen. Fix: an JEDE
        // eigene (nicht rigide) Unterbaugruppe delegieren, deren EIGENES Dokument zu 'obj' passt -
        // canonicalizeForMbD() ist rekursiv, loest dadurch automatisch beliebig viele
        // Verschachtelungsebenen bis zum tatsaechlich tiefsten echten Objekt auf.
        for (auto* asmLink : getSubAssemblies()) {
            if (!asmLink || asmLink->isRigid()) {
                continue;
            }
            AssemblyObject* nested = asmLink->getLinkedAssembly();
            if (!nested || nested == this || nested->getDocument() != obj->getDocument()) {
                continue;
            }
            return nested->canonicalizeForMbD(obj);
        }
        return obj;
    }

    // Schritt 2: jede Verschachtelungsebene identitaetsbasiert aufloesen. Ebene 0 ist ein
    // direktes, eindeutig gefundenes Kind von 'this->Group' - keine Uebersetzung noetig. Ab Ebene
    // 1 lebt path[i] als Spiegel INNERHALB von path[i-1]s eigener Group; path[i-1]s eigene
    // objLinkMap (von path[i-1]s eigenem synchronizeComponents() gepflegt) uebersetzt diesen
    // Spiegel identitaetsbasiert auf sein echtes Quellobjekt - das ist exakt dieselbe
    // Rueckuebersetzung, die vorher (fehleranfaellig) ueber einen Namensvergleich lief. Jede Ebene
    // muss zudem eine FLEXIBLE (nicht rigide) AssemblyLink sein, sonst ist die entsprechende
    // Ebene bereits die atomare Einheit fuer den Solver und wird unveraendert zurueckgegeben
    // (Rigid-Sub-Baugruppe verhaelt sich wie ein einziges starres Teil, siehe getJoints()'
    // subJoints-Rekursion).
    // FCPROJECT-PATCH (Bug C, "Instanz-Identitaet innerhalb einer duplizierten flexiblen
    // Unterbaugruppe", 2026-09-16/17): Duplikation kann auf JEDER Ebene dieser Kette auftreten,
    // nicht nur auf der aeussersten - z.B. wenn eine bereits mehrfach eingefuegte Unterbaugruppe
    // (BG25 zweimal in BG22) selbst wieder eine im verlinkten Dokument bereits duplizierte
    // Unterbaugruppe spiegelt (BG25 hat selbst schon zwei eigene Halterbaugruppe-Instanzen) -
    // live am echten Projekt gefunden: ein Joint auf ein Bauteil INNERHALB einer solchen
    // zweifach verschachtelten Duplikation kollabierte weiterhin auf das eine geteilte, echte
    // Objekt, weil die fruehere Pruefung nur path.back() gegen 'this' pruefte (was nur bei
    // Verschachtelungstiefe 1 zufaellig richtig war). Jede Ebene wird deshalb jetzt EINZELN,
    // im jeweils richtigen Kontext (this fuer Ebene 0, sonst path[i-1]s eigene Group) geprueft -
    // siehe docs/ARCHITECTURE.md §4.1 "Bug C" fuer die volle Herleitung.
    auto siblingCandidatesFor = [this](Assembly::AssemblyLink* parent) {
        if (parent) {
            return parent->Group.getValues();
        }
        auto subs = getSubAssemblies();
        return std::vector<App::DocumentObject*>(subs.begin(), subs.end());
    };

    Assembly::AssemblyLink* previousLink = nullptr;
    bool lastContainerHasSiblings = false;
    for (auto* mirrorLink : path) {
        lastContainerHasSiblings = hasSiblingInstances(siblingCandidatesFor(previousLink), mirrorLink);
        if (lastContainerHasSiblings) {
            return obj;
        }

        App::DocumentObject* levelReal = mirrorLink;
        if (previousLink) {
            levelReal = previousLink->getSourceForMirror(mirrorLink);
            if (!levelReal) {
                Base::Console().warning(
                    "Assembly: canonicalizeForMbD() - kein Quellobjekt fuer Spiegel '%s' in "
                    "objLinkMap von '%s' gefunden.\n",
                    mirrorLink->getNameInDocument(),
                    previousLink->getNameInDocument()
                );
                return obj;
            }
        }
        auto* realAsmLink = freecad_cast<Assembly::AssemblyLink*>(levelReal);
        if (!realAsmLink || realAsmLink->isRigid()) {
            return levelReal;
        }
        previousLink = realAsmLink;
    }

    // 'obj' liegt direkt in 'this->Group' (kein AssemblyLink-Container dazwischen) - bereits
    // kanonisch, keine Uebersetzung noetig (entspricht dem alten Verhalten: eine Namenssuche nach
    // obj->getNameInDocument() im eigenen Dokument fand trivial obj selbst wieder).
    if (path.empty()) {
        return obj;
    }

    // FCPROJECT-PATCH (2026-09-19, "Fuehrung bleibt haengen" - siehe Projekt-Memory
    // requirement-flexible-subassembly-any-anchor-point): Blatt: 'obj' lebt direkt in
    // path.back()s eigener Group. Die alte Fassung pruefte hier IMMER, ob 'obj' SELBST (falls
    // eine AssemblyLink) innerhalb path.back()s Group ein Geschwister hat, das auf dieselbe
    // verlinkte Datei zeigt - UNABHAENGIG davon, ob path.back() (der umschliessende Container,
    // z.B. eine Fuehrungsbaugruppe) selbst dupliziert ist. Live am echten Projekt widerlegt: zwei
    // BEWUSST unterschiedliche, einzeln benannte Bauteile (z.B. eine feste und eine gleitende
    // Halterung), die zufaellig dieselbe Konstruktionsdatei verlinken, sind KEINE "Instanz A/B
    // derselben Einfuegung" - Bug Cs "nicht kollabieren, eigene Identitaet behalten"-Regel gehoert
    // nur zum Fall "der umschliessende CONTAINER selbst wurde mehrfach eingefuegt" (siehe
    // hasSiblingInstances()-Aufruf oben in der Schleife). Ohne diese Einschraenkung kollabierte
    // ein extern (per kurzem Pfad, ueber einen frischen Bug-C-Spiegel) referenziertes Bauteil
    // NICHT auf dieselbe Identitaet wie ein intern (per subJoints-Rekursion durch die Vorlage)
    // referenziertes Bauteil - zwei verschiedene Zeiger fuer dasselbe Bauteil, wodurch
    // getConnectedParts() den internen Joint faelschlich als "nicht erreichbar" verwarf. Fix: die
    // Geschwister-Pruefung fuer 'obj' selbst nur noch anwenden, wenn AUCH path.back() (der
    // Container) selbst als dupliziert erkannt wurde (lastContainerHasSiblings, oben in der
    // Schleife berechnet) - sonst immer ueber getSourceForMirror() auf die echte, kanonische
    // Identitaet auflösen. Funktioniert bei beliebiger Verschachtelungstiefe, da
    // lastContainerHasSiblings sich stets auf path.back() (das zuletzt durchquerte Element)
    // bezieht, unabhaengig davon wie lang path ist.
    if (lastContainerHasSiblings) {
        if (auto* asmLinkObj = freecad_cast<Assembly::AssemblyLink*>(obj)) {
            if (hasSiblingInstances(path.back()->Group.getValues(), asmLinkObj)) {
                return obj;
            }
        }
    }
    App::DocumentObject* resolved = path.back()->getSourceForMirror(obj);
    return resolved ? resolved : obj;
}

// FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix, siehe
// patches/assembly-architecture-overview.md, Abschnitt "Teilschritt 2 - Umsetzung"): siehe
// ausfuehrliche Erklaerung (inkl. der bewussten Abweichung beim subPath - wird derzeit nirgends
// an getMbDData()/getMbDPart() weitergereicht) am Deklarationsort in AssemblyObject.h.
App::DocumentObject* AssemblyObject::resolvePartForMbD(
    App::DocumentObject* joint,
    const char* propRefName,
    const std::string& nestingPrefix
)
{
    ResolvedJointRef resolved = resolveJointReference(this, joint, propRefName, nestingPrefix);
    if (resolved.obj) {
        // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix,
        // 2026-09-03, live durch Nutzer-Maus-Drag aufgedeckt - sechste Baustelle): fuer einen
        // NICHT verschachtelten Joint (nestingPrefix leer), dessen Referenz zufaellig direkt auf
        // ein lokales Spiegel-Objekt INNERHALB einer verschachtelten flexiblen AssemblyLink zeigt
        // (z.B. Joint002 "Starrer Verbund" im Top-Level-JointGroup, Ziel "App::Link BoxA"
        // innerhalb von unterAssambly), liefert resolveJointReference() nur diesen ROHEN,
        // lokalen Pointer - OHNE nestingPrefix hat die Funktion keinen Grund, weiter durch
        // getLinkedAssembly() aufzuloesen. Subs EIGENER Slider-Joint (ueber subJoints-Rekursion,
        // nestingPrefix="unterAssambly.") landet dagegen beim ECHTEN Sub#BoxA - zwei
        // UNTERSCHIEDLICHE Pointer fuer dasselbe reale Teil. Ohne dieses canonicalizeForMbD()
        // sah removeUnconnectedJoints()/getConnectedParts() (die resolvePartForMbD() OHNE
        // Umweg ueber getMbDData() direkt fuer Erreichbarkeits-Vergleiche nutzen) die beiden
        // Pointer als unverbunden - der tatsaechlich erreichbare Sub-Joint wurde faelschlich als
        // "nicht erreichbar" entfernt (live per Log bestaetigt, nachdem eine redundante
        // Erdung in Sub entfernt wurde, die das vorher zufaellig kaschiert hatte).
        //
        // FCPROJECT-PATCH (Bug C, "Instanz-Identitaet innerhalb einer duplizierten flexiblen
        // Unterbaugruppe", 2026-09-16): 'resolvedViaInstanceMirror' bedeutet, resolveJointReference()
        // hat 'resolved.obj' bereits per objLinkMap-Vorwaertslookup durch den Spiegel DIESER
        // konkreten aeusseren Instanz ersetzt - ein canonicalizeForMbD() DARAUF wuerde das per
        // getSourceForMirror() sofort wieder auf den geteilten, instanzblinden Zeiger zurueckdrehen
        // (siehe docs/ARCHITECTURE.md §4.1 "Bug C"). Deshalb hier NICHT kanonisieren - der Spiegel
        // IST bereits die korrekte, instanzeigene Identitaet.
        if (resolved.resolvedViaInstanceMirror) {
            return resolved.obj;
        }
        return canonicalizeForMbD(resolved.obj);
    }

    // Defensiver Ruecksfall: unveraendertes Altverhalten fuer jeden Fall, den
    // resolveJointReference() (noch) nicht abdeckt.
    return canonicalizeForMbD(getMovingPartFromRef(joint, propRefName));
}

bool AssemblyObject::isMbDJointValid(App::DocumentObject* joint, const std::string& nestingPrefix)
{
    // When dragging a part, we are bundling fixed parts together.
    // This may lead to a conflicting joint that is self referencing a MbD part.
    // The solver crash when fed such a bad joint. So we make sure it does not happen.
    //
    // FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix): auf
    // resolvePartForMbD() umgestellt (statt der sub-pfad-blinden getMovingPartFromRef()). Fuer
    // einen nicht verschachtelten Joint (nestingPrefix "") liefert das exakt dasselbe Objekt wie
    // bisher; ein tief verschachtelter Joint bekommt jetzt tatsaechlich die zwei
    // UNTERSCHIEDLICHEN, korrekt aufgeloesten lokalen Spiegel-Teile statt zweimal denselben
    // kollabierten Zwischen-Wrapper.
    App::DocumentObject* part1 = resolvePartForMbD(joint, "Reference1", nestingPrefix);
    App::DocumentObject* part2 = resolvePartForMbD(joint, "Reference2", nestingPrefix);
    if (!part1 || !part2) {
        return false;
    }

    // If this joint is self-referential it must be ignored.
    // alreadyResolved=true: part1/part2 kommen aus resolvePartForMbD() (s.o.), siehe
    // Bug-C-Kommentar an getMbDData()'s Deklaration.
    if (getMbDPart(part1, /*alreadyResolved=*/true) == getMbDPart(part2, /*alreadyResolved=*/true)) {
        // FCPROJECT-PATCH (10): getJointContextName() statt getFullLabel() - Label ist fuer JEDEN
        // gleichartigen Joint identisch (z.B. "Parallel" fuer jeden Parallel-Joint), der blosse
        // Name allein (z.B. "Joint005") ist zwar eindeutig, verraet aber nicht, in welcher (ggf.
        // verschachtelten) Unterbaugruppe der Joint sitzt - jede Unterbaugruppe hat ihre eigene,
        // lokal bei 0 beginnende Joint-Nummerierung. getJointContextName() liefert deshalb den
        // vollen Pfad ueber die Eltern-Kette, z.B. "Halterbaugruppe.Joint005". Gleiches Muster
        // wie Fix 8 (getContext() in JointObject.py).
        //
        // FCPROJECT-PATCH (2026-08-28, "kleiner erster Schritt"; Warnungstext seit Teilschritt 2
        // leicht angepasst): part1==part2 heisst jetzt, dass beide Referenzen NACH
        // adressierungsbewusster Aufloesung (resolvePartForMbD(), s.o.) auf dasselbe Objekt
        // zeigen - fuer einen echten, heute schon funktionierenden Redundanz-/Konflikt-Fall ist
        // das weiterhin zuverlaessig, ein tief verschachtelter Joint kollabiert dank Teilschritt 2
        // i.d.R. NICHT mehr faelschlich hierher. Die lokalen Sub-Pfade beider Referenzen werden
        // trotzdem weiter mit ausgegeben, falls doch noch ein bisher unbekannter Kollisionsfall
        // auftritt.
        auto* prop1 = joint->getPropertyByName<App::PropertyXLinkSub>("Reference1");
        auto* prop2 = joint->getPropertyByName<App::PropertyXLinkSub>("Reference2");
        std::string sub1 = (prop1 && !prop1->getSubValues().empty()) ? prop1->getSubValues()[0] : std::string("<leer>");
        std::string sub2 = (prop2 && !prop2->getSubValues().empty()) ? prop2->getSubValues()[0] : std::string("<leer>");
        Base::Console().warning(
            "Assembly: Ignoring joint (%s) because its parts are connected by a fixed "
            "joint bundle. This joint is a conflicting or redundant constraint. "
            "[FCProject-Diagnose: Reference1 zeigt auf '%s' (Sub-Pfad '%s'), Reference2 auf "
            "'%s' (Sub-Pfad '%s') - falls die Sub-Pfade unterschiedlich sind, ist das "
            "vermutlich KEIN echter Konflikt, sondern eine bekannte Solver-Einschraenkung bei "
            "verschachtelten Baugruppen, siehe patches/bugreport-nested-flex-joint-detach/]\n",
            getJointContextName(joint),
            part1->getNameInDocument(), sub1.c_str(),
            part2->getNameInDocument(), sub2.c_str()
        );
        return false;
    }
    return true;
}

AssemblyObject::MbDPartData AssemblyObject::getMbDData(App::DocumentObject* part, bool alreadyResolved)
{
    // FCPROJECT-PATCH (Befund 3, Teilschritt 2e): zentraler Kanonisierungs-Punkt - jeder Aufrufer
    // (Joints via resolvePartForMbD(), geerdete Teile via fixGroundedPart(), etc.) landet dadurch
    // garantiert auf demselben Pointer fuer ein und dasselbe reale Teil, egal ob er ueber die
    // lokale Spiegel-Kopie oder das echte Objekt hereinkam - siehe canonicalizeForMbD() fuer die
    // ausfuehrliche Begruendung. Ohne das legte objectPartMap (pointer-keyed) fuer beide Wege
    // getrennte MbD-Teile an, wodurch ein geerdetes Teil und der es bewegende Joint nie im selben
    // Constraint-Graphen landeten.
    //
    // FCPROJECT-PATCH (Bug C, 2026-09-16): 'alreadyResolved' (siehe Deklaration in
    // AssemblyObject.h) ueberspringt genau dieses canonicalizeForMbD() - noetig, weil es einen
    // von resolvePartForMbD() bewusst instanzspezifisch ersetzten Spiegel sonst sofort per
    // getSourceForMirror() auf den geteilten, instanzblinden Zeiger zurueckdrehen wuerde.
    if (!alreadyResolved) {
        part = canonicalizeForMbD(part);
    }

    auto it = objectPartMap.find(part);
    if (it != objectPartMap.end()) {
        // part has been associated with an ASMTPart before
        return it->second;
    }

    // Associate objects that belong to an active rigid cluster.
    if (auto* rep = getRigidRepresentative(part)) {
        Base::Placement repPlc = getPlacementFromProp(rep, "Placement");

        std::shared_ptr<ASMTPart> mbdPart;
        const auto repMapped = objectPartMap.find(rep);
        if (repMapped != objectPartMap.end()) {
            mbdPart = repMapped->second.part;
        }
        else {
            std::string repName = rep->getFullName();
            mbdPart = makeMbdPart(repName, repPlc);
            mbdAssembly->addPart(mbdPart);
            objectPartMap[rep] = {mbdPart, Base::Placement()};
        }

        if (const auto* members = getRigidMembers(rep)) {
            for (auto* member : *members) {
                if (!member || objectPartMap.find(member) != objectPartMap.end()) {
                    continue;
                }

                Base::Placement memberPlc = getPlacementFromProp(member, "Placement");
                objectPartMap[member] = {mbdPart, repPlc.inverse() * memberPlc};
            }
        }

        auto mapped = objectPartMap.find(part);
        if (mapped != objectPartMap.end()) {
            return mapped->second;
        }
    }

    // part has not been associated with an ASMTPart before
    std::string str = part->getFullName();
    Base::Placement plc = getPlacementFromProp(part, "Placement");
    std::shared_ptr<ASMTPart> mbdPart = makeMbdPart(str, plc);
    mbdAssembly->addPart(mbdPart);
    MbDPartData data = {mbdPart, Base::Placement()};
    objectPartMap[part] = data;  // Store the association
    Base::Console().log(
        "FCPROJECT-DEBUG getMbDData: NEW mbdPart '%s' for key '%s'\n",
        str.c_str(),
        part->getFullName().c_str()
    );

    // Associate other objects connected with fixed joints
    if (bundleFixed) {
        // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix,
        // 2026-09-03, live durch Nutzer-Maus-Drag aufgedeckt): part1/part2/partToAdd hier auf
        // canonicalizeForMbD() umgestellt. Ohne das schrieb dieser Block DIREKT in objectPartMap
        // (ohne ueber getMbDData() zu gehen) und benutzte dabei den ROHEN, nicht kanonisierten
        // Pointer als Schluessel - z.B. das lokale App::Link-Spiegelobjekt "BoxA" innerhalb einer
        // verschachtelten flexiblen AssemblyLink, referenziert von einem GANZ NORMALEN,
        // nicht-verschachtelten Fixed-Joint (Reference direkt auf die lokale Kopie, kein Nesting
        // am Joint selbst). Sub's ECHTER Slider-Joint (BoxA<->BoxB) landet dagegen ueber die
        // adressierungsbewusste Rekursion beim ECHTEN Sub#BoxA - zwei GETRENNTE objectPartMap-
        // Eintraege fuer dasselbe reale Teil, exakt dasselbe Muster wie der urspruengliche
        // canonicalizeForMbD()-Fix es fuer Grounding/Joint-Referenzen behoben hat. Sichtbares
        // Symptom: ein Fixed-Bundle (hier: BoxD<->BoxA) "gewinnt" den lokalen Spiegel-Pointer,
        // der ECHTE Slider-Joint bekommt einen komplett separaten, unabhaengigen MbD-Koerper -
        // BoxB folgt BoxDs Bewegung dadurch nicht.
        auto addConnectedFixedParts = [&](App::DocumentObject* currentPart, auto& self) -> void {
            std::vector<App::DocumentObject*> joints = getJointsOfPart(currentPart);
            for (auto* joint : joints) {
                JointType jointType = getJointType(joint);
                if (jointType == JointType::Fixed) {
                    App::DocumentObject* part1 = canonicalizeForMbD(getMovingPartFromRef(joint, "Reference1"));
                    App::DocumentObject* part2 = canonicalizeForMbD(getMovingPartFromRef(joint, "Reference2"));
                    if (!part1 || !part2) {
                        continue;
                    }
                    App::DocumentObject* partToAdd = currentPart == part1 ? part2 : part1;

                    if (objectPartMap.find(partToAdd) != objectPartMap.end()) {
                        // already added
                        continue;
                    }

                    Base::Placement plci = getPlacementFromProp(partToAdd, "Placement");
                    MbDPartData partData = {mbdPart, plc.inverse() * plci};
                    objectPartMap[partToAdd] = partData;  // Store the association
                    Base::Console().log(
                        "FCPROJECT-DEBUG getMbDData: BUNDLE-FIXED key '%s' -> mbdPart '%s' (via joint '%s')\n",
                        partToAdd->getFullName().c_str(),
                        mbdPart->name.c_str(),
                        joint->getFullName().c_str()
                    );

                    // Recursively call for partToAdd
                    self(partToAdd, self);
                }
            }
        };

        addConnectedFixedParts(part, addConnectedFixedParts);
    }
    return data;
}

std::shared_ptr<ASMTPart> AssemblyObject::getMbDPart(App::DocumentObject* part, bool alreadyResolved)
{
    if (!part) {
        return nullptr;
    }
    return getMbDData(part, alreadyResolved).part;
}

std::shared_ptr<ASMTPart> AssemblyObject::makeMbdPart(std::string& name, Base::Placement plc, double mass)
{
    auto mbdPart = CREATE<ASMTPart>::With();
    mbdPart->setName(name);

    auto massMarker = CREATE<ASMTPrincipalMassMarker>::With();
    massMarker->setMass(mass);
    massMarker->setDensity(1.0);
    massMarker->setMomentOfInertias(1.0, 1.0, 1.0);
    mbdPart->setPrincipalMassMarker(massMarker);

    Base::Vector3d pos = plc.getPosition();
    mbdPart->setPosition3D(pos.x, pos.y, pos.z);

    // TODO : replace with quaternion to simplify
    Base::Rotation rot = plc.getRotation();
    Base::Matrix4D mat;
    rot.getValue(mat);
    Base::Vector3d r0 = mat.getRow(0);
    Base::Vector3d r1 = mat.getRow(1);
    Base::Vector3d r2 = mat.getRow(2);
    mbdPart->setRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);

    return mbdPart;
}

std::shared_ptr<ASMTMarker> AssemblyObject::makeMbdMarker(std::string& name, Base::Placement& plc)
{
    auto mbdMarker = CREATE<ASMTMarker>::With();
    mbdMarker->setName(name);

    Base::Vector3d pos = plc.getPosition();
    mbdMarker->setPosition3D(pos.x, pos.y, pos.z);

    // TODO : replace with quaternion to simplify
    Base::Rotation rot = plc.getRotation();
    Base::Matrix4D mat;
    rot.getValue(mat);
    Base::Vector3d r0 = mat.getRow(0);
    Base::Vector3d r1 = mat.getRow(1);
    Base::Vector3d r2 = mat.getRow(2);
    mbdMarker->setRotationMatrix(r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z);

    return mbdMarker;
}

std::vector<ObjRef> AssemblyObject::getDownstreamParts(
    App::DocumentObject* part,
    App::DocumentObject* joint
)
{
    if (!part) {
        return {};
    }

    // First we deactivate the joint
    bool state = false;
    if (joint) {
        state = getJointActivated(joint);
        setJointActivated(joint, false);
    }

    // FCPROJECT-PATCH (Mehrfachinstanz-Fix): direkt der
    // JointRef-Vektor statt extractJointObjects() - siehe isPartConnected() fuer die Begruendung.
    std::vector<JointRef> joints = getJoints();

    std::vector<ObjRef> connectedParts = {{part, nullptr}};
    traverseAndMarkConnectedParts(part, connectedParts, joints);

    std::vector<ObjRef> downstreamParts;
    for (auto& parti : connectedParts) {
        if (!isPartConnected(parti.obj) && (parti.obj != part)) {
            downstreamParts.push_back(parti);
        }
    }

    if (joint) {
        setJointActivated(joint, state);
    }

    return downstreamParts;
}

App::DocumentObject* AssemblyObject::getUpstreamMovingPart(
    App::DocumentObject* part,
    App::DocumentObject*& joint,
    std::string& name,
    std::vector<App::DocumentObject*> excludeJoints
)
{
    if (!part || isPartGrounded(part)) {
        return nullptr;
    }

    excludeJoints.push_back(joint);

    joint = getJointOfPartConnectingToGround(part, name, excludeJoints);
    JointType jointType = getJointType(joint);
    if (jointType != JointType::Fixed) {
        return part;
    }

    part = getMovingPartFromRef(joint, name == "Reference1" ? "Reference2" : "Reference1");

    return getUpstreamMovingPart(part, joint, name);
}

double AssemblyObject::getObjMass(App::DocumentObject* obj)
{
    if (!obj) {
        return 0.0;
    }

    for (auto& pair : objMasses) {
        if (pair.first == obj) {
            return pair.second;
        }
    }
    return 1.0;
}

void AssemblyObject::setObjMasses(std::vector<std::pair<App::DocumentObject*, double>> objectMasses)
{
    objMasses = objectMasses;
}

std::vector<AssemblyLink*> AssemblyObject::getSubAssemblies() const
{
    std::vector<AssemblyLink*> subAssemblies = {};

    App::Document* doc = getDocument();

    std::vector<DocumentObject*> assemblies = doc->getObjectsOfType(
        Assembly::AssemblyLink::getClassTypeId()
    );
    for (auto assembly : assemblies) {
        if (hasObject(assembly)) {
            subAssemblies.push_back(freecad_cast<AssemblyLink*>(assembly));
        }
    }

    return subAssemblies;
}

// FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix, 2026-09-03):
// siehe ausfuehrliche Erklaerung am Deklarationsort in AssemblyObject.h.
bool AssemblyObject::isNestedUnderFlexibleParent() const
{
    for (auto* obj : getInList()) {
        auto* asmLink = freecad_cast<Assembly::AssemblyLink*>(obj);
        if (asmLink && !asmLink->isRigid()) {
            return true;
        }
    }
    return false;
}

void AssemblyObject::ensureIdentityPlacements()
{
    std::vector<App::DocumentObject*> group = Group.getValues();
    for (auto* obj : group) {
        // When used in assembly, link groups must have identity placements.
        if (obj->isLinkGroup()) {
            auto* link = dynamic_cast<App::Link*>(obj);
            auto* pPlc = obj->getPlacementProperty();
            if (!pPlc || !link) {
                continue;
            }

            Base::Placement plc = pPlc->getValue();
            if (plc.isIdentity()) {
                continue;
            }

            pPlc->setValue(Base::Placement());
            obj->purgeTouched();

            // To keep the LinkElement positions, we apply plc to their placements
            std::vector<App::DocumentObject*> elts = link->ElementList.getValues();
            for (auto* elt : elts) {
                pPlc = elt->getPlacementProperty();
                pPlc->setValue(plc * pPlc->getValue());
                elt->purgeTouched();
            }
        }
    }
}

void AssemblyObject::syncGroundedJoints()
{
    if (App::GetApplication().isRestoring()) {
        return;
    }

    std::vector<App::DocumentObject*> groundedJoints = getGroundedJoints();
    std::map<App::DocumentObject*, App::DocumentObject*> groundedMap;
    for (auto gJoint : groundedJoints) {
        auto propObj = dynamic_cast<App::PropertyLink*>(gJoint->getPropertyByName("ObjectToGround"));
        if (propObj && propObj->getValue()) {
            groundedMap[propObj->getValue()] = gJoint;
        }
    }

    std::vector<App::DocumentObject*> allParts = getAssemblyComponents(this);

    for (auto part : allParts) {
        if (!part) {
            continue;
        }
        auto propPlc = part->getPlacementProperty();
        if (!propPlc) {
            continue;
        }

        bool isReadOnly = propPlc->isReadOnly();
        auto it = groundedMap.find(part);
        bool hasJoint = (it != groundedMap.end());

        // Create grounding joint if placement is locked but no joint exists
        if (isReadOnly && !hasJoint) {
            Base::Console().log(
                "FCPROJECT-DEBUG: syncGroundedJoints('%s') legt NEUEN GroundedJoint an fuer "
                "part='%s' (Dokument '%s').\n",
                getFullName(),
                part->getNameInDocument(),
                part->getDocument() ? part->getDocument()->getName() : "<null>"
            );
            // Konsistenter Zustand (oder Selbstheilung greift) - ein evtl. anhaengiger
            // Loesch-Verdacht von einem frueheren Aufruf ist damit hinfaellig.
            pendingGroundedJointRemoval.erase(part);

            Base::PyGILStateLocker lock;
            try {
                // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix,
                // 2026-09-04, GrandTop-Endlosschleife): 'part' kann dank getAssemblyComponents()s
                // Rekursion in verschachtelte flexible AssemblyLinks ein ECHTES Objekt aus einem
                // ANDEREN Dokument sein (z.B. 'part' aus MinimalReproSub, waehrend 'this' zu
                // MinimalReproGrandTop gehoert) - derselbe Identitaetsraum-Fehler wie bei
                // getMovingPartFromSel()/resolveJointReference() (siehe dort). Die alte Zeile nutzte
                // IMMER getDocument() (das Dokument DIESER Assembly) fuer BEIDE Lookups (asm UND
                // part) - fuer ein cross-document 'part' liefert doc.getObject(partName) dann
                // entweder None oder (schlimmer, bei zufaelliger Namensgleichheit) ein VOELLIG
                // ANDERES Objekt. JointObject.GroundedJoint(j, None) wirft eine Exception, NACHDEM
                // jg.newObject() das kaputte 'j' bereits angelegt hat - dessen ObjectToGround bleibt
                // leer, hasJoint bleibt beim naechsten solve() weiterhin False fuer 'part', ein
                // NEUES kaputtes GroundedJoint wird angelegt - fuer immer (jede Neuanlage touched
                // das Dokument, loest sofort den naechsten Recompute/solve()-Durchlauf aus). Erklaert
                // vermutlich auch die laenger bekannte, bisher nur als "kosmetisch" eingestufte
                // GroundedJoint-Vervielfachung (mehrere GroundedJoint/GroundedJoint003/... fuer
                // dasselbe Teil, siehe project_fcproject_groundedjoint_proliferation_bug-Memory).
                // Fix: 'part' wird in SEINEM EIGENEN Dokument gesucht, nicht in dem der Assembly.
                std::string docName = getDocument()->getName();
                std::string partDocName =
                    part->getDocument() ? part->getDocument()->getName() : docName;
                std::string asmName = getNameInDocument();
                std::string partName = part->getNameInDocument();
                std::string code = "import FreeCAD\n"
                                   "try:\n"
                                   "    import JointObject\n"
                                   "    import UtilsAssembly\n"
                                   "    doc = FreeCAD.getDocument('"
                    + docName
                    + "')\n"
                      "    partDoc = FreeCAD.getDocument('"
                    + partDocName
                    + "')\n"
                      "    asm = doc.getObject('"
                    + asmName
                    + "')\n"
                      "    part = partDoc.getObject('"
                    + partName
                    + "')\n"
                      "    jg = UtilsAssembly.getJointGroup(asm)\n"
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
        // Delete grounding joint if placement lock was lifted - aber erst nach einer zweiten
        // Bestaetigung in einem SPAETEREN solve()-Aufruf (siehe Kommentar bei
        // pendingGroundedJointRemoval in AssemblyObject.h). Das faengt die Race Condition ab,
        // bei der der ganz fruehe, durch onChanged(&Group) waehrend des Dokument-Restores
        // ausgeloeste solve()-Aufruf schneller laeuft als GroundedJoint.onDocumentRestored()
        // (Python), das das ReadOnly-Flag neu setzt: bei einem einmaligen Race-Treffer ist das
        // Flag spaetestens beim naechsten solve() korrekt gesetzt, der Verdacht wird dann durch
        // den ersten if-Zweig oben (isReadOnly && !hasJoint faellt weg) wieder ausgeraeumt,
        // BEVOR es zu dieser zweiten Bestaetigung kommt. Beim echten Anwendungsfall (Nutzer hebt
        // die Sperre manuell per Rechtsklick auf) bleibt der inkonsistente Zustand dagegen ueber
        // mehrere solve()-Aufrufe hinweg bestehen - dort loescht der zweite Treffer wie bisher.
        else if (!isReadOnly && hasJoint) {
            if (pendingGroundedJointRemoval.count(part)) {
                Base::Console().message(
                    "Assembly: GroundedJoint '%s' fuer Teil '%s' entfernt (Sperre wurde "
                    "ueber mehrere Solve-Zyklen hinweg aufgehoben).\n",
                    it->second->getNameInDocument(), part->getNameInDocument()
                );
                getDocument()->removeObject(it->second->getNameInDocument());
                pendingGroundedJointRemoval.erase(part);
            }
            else {
                pendingGroundedJointRemoval.insert(part);
            }
        }
        else {
            // Konsistenter Zustand (isReadOnly && hasJoint, oder !isReadOnly && !hasJoint) -
            // ein evtl. anhaengiger Loesch-Verdacht ist hinfaellig.
            pendingGroundedJointRemoval.erase(part);
        }
    }
}

int AssemblyObject::numberOfComponents() const
{
    return getAssemblyComponents(this).size();
}

bool AssemblyObject::isEmpty() const
{
    return numberOfComponents() == 0;
}
