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
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
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

#include "AssemblyIdentityGraph.h"
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

// FCPROJECT-PATCH (2026-10-04, BG22-Haenger in der GUI, per gdb-Stichproben gefunden): der
// Baum fragt fuer JEDEN Joint das Part_Detached-Overlay ab (getOverlayIcons() ->
// isPartConnected()); isPartConnected() loest beim Durchlaufen des Joint-Graphen JEDEN Joint in
// JEDEM Schritt per resolvePartForMbD() auf - und jeder dieser Aufrufe baute bisher einen NEUEN
// IdentityGraph, dessen instanceOf()-Index jedes Mal komplett neu entstand. Bei BG22 (45
// Dokumente) waren das Minuten ohne jede Log-Ausgabe. Loesung: fuer die Dauer EINES Vorgangs einen
// gemeinsamen Graphen bereitstellen. 'fresh' erzwingt einen eigenen Graphen (solve() kann vorher in
// syncGroundedJoints() die Struktur aendern); verschachtelte Scopes ohne 'fresh' nutzen den aeusseren.
// FCPROJECT-PATCH (2026-10-10, Plan plans/2026-10-10-simulation-an-struktur-anpassen.md,
// Schritt 4): ist eine Unterbaugruppe mehrfach eingebaut, ist ihr Gelenk in der aeusseren BG je
// Instanz ein eigenes Gelenk (eigenes MbD-Gelenk, jointInstanceName()). Eine Motion legt die
// gemeinte Instanz im Unterpfad ihrer vorhandenen Property "Joint" ab (Verschachtelungspfad, z.B.
// "CNC3018_032_A_FuehrungsBaugruppe401.") - keine neue Property, Dateien bleiben kompatibel.
// Leerer Unterpfad (alte Dateien, Gelenk direkt in der BG): die Motion gilt wie bisher fuer jede
// Instanz dieses Gelenks.
bool motionMatchesJointInstance(const App::PropertyXLinkSub* pJoint, const std::string& nestingPrefix)
{
    const auto& subs = pJoint->getSubValues();
    if (subs.empty() || subs.front().empty()) {
        return true;
    }
    return subs.front() == nestingPrefix;
}

class ScopedIdentityGraph
{
public:
    ScopedIdentityGraph(Assembly::AssemblyObject* assembly, Assembly::IdentityGraph*& slot, bool fresh = false)
        : slot(slot)
        , previous(slot)
    {
        if (fresh || !slot) {
            owned = std::make_unique<Assembly::IdentityGraph>(assembly);
            slot = owned.get();
        }
    }
    ~ScopedIdentityGraph()
    {
        if (owned) {
            slot = previous;
        }
    }
    ScopedIdentityGraph(const ScopedIdentityGraph&) = delete;
    ScopedIdentityGraph& operator=(const ScopedIdentityGraph&) = delete;

private:
    Assembly::IdentityGraph*& slot;
    Assembly::IdentityGraph* previous;
    std::unique_ptr<Assembly::IdentityGraph> owned;
};

struct RotationJointSide
{
    App::DocumentObject* joint = nullptr;
    std::string nestingPrefix;
    const char* carrierRefName = nullptr;
    const char* carrierPlcName = nullptr;
};

std::string jointInstanceName(App::DocumentObject* joint, const std::string& nestingPrefix);

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

    return getJcsReferenceGlobalPlacement(ref) * App::GeoFeature::getPlacementFromProp(joint, plcName);
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

// FCPROJECT-PATCH (2026-10-27, jointtype-matrix gears_coaxial__duplicate): Zahnrad- und
// Traegerseite ueber resolvePartForMbD() mit dem jeweiligen nestingPrefix vergleichen statt ueber
// die adressierungsblinde getMovingPartFromRef(). Sonst fand die Suche bei einer doppelt
// eingebauten Unterbaugruppe jedes Drehgelenk einmal je Instanz, hielt das fuer mehrdeutig und
// setzte keinen Traeger ("Coaxial GearJoint requires a carrier marker").
bool findRotationCarrierForGearSide(
    AssemblyObject* assembly,
    App::DocumentObject* gearJoint,
    const std::string& gearPrefix,
    const char* gearRefName,
    const char* gearPlcName,
    RotationJointSide& carrierSide
)
{
    auto* gearPart = assembly->resolvePartForMbD(gearJoint, gearRefName, gearPrefix);
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
            if (assembly->resolvePartForMbD(joint, movingRefName, jr.nestingPrefix) != gearPart) {
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
            carrierSide.nestingPrefix = jr.nestingPrefix;
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
    const std::shared_ptr<ASMTJoint>& mbdJoint,
    const std::string& nestingPrefix
)
{
    auto gearJoint = std::dynamic_pointer_cast<ASMTGearJoint>(mbdJoint);
    if (!gearJoint) {
        return;
    }

    RotationJointSide side1;
    RotationJointSide side2;
    const bool hasCarrier1
        = findRotationCarrierForGearSide(assembly, joint, nestingPrefix, "Reference1", "Placement1", side1);
    const bool hasCarrier2
        = findRotationCarrierForGearSide(assembly, joint, nestingPrefix, "Reference2", "Placement2", side2);
    if (!hasCarrier1 || !hasCarrier2) {
        return;
    }

    auto* carrierPart1
        = assembly->resolvePartForMbD(side1.joint, side1.carrierRefName, side1.nestingPrefix);
    auto* carrierPart2
        = assembly->resolvePartForMbD(side2.joint, side2.carrierRefName, side2.nestingPrefix);
    if (!carrierPart1 || !carrierPart2
        || assembly->getMbDPart(carrierPart1, /*alreadyResolved=*/true)
            != assembly->getMbDPart(carrierPart2, /*alreadyResolved=*/true)) {
        return;
    }

    const std::string carrierMarkerName = jointInstanceName(joint, nestingPrefix) + "-Carrier";
    std::string fullMarkerNameK = assembly->handleOneSideOfJoint(
        side1.joint,
        side1.carrierRefName,
        side1.carrierPlcName,
        carrierMarkerName,
        side1.nestingPrefix
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
        // FCPROJECT-PATCH (2026-09-25, siehe setActiveEditContext()-Deklaration in
        // AssemblyObject.h): waehrend der Nutzer GENAU DIESE Instanz aktiv bearbeitet, wird sie
        // wie eine echte Top-Level-Instanz behandelt (eigener solve()), egal ob eine aeussere
        // Baugruppe sie ohnehin (schlecht oder gut) mitloest.
        if (isNestedUnderFlexibleParent() && !activeEditContext) {
            Base::Console().log(
                "Assembly: '{}' skipped its own solve() - nested under a flexible parent "
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
        // FCPROJECT-PATCH (2026-10-11, "Startchaos"): beim Einlesen einer Datei aendert sich Group
        // schon, waehrend die uebrigen Objekte und verlinkten Dokumente noch fehlen.
        // updateSolveStatus() loeste dann "auf Verdacht" (gdb: alle Solves beim Oeffnen von BG22
        // kamen von hier) - jede mitgeladene Unterbaugruppe einmal, meist mit "no grounded part
        // found", weil die Erdung noch nicht eingelesen war. Waehrend des Ladens deshalb nicht;
        // die gespeicherte Lage ist bereits geloest, der naechste Recompute/Drag loest regulaer.
        if (!isRestoring() && !App::GetApplication().isRestoring()) {
            updateSolveStatus();
        }
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

// FCPROJECT-PATCH (2026-09-27, live am echten BG37->BG43->BG67-Projekt gefunden, "Motor
// kollabiert"-Bug): syncGroundedJoints() verglich bisher gegen getGroundedJoints() - das durchsucht
// NUR die JointGroup DIESER Ebene, NICHT rekursiv wie getGroundedParts() (siehe dessen Kommentar
// "Nutzerkorrektur 2026-09-04"). Fuer ein Teil, das BEREITS ueber einen zwei (oder mehr) Ebenen
// tiefer liegenden, eigenen GroundedJoint fixiert ist (z.B. ein Motor-Getriebeteil in einer
// verschachtelten flexiblen Unterbaugruppe), meldete syncGroundedJoints() faelschlich "isReadOnly,
// aber kein Joint gefunden" und legte HIER, auf der AEUSSEREN Ebene, einen ZWEITEN, redundanten
// GroundedJoint fuer dasselbe physische Teil an. Zwei unabhaengige Erdungen fuer dasselbe Teil
// zwingen den Solver, andere (echte, noetige) Joints als redundant zu verwerfen, um nicht zu
// crashen - beobachtet als kompletter Verlust ALLER internen Motor-Gelenke (elf Getriebeteile
// fielen komplett auseinander, jedes landete auf (0,0,0)). Fix: rekursiv (identisches
// Abstiegsmuster wie getGroundedParts(): getSubAssemblies(), rigide ueberspringen,
// getLinkedAssembly() rekursiv) sammeln, WELCHE Teile ANYWHERE (nicht nur lokal) bereits einen
// GroundedJoint haben - nur das entscheidet, ob eine NEUE Erdung noetig ist. Fuer die bestehende
// "Loesche verwaiste lokale Erdung"-Logik bleibt weiterhin ausschliesslich die LOKALE Map
// massgeblich (nie ein Objekt in einem FREMDEN Dokument loeschen).
void collectGroundedPartsRecursive(
    AssemblyObject* assembly,
    std::unordered_set<App::DocumentObject*>& groundedParts
)
{
    if (!assembly) {
        return;
    }
    for (auto* gJoint : assembly->getGroundedJoints()) {
        auto* propObj = dynamic_cast<App::PropertyLink*>(gJoint->getPropertyByName("ObjectToGround"));
        if (propObj && propObj->getValue()) {
            groundedParts.insert(propObj->getValue());
        }
    }
    for (auto* subAssembly : assembly->getSubAssemblies()) {
        if (!subAssembly || subAssembly->isRigid()) {
            continue;
        }
        AssemblyObject* nested = subAssembly->getLinkedAssembly();
        if (!nested || nested == assembly) {
            continue;
        }
        collectGroundedPartsRecursive(nested, groundedParts);
    }
}

}  // namespace

int AssemblyObject::solve(bool enableRedo)
{
    // updateSolveStatus() solves on demand; suppress that while a solve is running.
    Base::StateLocker lock(solveInProgress);

    // FCPROJECT-PATCH (11): mehr Rueckmeldung ueber den Solver-Ablauf. Bisher blieb sowohl ein
    // erfolgreicher Solve als auch ein mangels geerdetem Teil komplett uebersprungener Solve
    // (groundedObjs.empty() -> return -6) OHNE JEDE Konsolen-Ausgabe - nur echte Exceptions
    // wurden gemeldet (siehe catch-Bloecke unten). Im PDM-Alltag mit vielen verschachtelten
    // Baugruppen ist dadurch oft unklar, ob/wann der Solver ueberhaupt gelaufen ist und was er
    // dabei festgestellt hat (z.B. redundante Joints, siehe Fix 10) - die drei Meldungen unten
    // decken die Fragen "1. gestartet? 2. berechnet? 3. mit welchem Ergebnis?" ab.
    Base::Console().log("Assembly: Solving '{}'...\n", getFullName());

    ensureIdentityPlacements();

    syncGroundedJoints();

    // Ein gemeinsamer IdentityGraph fuer den Rest dieses Solves (siehe ScopedIdentityGraph); frisch,
    // weil syncGroundedJoints() eben einen GroundedJoint angelegt haben kann.
    ScopedIdentityGraph graphScope(this, scopedGraph, /*fresh=*/true);

    mbdAssembly = makeMbdAssembly();
    objectPartMap.clear();
    rebuildRigidClusters();
    syncActiveRigidGroupPlacements();
    motions.clear();

    auto groundedObjs = fixGroundedParts();
    if (groundedObjs.empty()) {
        // If no part fixed we can't solve.
        Base::Console().warning(
            "Assembly: Solve of '{}' skipped - no grounded part found.\n",
            getFullName()
        );
        return -6;
    }

    // FCPROJECT-PATCH (2026-10-03, "Erdung ist Kontext-Eigenschaft"): sichtbar machen, welche
    // verschachtelten Erdungen im Kontext dieser Baugruppe ignoriert werden (ersetzt die fruehere
    // Warnung "schaltet redundante Erdung ab"). Bewusst hier und nicht in getGroundedParts() -
    // das liegt im Drag-Heisspfad. Nichts wird abgeschaltet, die Unterbaugruppe behaelt ihre
    // Erdung fuer sich allein.
    {
        std::vector<std::pair<App::DocumentObject*, App::DocumentObject*>> ignoredNestedRoots;
        getGroundedParts(&ignoredNestedRoots);
        for (const auto& [ignored, winner] : ignoredNestedRoots) {
            Base::Console().log(
                "Assembly: '{}' ignoriert verschachtelte Erdung von '{}' - im Kontext dieser "
                "Baugruppe ist '{}' massgeblich (die Unterbaugruppe selbst bleibt geerdet).\n",
                getFullName(),
                ignored ? ignored->getFullName() : "?",
                winner ? winner->getFullName() : "?"
            );
        }
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

    setJointPriorities(jointRefs);
    redundancyCacheApplied = applyRedundancyCache(currentRedundancyCache());
    try {
        mbdAssembly->runPreDrag();
        // FCPROJECT-PATCH (2026-10-11, live an BG22 gefunden: BG75 per Zylinder-Fuehrung
        // angebunden, M5-Schrauben mit Scheibe/Nutenstein rutschen 67-172mm aus ihrem Langloch):
        // runPreDrag() beachtet Gelenkgrenzen NICHT, OndselSolver prueft sie nur im
        // Drag-Schritt (PosICDragLimitNewtonRaphson). Wird eine Baugruppe ueber neue Joints
        // verschoben, bleiben Teile an einem Slider einfach in der Welt stehen und landen weit
        // jenseits ihrer Grenze. Deshalb hier ein Grenz-Durchgang ohne gezogene Teile: verletzte
        // Grenzen werden aktiviert und nachgeloest. Lassen sie sich nicht erfuellen, setzt
        // runDragStep() selbst auf das Ergebnis von runPreDrag() zurueck. Upstream identisch
        // ohne diesen Durchgang.
        if (!mbdAssembly->limits->empty()) {
            mbdAssembly->runDragStep(std::make_shared<std::vector<std::shared_ptr<ASMTPart>>>());
        }
        reportUnsatisfiedLimits();
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
    Base::Console().log(
        "Assembly: '{}' computed ({} joint(s), {} grounded part(s)):\n",
        getFullName(),
        joints.size(),
        groundedObjs.size()
    );
    for (auto* obj : groundedObjs) {
        if (!obj) {
            continue;
        }
        Base::Console().log("Assembly:   grounded part: {}\n", getJointContextName(obj));
    }

    for (const auto& [rep, members] : rigidMembersByRep) {
        Base::Console().log(
            "Assembly: rigid group '{}' ({} Teil(e)): {}.\n",
            getJointContextName(rep),
            members.size(),
            joinContextNames(members)
        );
    }

    // FCPROJECT-PATCH (2026-10-03, "BG43-Nachdrehen", zum Ausprobieren): Ausgangswerte merken,
    // um nach dem Zurueckschreiben zu sehen, ob dieser Durchlauf noch etwas bewegt hat (siehe
    // Wiederholung weiter unten).
    std::vector<std::pair<App::DocumentObject*, Base::Placement>> placementsBefore;
    for (const auto& [obj, data] : objectPartMap) {
        if (obj) {
            placementsBefore.emplace_back(obj, getPlacementFromProp(obj, "Placement"));
        }
    }

    setNewPlacements();
    updateRigidPlacementCache();

    redrawJointPlacements(joints);

    updateSolveStatus();

    double maxMoved = 0.0;
    for (const auto& [obj, before] : placementsBefore) {
        Base::Placement after = getPlacementFromProp(obj, "Placement");
        double rotDiff = 0.0;
        Base::Vector3d axis;
        (before.getRotation().inverse() * after.getRotation()).getRawValue(axis, rotDiff);
        maxMoved = std::max({maxMoved, (after.getPosition() - before.getPosition()).Length(),
                             std::abs(rotDiff)});
    }

    // FCPROJECT-PATCH (2026-10-14, Regel C7): Warnung nur noch bei einem Widerspruch ueber der
    // Toleranz; stimmige Redundanz wird geschluckt (nur Protokoll).
    if (lastHasRedundancies) {
        std::string names;
        for (const auto& n : lastConflictingJoints) {
            if (!names.empty()) {
                names += ", ";
            }
            names += n;
        }
        Base::Console().warning(
            "Assembly: Solve of '{}' finished with {} conflicting joint(s) (ueberbestimmt, "
            "ausserhalb der Toleranz): {}.\n",
            getFullName(),
            lastConflictingJoints.size(),
            names
        );
        for (const auto& group : lastRedundancyGroups) {
            Base::Console().warning("Assembly:   ueberbestimmt: {}\n", group);
        }
    }
    else if (!lastRedundantJoints.empty()) {
        Base::Console().log(
            "Assembly: Solve of '{}' finished successfully; {} redundante Bedingung(en) in {} "
            "Gelenk(en) innerhalb der Toleranz geschluckt.\n",
            getFullName(),
            lastRedundancyStates.size(),
            lastRedundantJoints.size()
        );
    }
    else {
        Base::Console().log("Assembly: Solve of '{}' finished successfully.\n", getFullName());
    }

    // FCPROJECT-PATCH (2026-10-14, Regel C7): ein Widerspruch mit vorab herausgenommenen
    // Bedingungen kann daher ruehren, dass eine gemerkte Bedingung gar nicht mehr redundant ist
    // (z.B. stellungsabhaengig). Dann einmal ohne Gedaechtnis frisch loesen.
    if (redundancyCacheApplied && newRedundancyConflict) {
        Base::Console().log(
            "Assembly: '{}' - Widerspruch mit gemerkten Redundanzen, loese ohne Gedaechtnis neu.\n",
            getFullName()
        );
        currentRedundancyCache() = RedundancyCache();
        return solve(false);
    }

    // FCPROJECT-PATCH (2026-10-03, "BG43-Nachdrehen", zum Ausprobieren): bei grossem Startfehler
    // (z.B. Unterbaugruppe separat gespeichert, Rotor 40+ Grad gegenueber der Kupplung verdreht)
    // konvergiert OndselSolver nicht in einem Durchlauf - er erklaert numerisch Constraints fuer
    // "redundant" und hoert vor dem Ziel auf, erst der naechste Solve trifft. Deshalb: solange
    // Redundanzen gemeldet werden UND dieser Durchlauf noch etwas bewegt hat, intern erneut loesen
    // (hoechstens zweimal). Echt redundante, bereits konvergierte Systeme bewegen sich nicht mehr
    // und brechen nach einem Zusatzlauf ab.
    static thread_local int redundancyRetryDepth = 0;
    constexpr int maxRedundancyRetries = 2;
    constexpr double movedTolerance = 1e-7;
    if (lastHasRedundancies && maxMoved > movedTolerance
        && redundancyRetryDepth < maxRedundancyRetries) {
        Base::Console().log(
            "Assembly: '{}' - Redundanzen gemeldet und noch Bewegung ({:.3g}), loese erneut ({}/{}).\n",
            getFullName(),
            maxMoved,
            redundancyRetryDepth + 1,
            maxRedundancyRetries
        );
        ++redundancyRetryDepth;
        int rc = solve(false);
        --redundancyRetryDepth;
        return rc;
    }

    // FCPROJECT-PATCH (2026-10-03, Plan plans/2026-10-03-solver-nur-auf-spiegeln.md,
    // Nutzerentscheidung "zeigt ja, aendert nein"): hier stand solveHiddenFlexibleUnderRigid(), das
    // flexible Unterbaugruppen hinter einer rigiden Ebene aus DIESEM Kontext heraus in ihrem eigenen
    // Dokument loeste und damit Originale veraenderte. Eine rigide Unterbaugruppe ist hier ein
    // einziger Koerper und zeigt den Stand ihres eigenen Dokuments; ihr Inneres wird nur in ihrem
    // eigenen Kontext geloest.

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

    // Solve on demand when queried before the system is solved, but not from within
    // a solve: solve() calls this, so a failed solve would recurse indefinitely.
    if (!solveInProgress && (!mbdAssembly || !mbdAssembly->mbdSystem)
        && !App::GetApplication().isRestoring()) {
        solve();
    }

    if (!mbdAssembly || !mbdAssembly->mbdSystem) {
        return;
    }

    // FCPROJECT-PATCH (2026-10-03, live an BG43 gefunden, [[todo-redundant-joint-crash-wrong-assembly-attribution]]):
    // die MbD-Joint-Namen sind jointInstanceName() = nestingPrefix + joint->getFullName() (siehe
    // jointParts()). Die bisherige Aufloesung schnitt alles bis zum letzten '#' ab und suchte den
    // Rest im Dokument DIESER Baugruppe - ein verschachtelter Joint "BG67_III.BG67#Joint" landete
    // so faelschlich bei BG43s eigenem "Joint" (falsche Meldung "Over-constrained: <BG43-Joint>",
    // falsche Auswahl). Jetzt exakte Zuordnung ueber dieselben Instanznamen wie beim Aufbau.
    std::unordered_map<std::string, App::DocumentObject*> solverNameToJoint;
    for (const auto& jr : getJoints(false, true, false)) {
        if (jr.joint) {
            solverNameToJoint[jointInstanceName(jr.joint, jr.nestingPrefix)] = jr.joint;
        }
    }
    const std::string solverPathPrefix = "/OndselAssembly/";

    // Iterate through all joints and motions in the MBD system
    mbdAssembly->mbdSystem->jointsMotionsDo([&](std::shared_ptr<MbD::Joint> jm) {
        if (!jm) {
            return;
        }
        // Base::Console().warning("jm->name {}\n", jm->name);
        bool isJointRedundant = false;

        jm->constraintsDo([&](std::shared_ptr<MbD::Constraint> con) {
            if (!con) {
                return;
            }

            std::string spec = con->constraintSpec();
            // A constraint is redundant if its spec starts with "Redundant"
            if (spec.rfind("Redundant", 0) == 0) {
                isJointRedundant = true;
                Base::Console().log(
                    "FCPROJECT-DEBUG updateSolveStatus: redundant constraint '{}' in solver joint '{}'\n",
                    spec,
                    jm->name
                );
            }
            // Base::Console().warning("    - {}\n", spec);
            // FCPROJECT-PATCH (2026-10-14, Regel C7): herausgenommene Bedingungen sind von den
            // uebrigen abhaengig und nehmen keinen Freiheitsgrad weg.
            if (!con->isRedundant()) {
                --lastDoF;
            }
        });

        std::string solverName = jm->name;
        if (solverName.rfind(solverPathPrefix, 0) == 0) {
            solverName = solverName.substr(solverPathPrefix.size());
        }
        auto mapped = solverNameToJoint.find(solverName);
        App::DocumentObject* docObj = mapped != solverNameToJoint.end() ? mapped->second : nullptr;

        // We only care about objects that are actual joints in the FreeCAD document.
        // This effectively filters out the grounding joints, which are named after parts.
        if (!docObj || !docObj->getPropertyByName("Reference1")) {
            return;
        }

        if (isJointRedundant) {
            // Check if this joint is already in the list to avoid duplicates. Joints aus einem
            // verschachtelten Dokument als "Dokument#Objekt" (eindeutig, von der GUI aufloesbar).
            std::string objName = docObj->getDocument() == getDocument()
                ? std::string(docObj->getNameInDocument())
                : docObj->getFullName();
            if (std::find(lastRedundantJoints.begin(), lastRedundantJoints.end(), objName)
                == lastRedundantJoints.end()) {
                lastRedundantJoints.push_back(objName);
            }
        }
    });

    // FCPROJECT-PATCH (2026-10-11, live an BG22: Parallel-Verbindung ueberbestimmte die Drehung um
    // eine Gewindestange, gemeldet wurde ein Gelenk einer anderen Fuehrungsbaugruppe): welche
    // Gleichung OndselSolver entnimmt, haengt von der Pivotreihenfolge ab. Die eigentliche
    // ueberbestimmte Gruppe liefert System::redundancyGroups (alle Gelenke, von deren Gleichungen
    // die entnommene linear abhaengt). Als Verursacher gilt das zuletzt angelegte Gelenk dieser
    // Baugruppe in der Gruppe (hoechste ID; sonst das zuletzt angelegte ueberhaupt).
    lastRedundancyGroups.clear();
    auto displayName = [this](App::DocumentObject* obj) {
        std::string name = obj->getDocument() == getDocument() ? std::string(obj->getNameInDocument())
                                                                : obj->getFullName();
        if (obj->Label.getStrValue() != obj->getNameInDocument()) {
            name += " '" + obj->Label.getStrValue() + "'";
        }
        if (auto* type = dynamic_cast<App::PropertyEnumeration*>(obj->getPropertyByName("JointType"))) {
            name += " (" + std::string(type->getValueAsString()) + ")";
        }
        return name;
    };
    // FCPROJECT-PATCH (2026-10-14, Regel C7): jede herausgenommene Bedingung nachmessen. Innerhalb
    // der Toleranz gilt sie als erfuellt (geschluckt), darueber ist sie ein Widerspruch.
    ParameterGrp::handle hPgr = App::GetApplication().GetParameterGroupByPath(
        "User parameter:BaseApp/Preferences/Mod/Assembly"
    );
    const double tolLength = hPgr->GetFloat("RedundancyToleranceLength", 0.01);
    const double tolAngle = hPgr->GetFloat("RedundancyToleranceAngle", 0.01);
    lastRedundancyStates.clear();
    lastConflictingJoints.clear();
    std::set<std::string> conflictingSolverNames;
    std::set<std::pair<std::string, std::size_t>> removedNow;
    std::set<std::pair<std::string, std::size_t>> conflictsNow;
    for (const auto& st : mbdAssembly->mbdSystem->redundantConstraintStates()) {
        removedNow.insert({st.jointName, st.index});
        const double dev = st.isAngle
            ? Base::toDegrees(std::asin(std::min(1.0, std::abs(st.value))))
            : std::abs(st.value);
        const bool ok = dev <= (st.isAngle ? tolAngle : tolLength);
        std::string solverName = st.jointName;
        if (solverName.rfind(solverPathPrefix, 0) == 0) {
            solverName = solverName.substr(solverPathPrefix.size());
        }
        auto mapped = solverNameToJoint.find(solverName);
        App::DocumentObject* docObj = mapped != solverNameToJoint.end() ? mapped->second : nullptr;
        const std::string name = docObj ? displayName(docObj) : solverName;
        std::ostringstream line;
        line.precision(4);
        line << name << " [" << st.index << "]: Abweichung " << std::fixed << dev
             << (st.isAngle ? " deg" : " mm") << ", " << (ok ? "geschluckt" : "Konflikt");
        lastRedundancyStates.push_back(line.str());
        if (ok) {
            continue;
        }
        conflictsNow.insert({st.jointName, st.index});
        conflictingSolverNames.insert(st.jointName);
        if (docObj) {
            std::string objName = docObj->getDocument() == getDocument()
                ? std::string(docObj->getNameInDocument())
                : docObj->getFullName();
            if (std::find(lastConflictingJoints.begin(), lastConflictingJoints.end(), objName)
                == lastConflictingJoints.end()) {
                lastConflictingJoints.push_back(objName);
            }
        }
    }
    // Gruppen kommen nur aus einer frischen Suche; sonst die gemerkten.
    const auto& freshGroups = mbdAssembly->mbdSystem->redundancyGroups;
    const auto groupSource = freshGroups.empty() ? currentRedundancyCache().groups : freshGroups;
    // Ein Widerspruch, der schon beim Merken bekannt war, ist kein Grund, frisch zu loesen. Ein
    // NEUER Widerspruch mit vorab herausgenommenen Bedingungen kann dagegen daher ruehren, dass
    // eine gemerkte Bedingung nicht mehr redundant ist (solve() loest dann ohne Gedaechtnis neu).
    newRedundancyConflict = false;
    for (const auto& c : conflictsNow) {
        if (!redundancyCacheApplied || currentRedundancyCache().conflicts.count(c) == 0) {
            newRedundancyConflict = true;
        }
    }
    if (!redundancyCacheApplied || !newRedundancyConflict) {
        currentRedundancyCache().signatures = mbdJointSignatures();
        currentRedundancyCache().removed = removedNow;
        currentRedundancyCache().conflicts = conflictsNow;
        currentRedundancyCache().groups = groupSource;
    }

    std::set<std::vector<App::DocumentObject*>> reportedGroups;
    for (const auto& [removed, members] : groupSource) {
        if (conflictingSolverNames.count(removed) == 0) {
            continue;
        }
        std::vector<App::DocumentObject*> group;
        for (std::string member : members) {
            if (member.rfind(solverPathPrefix, 0) == 0) {
                member = member.substr(solverPathPrefix.size());
            }
            auto mapped = solverNameToJoint.find(member);
            if (mapped != solverNameToJoint.end() && mapped->second
                && std::find(group.begin(), group.end(), mapped->second) == group.end()) {
                group.push_back(mapped->second);
            }
        }
        if (group.size() < 2) {
            continue;
        }
        std::sort(group.begin(), group.end());
        if (!reportedGroups.insert(group).second) {
            continue;
        }
        auto rank = [this](App::DocumentObject* obj) {
            return std::make_pair(obj->getDocument() == getDocument(), obj->getID());
        };
        std::sort(group.begin(), group.end(), [&rank](App::DocumentObject* a, App::DocumentObject* b) {
            return rank(a) > rank(b);
        });
        // FCPROJECT-PATCH (2026-10-14, Regel C7): das Gelenk, dessen Bedingung die Toleranz
        // ueberschreitet, steht vorn - es ist im Modell tatsaechlich nicht erfuellt.
        std::string removedName = removed;
        if (removedName.rfind(solverPathPrefix, 0) == 0) {
            removedName = removedName.substr(solverPathPrefix.size());
        }
        auto violated = solverNameToJoint.find(removedName);
        if (violated != solverNameToJoint.end()) {
            auto it = std::find(group.begin(), group.end(), violated->second);
            if (it != group.end()) {
                std::rotate(group.begin(), it, it + 1);
            }
        }
        std::string line = displayName(group.front()) + " nicht erfuellt, widerspricht:";
        for (std::size_t i = 1; i < group.size(); ++i) {
            line += (i == 1 ? " " : ", ") + displayName(group[i]);
        }
        lastRedundancyGroups.push_back(line);
    }

    // Update the summary boolean flag. FCPROJECT-PATCH (2026-10-14, Regel C7): nur ein Widerspruch
    // ueber der Toleranz zaehlt als Ueberbestimmung; stimmige Redundanz steht nur informativ in
    // lastRedundantJoints/lastRedundancyStates.
    lastHasConflict = !lastConflictingJoints.empty();
    lastHasRedundancies = lastHasConflict;

    signalSolverUpdate();
}

int AssemblyObject::generateSimulation(App::DocumentObject* sim)
{
    // FCPROJECT-PATCH (2026-10-10, Plan plans/2026-10-10-simulation-an-struktur-anpassen.md,
    // Schritt 2): ein IdentityGraph fuer den ganzen Modellaufbau, wie in solve(). Gemessen an BG22
    // ohne spuerbaren Zeitgewinn - die ~10s liegen zu 97% in runKINEMATIC() selbst (101 Zeitschritte
    // ueber das ganze Modell) -, aber derselbe Graph fuer alle Aufloesungen eines Modellaufbaus.
    ScopedIdentityGraph graphScope(this, scopedGraph, /*fresh=*/true);

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
    // FCPROJECT-PATCH (2026-10-20, Zieh-Start-Latenz): Erreichbarkeit EINMAL fuer alle gezogenen
    // Teile statt isPartConnected() je Teil - jede Abfrage baute Erdung, Gelenkliste und
    // Traversierung neu auf. Seit der Motor (BG112) in BG22 als starre Gruppe mitlaeuft, zieht
    // der Z-Schlitten 27 statt 5 Teile; der Zieh-Start wurde dadurch spuerbar langsamer.
    std::unordered_set<App::DocumentObject*> connectedDragParts;
    {
        ScopedIdentityGraph graphScope(this, scopedGraph);
        const auto connectedParts = getConnectedPartSet();
        for (auto* part : dragParts) {
            if (part && connectedParts.contains(canonicalizeForMbD(part))) {
                connectedDragParts.insert(part);
            }
        }
    }
    auto isConnected = [&](App::DocumentObject* part) {
        return connectedDragParts.contains(part);
    };
    bool hasUnconnectedDragPart = std::ranges::any_of(dragParts, [&](App::DocumentObject* part) {
        return part && !isConnected(part);
    });

    if (hasUnconnectedDragPart) {
        prepareMbdForIslandDrag(dragParts);
    }
    else {
        solve();
    }
    bundleFixed = false;

    draggedParts.clear();
    std::vector<App::DocumentObject*> bundledDragParts;

    for (auto part : dragParts) {
        const bool isRigidClustered = getRigidRepresentative(part) != nullptr;

        // make sure no duplicate
        if (std::ranges::find(draggedParts, part) != draggedParts.end()) {
            continue;
        }

        // - Free-floating parts should not be added since they are ignored by the solver.
        // During ungrounded island dragging, prepareMbdForIslandDrag seeds the MBD system from
        // the dragged parts, so objectPartMap is the source of truth instead.
        // - Active rigid-cluster members are solver-connected through the shared MbD part.
        if (!isConnected(part) && (!objectPartMap.contains(part) || !isRigidClustered)) {
            continue;
        }

        // Rigid-cluster members stay draggable because they share one MbD part.
        if (isRigidClustered) {
            draggedParts.push_back(part);
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
            bundledDragParts.push_back(part);
            continue;
        }

        draggedParts.push_back(part);
    }

    // FCPROJECT-PATCH (2026-10-11, live an BG22 gefunden: Halteplatte von BG75 gezogen, sie
    // loeste sich 150mm vom Motor): ein gebuendeltes Teil (offsetPlc != Identitaet) wurde oben
    // immer verworfen - upstream noetig, weil doDragStep() dort die Teil-Placement direkt auf den
    // Buendel-Koerper setzt. War es das EINZIGE gezogene Teil, blieb draggedParts leer,
    // doDragStep() kehrte sofort zurueck, und das Teil folgte der Maus ohne Solver. Unser
    // doDragStep() rechnet das Ziel ueber offsetPlc auf den Buendel-Koerper um; ein gebuendeltes
    // Teil darf diesen Koerper deshalb fuehren - aber nur, wenn kein anderes gezogenes Teil ihn
    // schon fuehrt (das Buendel-Hauptteil hat Vorrang).
    for (auto* part : bundledDragParts) {
        auto body = getMbDPart(part);
        if (!body) {
            continue;
        }
        const bool covered = std::ranges::any_of(draggedParts, [this, &body](App::DocumentObject* p) {
            return getMbDPart(p) == body;
        });
        if (!covered) {
            draggedParts.push_back(part);
        }
    }
}

void AssemblyObject::prepareMbdForIslandDrag(std::vector<App::DocumentObject*> dragParts)
{
    ensureIdentityPlacements();
    syncGroundedJoints();

    mbdAssembly = makeMbdAssembly();
    objectPartMap.clear();
    motions.clear();

    auto seededParts = fixGroundedParts();
    for (auto* part : dragParts) {
        if (part) {
            seededParts.insert(part);
        }
    }

    std::vector<JointRef> joints = getJoints();
    removeUnconnectedJoints(joints, seededParts);
    if (joints.empty()) {
        objectPartMap.clear();
        mbdAssembly.reset();
        return;
    }

    jointParts(joints);

    setJointPriorities(joints);
    // Das Zieh-Modell enthaelt nur die beteiligten Teile und haelt andere fest als solve() - es hat
    // ein eigenes Gedaechtnis (erneutes Ziehen desselben Teils startet ohne Redundanzsuche).
    applyRedundancyCache(dragRedundancyCache);
    try {
        mbdAssembly->runPreDrag();
        dragRedundancyCache.signatures = mbdJointSignatures();
        dragRedundancyCache.removed.clear();
        for (const auto& st : mbdAssembly->mbdSystem->redundantConstraintStates()) {
            dragRedundancyCache.removed.insert({st.jointName, st.index});
        }
    }
    catch (const std::exception& e) {
        FC_ERR("Drag setup failed: " << e.what());
        objectPartMap.clear();
        mbdAssembly.reset();
        return;
    }
    catch (...) {
        FC_ERR("Drag setup failed: unhandled exception");
        objectPartMap.clear();
        mbdAssembly.reset();
        return;
    }
}

void AssemblyObject::doDragStep()
{
    if (!mbdAssembly || draggedParts.empty()) {
        return;
    }

    try {
        std::vector<std::shared_ptr<MbD::ASMTPart>> dragMbdParts;
        std::unordered_set<ASMTPart*> seenMbdParts;
        // Zielplacement je gezogenem Koerper im Solver-Rahmen, in der Reihenfolge von
        // draggedParts - der erste Eintrag ist das Leitteil (siehe Anschlag-Korrektur unten).
        std::vector<std::pair<std::shared_ptr<MbD::ASMTPart>, Base::Placement>> dragTargets;

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
            if (auto it = objectPartMap.find(part); it != objectPartMap.end()) {
                if (!it->second.offsetPlc.isIdentity()) {
                    plc = plc * it->second.offsetPlc.inverse();
                }
                // FCPROJECT-PATCH (2026-09-19, siehe containerChainPlc-Deklaration in
                // AssemblyObject.h): 'plc' ist hier bisher 'part's rohe LOKALE Placement, wird
                // aber gleich als ABSOLUTE Startposition an MbD uebergeben - dieselbe Luecke wie
                // in getMbDData()/setNewPlacements(), nur fuer den interaktiven Drag-Pfad. Ohne
                // diese Korrektur wuerde ein per Maus gezogenes, verschachteltes Bug-C-Spiegel-
                // objekt (Container nicht an Identity) beim Draggen "frei im Raum schweben"
                // (falscher/fehlender Container-Offset in der Startposition), waehrend ein
                // nachfolgendes vollstaendiges solve() wieder korrekt einrastet (das benutzt
                // bereits containerChainPlc, siehe setNewPlacements()).
                if (!it->second.containerChainPlc.isIdentity()) {
                    plc = it->second.containerChainPlc * plc;
                }
            }
            dragTargets.push_back({mbdPart, plc});
        }

        auto applyDragTargets = [&dragTargets]() {
            for (const auto& [mbdPart, plc] : dragTargets) {
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
        };
        applyDragTargets();

        // Timing mbdAssembly->runDragStep()
        auto dragPartsVec = std::make_shared<std::vector<std::shared_ptr<ASMTPart>>>(dragMbdParts);
        mbdAssembly->runDragStep(dragPartsVec);

        // FCPROJECT-PATCH (2026-10-10, "Rotor dreht beim Schieben des Traegers", BG22): die GUI
        // gibt JEDEM mitgezogenen Koerper als Ziel "Startlage + Mausweg" vor. Erreicht das
        // Leitteil sein Ziel nicht, weil ein Gelenk-Anschlag (Slider-Limit) es festhaelt, laufen
        // die Ziele der Mitlaeufer trotzdem weiter - der Solver muss den Widerspruch aufloesen und
        // tut das ueber freie Freiheitsgrade (live: Rotor um 9 Grad und mehr gedreht). Dann die
        // Ziele der Mitlaeufer an die TATSAECHLICH erreichte Bewegung des Leitteils anpassen und
        // den Schritt einmal wiederholen. Vor dem Anschlag (Ziel erreicht) kein Zusatzaufwand.
        // Beschrieben in docs/ARCHITECTURE.md Abschnitt 3.2a.
        if (dragTargets.size() > 1) {
            constexpr double leaderReachedTolerance = 1e-4;
            const Base::Placement leaderTarget = dragTargets.front().second;
            const Base::Placement leaderReached = getMbdPlacement(dragTargets.front().first);
            if (!leaderReached.isSame(leaderTarget, leaderReachedTolerance)) {
                const Base::Placement correction = leaderReached * leaderTarget.inverse();
                dragTargets.front().second = leaderReached;
                for (std::size_t i = 1; i < dragTargets.size(); ++i) {
                    dragTargets[i].second = correction * dragTargets[i].second;
                }
                applyDragTargets();
                mbdAssembly->runDragStep(dragPartsVec);
            }
        }

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

                // FCPROJECT-PATCH (2026-09-29, siehe fix-grounded-part-container-chain-
                // convergence.md - dieselbe Bug-Klasse, hier im Drag-Validierungspfad): 'oldPlc'
                // ist 'obj's ROHE LOKALE Placement-Property (z.B. der Motor-Stator 048 an seinem
                // eigenen Ursprung INNERHALB der verschachtelten BG67), waehrend 'newPlacement'
                // aus dem MbD-Koerper kommt, der bei der Erdung bereits MIT Container-Kette
                // (alreadyResolved=true, siehe fixGroundedPart()) in WELT-Koordinaten geseedet
                // wurde. Ohne dieselbe Komposition hier weicht 'oldPlc' IMMER um genau die
                // Container-Kette (z.B. BG67s eigene Platzierung innerhalb BG43) von
                // 'newPlacement' ab - jeder einzelne Drag-Schritt eines geerdeten Teils in einer
                // verschachtelten, nicht-rigiden Unterbaugruppe wird dadurch bedingungslos als
                // "bewegt" erkannt und verworfen, selbst wenn das Teil sich in Wahrheit ueberhaupt
                // nicht bewegt hat (live an BG43/BG67 gefunden: 048 blieb rechnerisch fest, wurde
                // aber bei JEDEM Rotor-Drag-Schritt als "moved" abgelehnt).
                if (!it->second.containerChainPlc.isIdentity()) {
                    oldPlc = it->second.containerChainPlc * oldPlc;
                }

                if (!oldPlc.isSame(newPlacement, Precision::Confusion())) {
                    Base::Console().warning(
                        "Assembly : Ignoring bad solve, a grounded object ({}) moved.\n",
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
    if (!mbdAssembly) {
        return;
    }

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

    // FCPROJECT-PATCH (2026-10-20, starre Gruppe ueber Baugruppengrenze, BG112 in BG22): auch
    // die starren Gruppen flexibel eingebauter Unterbaugruppen, Mitglieder ueber den
    // IdentityGraph auf dieselben Solver-Schluessel abgebildet wie Gelenk-Referenzen (Instanz im
    // aktiven Kontext). Vorher las dies nur die eigene Gelenkgruppe: im Kontext von BG22 hing vom
    // Motor (BG112, 21 Teile in einer starren Gruppe) nur das per Gelenk angebundene Teil.
    std::optional<IdentityGraph> ownGraph;
    IdentityGraph& graph = scopedGraph ? *scopedGraph : ownGraph.emplace(this);
    for (const auto& groupRef : getRigidGroupInstances()) {
        auto* const prop = groupRef.group ? dynamic_cast<App::PropertyLinkList*>(
                               groupRef.group->getPropertyByName("ObjectsToRigidGroup")
                           )
                                          : nullptr;
        if (!prop) {
            continue;
        }

        std::vector<App::DocumentObject*> keys;
        for (auto* const member : prop->getValues()) {
            IdentityHandle handle = graph.resolveRigidMember(member, groupRef.nestingPrefix);
            if (auto* key = handle.templateObj ? graph.materializeForObjectPartMap(handle) : nullptr) {
                keys.push_back(key);
            }
        }
        if (keys.size() < 2) {
            continue;
        }

        auto* const first = keys.front();
        for (auto* const key : keys | std::views::drop(1)) {
            unite(first, key);
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

namespace
{
// A singular solve can return NaN or infinite placements. NaN coordinates defeat
// the bounding-box rejection in SoRayPickAction, so writing one into the document
// makes every ray pick hit everything; reject them at the solver/document boundary.
bool isFinitePlacement(const Base::Placement& plc)
{
    const Base::Vector3d& pos = plc.getPosition();
    double q0 {};
    double q1 {};
    double q2 {};
    double q3 {};
    plc.getRotation().getValue(q0, q1, q2, q3);
    return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z) && std::isfinite(q0)
        && std::isfinite(q1) && std::isfinite(q2) && std::isfinite(q3);
}
}  // namespace

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

        // Geerdet heisst fest (siehe groundedBodies in AssemblyObject.h).
        if (groundedBodies.count(obj)) {
            if (obj->isTouched()) {
                obj->purgeTouched();
            }
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
        if (!pair.second.containerChainPlc.isIdentity()) {
            // FCPROJECT-PATCH (2026-09-19, "zweite BG25-Instanz falsch eingesetzt" - siehe
            // containerChainPlc-Deklaration in AssemblyObject.h): bis hierhin ist 'newPlacement'
            // die vom Solver berechnete ABSOLUTE Weltposition. 'obj' lebt aber verschachtelt
            // innerhalb eines oder mehrerer AssemblyLink-Container, deren eigene Placement NICHT
            // zwingend Identity ist (z.B. zweite Instanz einer flexiblen Unterbaugruppe, vom
            // Nutzer im Baum manuell weggezogen). Ohne diese Ruecktransformation wuerde die
            // absolute Weltposition direkt als lokale (Container-relative) Placement geschrieben -
            // beim Rendern kaeme der Container-Offset dann ein ZWEITES Mal oben drauf.
            newPlacement = pair.second.containerChainPlc.inverse() * newPlacement;
        }
        if (!isFinitePlacement(newPlacement)) {
            Base::Console().warning(
                "Assembly: solver returned a non-finite placement for '{}'; keeping its "
                "previous position.\n",
                obj->getFullName()
            );
            continue;
        }
        // FCPROJECT-PATCH (2026-10-03, Nutzerregel "zeigt ja, aendert nein"): nur schreiben, wenn
        // sich wirklich etwas bewegt hat - reines Rechenrauschen (live an BG22: 1e-12 mm an einem
        // Winkel in BG11 beim Abhaengigkeits-Recompute) markierte sonst ein Dokument als geaendert.
        constexpr double placementWriteTolerance = 1e-9;
        if (!propPlacement->getValue().isSame(newPlacement, placementWriteTolerance)) {
            propPlacement->setValue(newPlacement);
            obj->purgeTouched();
        }
        else if (obj->isTouched()) {
            // Nutzerbefund 2026-09-27 ("still touched after recompute" bei geerdeten Teilen wie
            // Fuehrung330/331): ein geerdetes Teil AENDERT seine Placement nie (isSame() bleibt
            // immer true), bekommt aber trotzdem gelegentlich ein gesetztes Touched-Flag - nicht
            // von uns, sondern von FreeCADs eigener dokumentuebergreifender Link-Weiterleitung
            // (_LinkTouched), wenn dasselbe kanonische Objekt ueber eine ANDERE Instanz (z.B. eine
            // zweite FuerungsBaugruppe, die dieselbe Fuehrung referenziert) im selben Rekompute-
            // Zyklus erneut beruehrt wird, NACHDEM dieser solve()-Durchlauf sein eigenes
            // purgeTouched() schon laengst ausgefuehrt hat. Ohne diesen Zweig bleibt das Flag bis
            // zum Ende des gesamten Dokument-Rekomputes stehen, obwohl inhaltlich alles korrekt
            // ist - siehe Document.cpp's Warnung "still touched after recompute".
            obj->purgeTouched();
        }

        // FCPROJECT-PATCH (2026-10-03, Plan plans/2026-10-03-solver-nur-auf-spiegeln.md): kein
        // syncLocalMirrorPlacement() mehr. 'obj' ist jetzt die INSTANZ im Kontext dieser
        // Baugruppe (IdentityGraph::instanceOf(), siehe materializeForObjectPartMap()) - also genau
        // das Objekt, das hier gerendert und gezogen wird. Es gibt keinen weiteren Spiegel in diesem
        // Kontext; Originale und Spiegel anderer Dokumente gehoeren nicht zu diesem Solve und werden
        // nicht mehr beschrieben (Nutzerregel: Original darf von aussen nie geaendert werden).
    }

    // Nutzerbefund 2026-09-27 ("still touched after recompute" bei geerdeten Teilen wie
    // Fuehrung330/331, live an BG22): die beiden isTouched()-Zweige oben (im Hauptzweig und in
    // syncLocalMirrorPlacement()) faengen den haeufigsten Fall ab, aber NICHT eine
    // Querbeeinflussung ZWISCHEN zwei verschiedenen objectPartMap-Eintraegen - z.B. wenn das
    // Setzen der Placement fuer EIN kanonisches Teil (per FreeCADs eigener dokumentuebergreifender
    // Link-Weiterleitung, _LinkTouched) ein BEREITS in einer FRUEHEREN Schleifen-Iteration
    // verarbeitetes und schon gesaeubertes Teil (bzw. dessen lokalen Spiegel) im selben Dokument
    // erneut beruehrt. Ein abschliessender, zweiter Durchlauf ueber ALLE Eintraege nach der
    // Hauptschleife faengt genau das ab, unabhaengig von der Bearbeitungsreihenfolge.
    for (auto& pair : objectPartMap) {
        App::DocumentObject* obj = pair.first;
        if (obj && obj->isTouched()) {
            obj->purgeTouched();
        }
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
// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 1): die bisherige Brute-Force-Suche
// (jeden Kandidaten unterhalb DIESER Instanz per canonicalizeForMbD() gegen 'realObj' pruefen)
// scheitert grundsaetzlich, sobald 'realObj' hinter einer duplizierten Instanz liegt -
// canonicalizeForMbD() gibt bei JEDER Instanz-Duplikation sofort mit dem unveraenderten
// Eingabewert auf (siehe dessen Kommentar), der Vergleich "canonicalizeForMbD(candidate) ==
// realObj" kann dadurch fuer KEINEN Kandidaten unterhalb der duplizierten Ebene je zutreffen -
// exakt die Ursache der live beobachteten "elf nie aktualisierten Getriebeteil-Spiegel"
// (BG37->BG43->BG67). graph.resolveObject()/mirrorsOf() loesen tiefengenerell auf: 'realObj' ist
// entweder das tiefe echte Objekt (keine Duplikation - mirrorsOf() sucht dann im gesamten
// lokalen Baum dieser Instanz) oder bereits selbst der instanzeigene lokale Spiegel (Duplikation
// im Spiel, siehe resolvePartForMbD() - mirrorsOf() sucht dann gezielt NUR innerhalb der
// zugehoerigen Instanz, nicht auch bei anderen Duplikat-Instanzen).
void AssemblyObject::syncLocalMirrorPlacement(App::DocumentObject* realObj, const Base::Placement& plc)
{
    if (!realObj) {
        return;
    }

    IdentityGraph graph(this);
    IdentityHandle handle = graph.resolveObject(realObj);

    for (auto* candidate : graph.mirrorsOf(handle)) {
        if (!candidate || candidate == realObj) {
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
        else if (candidate->isTouched()) {
            // Nutzerbefund 2026-09-27: siehe ausfuehrliche Begruendung am analogen Zweig in
            // setNewPlacements() - derselbe Effekt trifft hier den lokalen Spiegel (z.B. BG22s
            // Fuehrung330/331-Link), nicht nur das kanonische Objekt selbst.
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

    try {
        auto* proxy = dynamic_cast<App::PropertyPythonObject*>(joint->getPropertyByName("Proxy"));

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
    catch (Py::Exception&) {
        // Callers run inside Qt event handlers, which cannot propagate C++ exceptions
        // out of the joint's Python callback. Report the error and keep redrawing the
        // remaining joints.
        Base::PyException e;
        e.reportException();
    }
}

// FCPROJECT-PATCH (2026-10-10): Weltlage des Gelenk-Bezugssystems im AKTUELLEN Kontext - Marker
// relativ zum Teil, mit der Lage des MbD-Koerpers (Instanz samt Container-Kette, offsetPlc) in die
// Welt gebracht. Gilt fuer Spiegel wie fuer lokale Teile (Regel C2); nach einem Solve also der
// geloeste Stand.
std::optional<Base::Placement> AssemblyObject::getJcsInContext(
    App::DocumentObject* joint,
    const char* refName,
    const char* plcName,
    const std::string& nestingPrefix
)
{
    App::DocumentObject* part = resolvePartForMbD(joint, refName, nestingPrefix);
    auto* ref = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName(refName));
    if (!part || !ref) {
        return std::nullopt;
    }
    MbDPartData data = getMbDData(part, /*alreadyResolved=*/true);
    if (!data.part) {
        return std::nullopt;
    }
    Base::Placement plc = getPlacementFromProp(joint, plcName);
    plc = getJcsReferenceGlobalPlacement(ref) * plc;
    plc = getGlobalPlacement(part, ref).inverse() * plc;
    return getMbdPlacement(data.part) * data.offsetPlc * plc;
}

// FCPROJECT-PATCH (2026-10-14, Regel C7): Kennung aller Gelenke/Antriebe des aktuellen
// Solver-Modells (Klasse, Name, Marker I/J). Aendert sich ein Gelenk, ein Koerper (starr/flexibel)
// oder eine Erdung, aendert sich auch die Kennung.
std::set<std::string> AssemblyObject::mbdJointSignatures() const
{
    std::set<std::string> signatures;
    if (!mbdAssembly) {
        return signatures;
    }
    for (const auto& joint : *mbdAssembly->joints) {
        signatures.insert(joint->classname() + "|" + joint->name + "|" + joint->markerI + "|"
                          + joint->markerJ);
    }
    for (const auto& motion : *mbdAssembly->motions) {
        signatures.insert(motion->classname() + "|" + motion->name + "|" + motion->markerI + "|"
                          + motion->markerJ);
    }
    return signatures;
}

// FCPROJECT-PATCH (2026-10-14, Regel C7): Rangfolge fuer die Redundanzsuche - was schon da ist,
// gilt; redundant (und gegen die Toleranz geprueft) ist das Neueste. Erdungen und Gelenke aus
// Unterbaugruppen (dort fertig konstruiert) vor den eigenen Gelenken, diese nach Alter (ID).
void AssemblyObject::setJointPriorities(const std::vector<JointRef>& jointRefs)
{
    if (!mbdAssembly) {
        return;
    }
    mbdAssembly->jointPriority.clear();
    for (const auto& jr : jointRefs) {
        if (!jr.joint) {
            continue;
        }
        const long rank = jr.joint->getDocument() == getDocument()
            ? 1000000L + static_cast<long>(jr.joint->getID())
            : 1L;
        mbdAssembly->jointPriority[jointInstanceName(jr.joint, jr.nestingPrefix)] = rank;
    }
}

// Gemerkte Redundanzen vorab herausnehmen, wenn alle gemerkten Gelenke unveraendert im Modell
// stecken. Neue Gelenke duerfen dazukommen: eine abhaengige Gleichung bleibt abhaengig, wenn
// weitere Gleichungen hinzukommen; neue Redundanzen findet OndselSolver dann selbst.
bool AssemblyObject::applyRedundancyCache(const RedundancyCache& cache)
{
    if (!mbdAssembly || cache.removed.empty()) {
        return false;
    }
    const auto current = mbdJointSignatures();
    if (!std::includes(current.begin(), current.end(), cache.signatures.begin(), cache.signatures.end())) {
        return false;
    }
    mbdAssembly->knownRedundantConstraints = cache.removed;
    return true;
}

// FCPROJECT-PATCH (2026-10-13, Nutzerentscheidung "Aenderung im Original"): Aenderungen im
// Original werden in eingebauten Instanzen nicht nachgezogen (C2) - ausser Gelenkgrenzen: der
// Grenz-Durchgang in solve() zieht die Instanz in die neue Grenze. Laesst sich eine Grenze nicht
// einhalten (z.B. weil aeussere Gelenke das Teil festhalten), verwirft OndselSolver den Durchgang
// still (ASMTAssembly::runDragStep() -> restorePosRot()). Deshalb hier jede angelegte Grenze am
// Ergebnis nachmessen und verletzte melden.
void AssemblyObject::reportUnsatisfiedLimits()
{
    lastLimitViolations.clear();
    constexpr double lengthTol = 1e-6;
    constexpr double angleTolDeg = 1e-4;
    for (const auto& check : activeLimits) {
        const auto jcs1 = getJcsInContext(check.joint, "Reference1", "Placement1", check.nestingPrefix);
        const auto jcs2 = getJcsInContext(check.joint, "Reference2", "Placement2", check.nestingPrefix);
        if (!jcs1 || !jcs2) {
            continue;
        }
        double value = getJointValueFromJcs(*jcs1, *jcs2, check.isAngle);
        const double tol = check.isAngle ? angleTolDeg : lengthTol;
        if (check.isAngle) {
            value = Base::toDegrees(value);
            // atan2 liefert -180..180; Grenzen jenseits davon ueber die naechste Umdrehung messen.
            const double mid = check.minEnabled && check.maxEnabled ? (check.min + check.max) / 2.0
                : check.minEnabled                                   ? check.min
                                                                     : check.max;
            value += 360.0 * std::round((mid - value) / 360.0);
            if (check.minEnabled && check.maxEnabled && value > check.max + tol) {
                value -= 360.0;
            }
        }
        const bool belowMin = check.minEnabled && value < check.min - tol;
        const bool aboveMax = check.maxEnabled && value > check.max + tol;
        if (!belowMin && !aboveMax) {
            continue;
        }
        const char* unit = check.isAngle ? "deg" : "mm";
        std::string name = check.joint->getDocument() == getDocument()
            ? std::string(check.joint->getNameInDocument())
            : check.joint->getFullName();
        if (check.joint->Label.getStrValue() != check.joint->getNameInDocument()) {
            name += " '" + check.joint->Label.getStrValue() + "'";
        }
        std::ostringstream line;
        line.precision(4);
        line << std::fixed << name << ": " << value << " " << unit << ", erlaubt ";
        if (check.minEnabled) {
            line << check.min;
        }
        line << " .. ";
        if (check.maxEnabled) {
            line << check.max;
        }
        line << " " << unit;
        lastLimitViolations.push_back(line.str());
        Base::Console().warning(
            "Assembly: Gelenkgrenze in '{}' nicht einhaltbar: {}\n",
            getFullName(),
            line.str()
        );
    }
}

std::shared_ptr<ASMTAssembly> AssemblyObject::makeMbdAssembly()
{
    activeLimits.clear();
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
    const std::vector<App::DocumentObject*>& excludeJoints,
    std::string* nestingPrefixOut
)
{
    if (!part) {
        return nullptr;
    }

    // FCPROJECT-PATCH (2026-09-29, BG47-Drag "wird weggezogen" beim direkten Anklicken):
    // getJointsOfPart(part) kanonisiert 'part' bereits INTERN (siehe dortiger Kommentar), der
    // Kommentar unten ("'part' ist bereits die instanzeigene, kanonische Identitaet") war daher
    // nur eine Annahme ueber den Aufrufer, nicht durch diese Funktion selbst sichergestellt - der
    // Vergleich `part == part1/part2` weiter unten nutzte weiterhin den ROHEN, nicht kanonisierten
    // Parameter. Bei einem UI-/klick-aufgeloesten 'part', dessen Identitaet von der ueber
    // resolvePartForMbD() aufgeloesten Kanonischen abweicht (dieselbe Bug-Klasse wie in
    // getDownstreamParts(), siehe fix-getdownstreamparts-canonicalize-identity.md), schlaegt der
    // Vergleich fehl und die Funktion liefert faelschlich nullptr - findDragMode() faellt dann auf
    // den "ungeerdet frei ziehen"-Zweig zurueck, obwohl ein Fixed-Joint zu einem geerdeten
    // Nachbarn existiert (live beobachtet: BG47 direkt anklicken+ziehen loeste sich vom Rotor,
    // obwohl derselbe Fixed-Joint beim Ziehen ueber den Rotor korrekt gefunden wird).
    // FCPROJECT-PATCH (2026-10-10, Drag-Start-Latenz): ein gemeinsamer IdentityGraph fuer die
    // Pruefung aller Joints des Teils (isJointConnectingPartToGround() je Joint).
    ScopedIdentityGraph graphScope(this, scopedGraph);

    App::DocumentObject* canonicalPart = canonicalizeForMbD(part);
    if (!canonicalPart) {
        canonicalPart = part;
    }

    std::vector<JointRef> joints = getJointsOfPart(part);

    for (auto& jr : joints) {
        App::DocumentObject* joint = jr.joint;
        if (!joint) {
            continue;
        }

        if (std::ranges::find(excludeJoints, joint) != excludeJoints.end()) {
            continue;
        }

        // FCPROJECT-PATCH (2026-09-20, siehe getJointsOfPart()-Deklaration): resolvePartForMbD()
        // mit demselben nestingPrefix statt der adressierungsblinden getMovingPartFromRef().
        App::DocumentObject* part1 = resolvePartForMbD(joint, "Reference1", jr.nestingPrefix);
        App::DocumentObject* part2 = resolvePartForMbD(joint, "Reference2", jr.nestingPrefix);
        if (!part1 || !part2) {
            continue;
        }

        if (canonicalPart == part1
            && isJointConnectingPartToGround(joint, "Reference1", jr.nestingPrefix)) {
            name = "Reference1";
            if (nestingPrefixOut) {
                *nestingPrefixOut = jr.nestingPrefix;
            }
            return joint;
        }
        if (canonicalPart == part2
            && isJointConnectingPartToGround(joint, "Reference2", jr.nestingPrefix)) {
            name = "Reference2";
            if (nestingPrefixOut) {
                *nestingPrefixOut = jr.nestingPrefix;
            }
            return joint;
        }
    }
    return nullptr;
}

App::DocumentObject* AssemblyObject::getJointOfPartForUngroundedDrag(
    App::DocumentObject* part,
    std::string& name
)
{
    if (!part) {
        return nullptr;
    }

    // FCPROJECT-PATCH (Vanilla-Drift-Kollision, siehe getJointsOfPart()-Deklaration): diese
    // Funktion kam erst mit dem Upstream-Merge #30998 (asm_island_drag) zwischen dem alten und
    // dem neuen Vanilla-Sync-Stand hinzu - unsere JointRef-Migration von getJointsOfPart()
    // wusste beim urspruenglichen Schreiben noch nichts von ihr.
    std::vector<JointRef> joints = getJointsOfPart(part);
    for (auto& jr : joints) {
        App::DocumentObject* joint = jr.joint;
        if (!joint || !isJointTypeConnecting(joint)) {
            continue;
        }
        if (getJointType(joint) == JointType::Fixed) {
            continue;
        }

        App::DocumentObject* part1 = getMovingPartFromRef(joint, "Reference1");
        App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");
        if (!part1 || !part2) {
            continue;
        }

        std::string refName;
        if (part == part1) {
            refName = "Reference1";
        }
        else if (part == part2) {
            refName = "Reference2";
        }
        else {
            continue;
        }

        name = refName;
        return joint;
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
                "FCPROJECT-DEBUG getJoints: joint='{}' isError={} Suppressed={}\n",
                joint->getNameInDocument(),
                joint->isError() ? 1 : 0,
                (prop && prop->getValue()) ? 1 : 0
            );
        }
        if (joint->isError() || !prop || prop->getValue()) {
            // Filter grounded joints and deactivated joints.
            if (verboseLog) {
                Base::Console().log(
                    "Assembly: getJoints('{}') - Joint '{}' uebersprungen (isError={}, "
                    "Suppressed={}).\n",
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
                "FCPROJECT-DEBUG getJoints: joint='{}' part1='{}' part2='{}'\n",
                joint->getNameInDocument(),
                part1 ? part1->getFullName().c_str() : "<null>",
                part2 ? part2->getFullName().c_str() : "<null>"
            );
        }
        if (!part1 || !part2 || part1->getFullName() == part2->getFullName()) {
            // Remove incomplete joints. Left-over when the user deletes a part.
            // Remove incoherent joints (self-pointing joints)
            if (verboseLog) {
                Base::Console().log(
                    "Assembly: getJoints('{}') - Joint '{}' uebersprungen: part1='{}', "
                    "part2='{}' (unvollstaendig oder BEIDE Referenzen zeigen auf dasselbe "
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
                    Base::Console().log(
                        "Assembly: getJoints('{}') - Joint '{}' UEBERNOMMEN: part1='{}', "
                        "part2='{}'.\n",
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
                Base::Console().log(
                    "Assembly: getJoints('{}') - Joint '{}' uebersprungen: Proxy hat keine "
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

std::vector<App::DocumentObject*> AssemblyObject::getRigidGroups(bool fixMembers)
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

            if (fixMembers && validMembers.size() != rawMembers.size()) {
                prop->setValue(validMembers);
            }

            rigid_groups.emplace_back(obj);
        }
    }

    return rigid_groups;
}

std::vector<RigidGroupRef> AssemblyObject::getRigidGroupInstances(const std::string& nestingPrefix)
{
    std::vector<RigidGroupRef> groups;
    for (auto* group : getRigidGroups(/*fixMembers=*/nestingPrefix.empty())) {
        groups.push_back({group, nestingPrefix});
    }
    // Rekursion wie in getJoints(): nur flexible Unterbaugruppen.
    for (auto* assembly : getSubAssemblies()) {
        if (!assembly || assembly->isRigid()) {
            continue;
        }
        AssemblyObject* nestedAssembly = assembly->getLinkedAssembly();
        if (!nestedAssembly || nestedAssembly == this) {
            continue;
        }
        auto nested = nestedAssembly->getRigidGroupInstances(
            nestingPrefix + assembly->getNameInDocument() + "."
        );
        groups.insert(groups.end(), nested.begin(), nested.end());
    }
    return groups;
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

std::vector<JointRef> AssemblyObject::getJointsOfPart(App::DocumentObject* part)
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

    std::vector<JointRef> joints = getJoints();
    std::vector<JointRef> jointsOf;

    // FCPROJECT-PATCH (2026-09-20, siehe Deklaration in AssemblyObject.h): resolvePartForMbD()
    // statt der adressierungsblinden getMovingPartFromRef()+canonicalizeForMbD()-Kombination -
    // muss mit demselben nestingPrefix aufgeloest werden, mit dem getJoints() diesen Joint (ggf.
    // ein Subjoint einer verschachtelten flexiblen AssemblyLink) ueberhaupt gefunden hat, sonst
    // ist der Vergleich bei einer duplizierten Unterbaugruppe strukturell unentscheidbar (siehe
    // ausfuehrliche Begruendung an der Deklaration).
    for (auto& jr : joints) {
        App::DocumentObject* part1 = resolvePartForMbD(jr.joint, "Reference1", jr.nestingPrefix);
        App::DocumentObject* part2 = resolvePartForMbD(jr.joint, "Reference2", jr.nestingPrefix);
        if (canonicalPart == part1 || canonicalPart == part2) {
            jointsOf.push_back(jr);
        }
    }
    return jointsOf;
}

namespace
{
// FCPROJECT-PATCH (2026-10-13, live an BG22 gefunden: BG73 und BG75 enthalten beide den Motor
// BG67, beide erden dessen Stator; in BG22 landete die Erdung von BG73 beim Stator der
// BG75-Instanz - BG73 schwamm, BG75 war doppelt gehalten): die Instanz eines Objekts aus dem
// Dokumentbaum der von 'link' verlinkten Baugruppe in DIESEM Kontext. Der Weg von 'obj' durch die
// Spiegel-Container der Quelle wird Ebene fuer Ebene durch die Spiegel unter 'link' nachgegangen
// - eindeutig fuer genau diese Einbau-Instanz, gleich wie oft die innere Baugruppe sonst noch
// vorkommt. nullptr, wenn 'obj' dort nicht vorkommt.
App::DocumentObject* mirrorChildFor(AssemblyLink* mirrorContainer, App::DocumentObject* source)
{
    auto it = mirrorContainer->objLinkMap.find(source);
    if (it != mirrorContainer->objLinkMap.end() && it->second) {
        return it->second;
    }
    for (auto* child : mirrorContainer->Group.getValues()) {
        App::DocumentObject* linked = nullptr;
        if (auto* asmLink = freecad_cast<AssemblyLink*>(child)) {
            linked = asmLink->getLinkedObject2(false);
        }
        else if (auto* link = dynamic_cast<App::Link*>(child)) {
            linked = link->getLinkedObject(false);
        }
        if (linked == source) {
            return child;
        }
    }
    return nullptr;
}

App::DocumentObject* findInstanceUnder(
    App::DocumentObject* sourceContainer,  // AssemblyObject oder Spiegel-AssemblyLink der Quelle
    AssemblyLink* mirrorContainer,
    App::DocumentObject* obj,
    int depth
)
{
    constexpr int maxDepth = 16;
    if (!sourceContainer || !mirrorContainer || depth > maxDepth) {
        return nullptr;
    }
    auto* sourceLink = freecad_cast<AssemblyLink*>(sourceContainer);
    auto* groupExt = sourceContainer->getExtensionByType<App::GroupExtension>(true);
    if (!groupExt) {
        return nullptr;
    }
    for (auto* child : groupExt->Group.getValues()) {
        if (!child) {
            continue;
        }
        // Schluessel im Spiegel: auf oberster Ebene das Objekt selbst, darunter dessen Quelle
        // (die Spiegel unter 'mirrorContainer' verweisen auf dieselbe innerste Quelle).
        App::DocumentObject* key = child;
        if (sourceLink) {
            if (auto* src = sourceLink->getSourceForMirror(child)) {
                key = src;
            }
        }
        App::DocumentObject* mirrorChild = mirrorChildFor(mirrorContainer, key);
        if (!mirrorChild) {
            continue;
        }
        if (child == obj) {
            return mirrorChild;
        }
        auto* childLink = freecad_cast<AssemblyLink*>(child);
        auto* mirrorChildLink = freecad_cast<AssemblyLink*>(mirrorChild);
        if (childLink && mirrorChildLink) {
            if (auto* found = findInstanceUnder(childLink, mirrorChildLink, obj, depth + 1)) {
                return found;
            }
        }
    }
    return nullptr;
}
}  // namespace

std::unordered_set<App::DocumentObject*> AssemblyObject::getGroundedParts(
    std::vector<std::pair<App::DocumentObject*, App::DocumentObject*>>* ignoredNestedRoots,
    bool propagateRigid
)
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
    // FCPROJECT-PATCH (2026-10-03, "Erdung ist Kontext-Eigenschaft", live an BG43/BG67/BG47
    // gefunden, Nutzerauftrag "mit System, fuer alle verschachtelten BGs allgemein", Plan
    // plans/2026-10-03-erdung-als-kontext-eigenschaft.md): der obige Erreichbarkeits-Filter ist
    // hier zu einer allgemeinen Cluster-Aufloesung verallgemeinert und ERSETZT die bisherige
    // "Doppel-Anker-Abschaltung" in syncGroundedJoints() (2026-09-29). Jene schaltete ueberzaehlige
    // verschachtelte Erdungen ab, indem sie das ReadOnly-Flag der Placement IM DOKUMENT DER
    // UNTERBAUGRUPPE loeschte - genau das Flag, aus dem jede Ebene (auch die Unterbaugruppe fuer
    // sich allein) ihre Erdung ableitet. Folge: BG67 galt nach einem Recompute von BG43 auch
    // eigenstaendig als ungeerdet, alle Joints bekamen das Part_Detached-Overlay (Verstoss gegen
    // requirement-activated-bg-independent-of-loaded-context). Jetzt ist getGroundedParts() eine
    // REINE Funktion des Kontexts: jede Ebene behaelt ihre eigene Erdung im Dokument, die aeussere
    // Ebene IGNORIERT ueberzaehlige innere Erdungen nur fuer ihren eigenen Solve.
    //
    // Rekursive Komposition: nestedAssembly->getGroundedParts() liefert bereits die AUFGELOESTEN,
    // wirksamen Wurzeln dieser Unterbaugruppe - hier werden nur noch Konflikte zwischen den
    // LOKALEN Wurzeln dieser Ebene und den Wurzeln der direkten flexiblen Unterbaugruppen
    // aufgeloest. Damit ist jede Verschachtelungstiefe abgedeckt. Regeln pro Zusammenhangs-
    // komponente (Cluster, ueber echte Joints dieser Ebene inkl. verschachtelter Joints):
    //   - gleiche kanonische Identitaet (z.B. lokaler Spiegel und echtes Teil) -> ein Eintrag,
    //     der lokale gewinnt;
    //   - Cluster mit mindestens einer lokalen Wurzel -> alle lokalen bleiben, alle
    //     verschachtelten werden ignoriert (die aeussere, "weltnaehere" Erdung ist massgeblich);
    //   - Cluster nur aus verschachtelten Wurzeln -> die Wurzeln der ERSTEN beteiligten
    //     Unterbaugruppe (getSubAssemblies()-Reihenfolge, deterministisch) bleiben, die der
    //     uebrigen werden ignoriert (zwei unabhaengig verankerte Strukturen + Kreuz-Joint).
    // Umsetzung komplett ueber den IdentityGraph (Nutzerauftrag "mehr ueber IdentityGraph und
    // seine Funktionen"): EIN Graph fuer den ganzen Aufruf, Wurzeln ueber resolveObject()
    // (instanzbewusst), Zusammenhang ueber jointEdges() - dieselben, per resolveJointRef()
    // aufgeloesten Kanten, die auch der Solver sieht. Verglichen wird ueber
    // materializeForObjectPartMap(), also exakt den Schluessel, unter dem der Solver einen
    // Koerper fuehrt: ein roher lokaler Spiegel und das echte Teil landen so auf demselben
    // Koerper, zwei Instanzen derselben Unterbaugruppe (MidLink1/MidLink2) bleiben getrennt.
    // Verglichen wird bewusst ueber den Solver-Koerper, nicht ueber IdentityHandle-Gleichheit -
    // die Frage hier ist genau "derselbe Koerper?". (Anlass war ein inzwischen behobener
    // Handle-Split in refineNestedMirrorTarget(), siehe dortigen Kommentar; die Invariante
    // "ein Koerper = ein Handle" prueft jetzt verifyHandleConsistency() bzw. der Test
    // test_identitygraph_handle_consistency.)
    // Achtung: graph.isGrounded()/groundedHandles() rufen selbst getGroundedParts() auf und
    // duerfen hier deshalb NICHT benutzt werden.
    // Erst die verschachtelten Erdungs-Kandidaten einsammeln (billig, ohne Graph) - der
    // IdentityGraph wird nur gebaut, wenn es ueberhaupt welche gibt. getGroundedParts() liegt im
    // Drag-Heisspfad (isPartConnected() pro Mausereignis), eine flache Baugruppe zahlt so nichts.
    struct NestedCandidate
    {
        AssemblyLink* link;
        int subIndex;
        App::DocumentObject* obj;
    };
    std::vector<NestedCandidate> nestedCandidates;
    int subIndex = 0;
    for (auto* assembly : getSubAssemblies()) {
        if (!assembly || assembly->isRigid()) {
            continue;
        }
        AssemblyObject* nestedAssembly = assembly->getLinkedAssembly();
        if (!nestedAssembly || nestedAssembly == this) {
            continue;
        }
        // FCPROJECT-PATCH (2026-10-03, "Original-PLC aendert sich/Motor bewegt sich bei jedem
        // Solve" - live an BG43/BG67/BG47 gefunden): nestedAssembly->getGroundedParts() liefert
        // IMMER auch nestedAssembly->Origin.getValue() mit (vanilla-Logik weiter oben in dieser
        // Funktion, "Origin is not in Group so we add it separately" - fuer einen EIGENSTAENDIGEN
        // Solve von nestedAssembly selbst korrekt: sein Origin IST per Definition sein eigener,
        // fixer Nullpunkt). Hier, beim rekursiven Einsammeln FREMDER Erdungen fuer DIESE (aeussere)
        // Ebene, ist das falsch: Origin ist kein echtes, ueber Gelenke verbundenes Teil, sondern
        // nur ein interner Referenzpunkt der verschachtelten Baugruppe - OHNE diesen Ausschluss
        // landet es trotzdem in objectPartMap/fixGroundedPart() und bekommt vom AEUSSEREN
        // kombinierten Solve bei JEDEM Aufruf eine neu berechnete, nie konvergierende Placement
        // direkt in SEINEM EIGENEN Dokument verpasst (Verstoss gegen die Regel "Original darf
        // nie veraendert werden", siehe requirement-original-template-plc-never-changed.md) -
        // sichtbar als minimales, aber bei jedem Solve weiterdriftendes Zittern.
        App::DocumentObject* nestedOrigin = nestedAssembly->Origin.getValue();
        bool hasOwnGround = false;
        // FCPROJECT-PATCH (2026-10-20, starre Gruppe ueber Baugruppengrenze): nur die echten
        // Erdungswurzeln der Unterbaugruppe, nicht ueber deren starre Gruppen verteilt - sonst
        // galten im Kontext von BG22 alle 21 Teile des Motors (BG112) als eigene Erdungen,
        // obwohl die eine echte Wurzel wegen der Anbindung ignoriert wird. Verteilt wird unten
        // ueber die Gruppen dieser Ebene (rigidMembersByRep, inkl. verschachtelter Gruppen).
        for (auto* g : nestedAssembly->getGroundedParts(nullptr, /*propagateRigid=*/false)) {
            if (g && g != nestedOrigin) {
                nestedCandidates.push_back({assembly, subIndex, g});
                hasOwnGround = true;
            }
        }
        // FCPROJECT-PATCH (2026-10-13, Regel C5 in docs/CONSTRAINTS.md): eine eingebaute flexible
        // Unterbaugruppe OHNE eigene Erdung wurde, solange sie unverbunden ist, gar nicht geloest
        // (ihre Gelenke fielen als unverbunden heraus) - neue oder verschobene Teile blieben
        // liegen, wo sie waren. Jetzt haelt ihr erstes Teil sie an ihrer Ablage-Position, genau
        // wie eine eigene Erdung: dieselbe Gruppierung unten verwirft diesen Anker, sobald ein
        // Gelenk die Unterbaugruppe mit der restlichen Baugruppe verbindet.
        if (!hasOwnGround) {
            for (auto* obj : nestedAssembly->Group.getValues()) {
                auto* objLink = freecad_cast<AssemblyLink*>(obj);
                const bool isPart = obj
                    && (obj->isDerivedFrom<App::Link>() || obj->isDerivedFrom<PartApp::Feature>()
                        || obj->isDerivedFrom<App::Part>())
                    && obj != nestedOrigin && !obj->isDerivedFrom<JointGroup>();
                if (isPart && (!objLink || objLink->isRigid()) && obj->getPlacementProperty()) {
                    nestedCandidates.push_back({assembly, subIndex, obj});
                    break;
                }
            }
        }
        ++subIndex;
    }

    // FCPROJECT-PATCH (2026-10-10, Drag-Start-Latenz): einen bereits laufenden
    // ScopedIdentityGraph mitbenutzen (z.B. aus getDownstreamParts()/getJointOfPartConnecting-
    // ToGround()), statt bei jedem Aufruf einen neuen Graphen aufzubauen.
    std::optional<IdentityGraph> ownGraph;
    IdentityGraph& graph = scopedGraph ? *scopedGraph : ownGraph.emplace(this);
    struct GroundRoot
    {
        App::DocumentObject* obj;
        App::DocumentObject* key;  // graph.materializeForObjectPartMap(...) = Solver-Koerper
        int source;  // -1 = lokale Wurzel dieser Ebene, sonst Index der Unterbaugruppe
    };
    std::vector<GroundRoot> roots;
    auto solverKey = [&graph](const IdentityHandle& h, App::DocumentObject* fallback) {
        App::DocumentObject* key = graph.materializeForObjectPartMap(h);
        return key ? key : fallback;
    };
    std::unordered_set<App::DocumentObject*> rootKeys;
    bool anyNestedRoot = false;
    if (!nestedCandidates.empty()) {
        for (auto* g : groundedSet) {
            App::DocumentObject* key = solverKey(graph.resolveObject(g), g);
            if (rootKeys.insert(key).second) {
                roots.push_back({g, key, -1});
            }
        }
    }
    for (const auto& candidate : nestedCandidates) {
        App::DocumentObject* g = candidate.obj;
        // 'g' liegt im Identitaetsraum der Unterbaugruppe - bei mehrfach eingefuegter
        // Unterbaugruppe fuer alle Instanzen derselbe Zeiger. Die Identitaet(en) DIESER Instanz
        // sind die Graph-Knoten mit derselben Vorlage, deren duplicateInstancePath diese
        // AssemblyLink enthaelt; ohne Duplikation genuegt resolveObject().
        // FCPROJECT-PATCH (2026-10-13): ohne Instanz-Kontext loeste resolveObject(g) auf die
        // erstbeste Instanz auf - bei einer Unterbaugruppe, die ueber VERSCHIEDENE Eltern im Baum
        // steckt (BG67 in BG73 und BG75), steht im duplicateInstancePath nicht dieser Link, und es
        // traf die falsche. Zuerst die Instanz unter genau diesem Einbau-Link suchen.
        App::DocumentObject* instance
            = findInstanceUnder(candidate.link->getLinkedAssembly(), candidate.link, g, 0);
        IdentityHandle base = graph.resolveObject(instance ? instance : g);
        std::vector<IdentityHandle> instanceHandles;
        for (const auto& edge : graph.jointEdges()) {
            for (const IdentityHandle* h : {&edge.a, &edge.b}) {
                if (h->templateObj == base.templateObj
                    && std::ranges::find(h->duplicateInstancePath, candidate.link)
                        != h->duplicateInstancePath.end()
                    && std::ranges::find(instanceHandles, *h) == instanceHandles.end()) {
                    instanceHandles.push_back(*h);
                }
            }
        }
        if (instanceHandles.empty()) {
            instanceHandles.push_back(std::move(base));
        }
        for (const auto& h : instanceHandles) {
            App::DocumentObject* key = solverKey(h, g);
            if (rootKeys.insert(key).second) {
                roots.push_back({g, key, candidate.subIndex});
                anyNestedRoot = true;
            }
        }
    }

    if (anyNestedRoot) {
        // Kanten einmal auf Solver-Koerper abbilden (nur verbindende Jointtypen).
        std::vector<std::pair<App::DocumentObject*, App::DocumentObject*>> bodyEdges;
        for (const auto& edge : graph.jointEdges()) {
            if (!isJointTypeConnecting(edge.joint)) {
                continue;
            }
            App::DocumentObject* a = graph.materializeForObjectPartMap(edge.a);
            App::DocumentObject* b = graph.materializeForObjectPartMap(edge.b);
            if (a && b) {
                bodyEdges.emplace_back(a, b);
            }
        }
        // Mitglieder einer starren Gruppe sind ebenso verbunden (gleiche Solver-Schluessel, siehe
        // rebuildRigidClusters()).
        for (const auto& [rep, members] : rigidMembersByRep) {
            for (auto* member : members) {
                if (member && member != rep) {
                    bodyEdges.emplace_back(rep, member);
                }
            }
        }

        std::vector<int> clusterOf(roots.size(), -1);
        for (std::size_t i = 0; i < roots.size(); ++i) {
            if (clusterOf[i] != -1) {
                continue;
            }
            // Breitensuche ueber die verbindenden Kanten des Graphen.
            std::unordered_set<App::DocumentObject*> reached {roots[i].key};
            std::vector<App::DocumentObject*> queue {roots[i].key};
            while (!queue.empty()) {
                App::DocumentObject* current = queue.back();
                queue.pop_back();
                for (const auto& [a, b] : bodyEdges) {
                    App::DocumentObject* other = a == current ? b : b == current ? a : nullptr;
                    if (other && reached.insert(other).second) {
                        queue.push_back(other);
                    }
                }
            }
            for (std::size_t k = i; k < roots.size(); ++k) {
                if (clusterOf[k] == -1 && reached.count(roots[k].key)) {
                    clusterOf[k] = static_cast<int>(i);
                }
            }
        }

        for (std::size_t i = 0; i < roots.size(); ++i) {
            if (clusterOf[i] != static_cast<int>(i)) {
                continue;  // nur einmal pro Cluster (i = erstes Mitglied)
            }
            // Massgebliche Quelle: lokal, falls vorhanden, sonst die erste Unterbaugruppe.
            int keepSource = std::numeric_limits<int>::max();
            for (std::size_t k = i; k < roots.size(); ++k) {
                if (clusterOf[k] == static_cast<int>(i)) {
                    keepSource = std::min(keepSource, roots[k].source);
                }
            }
            App::DocumentObject* winner = nullptr;
            for (std::size_t k = i; k < roots.size(); ++k) {
                if (clusterOf[k] == static_cast<int>(i) && roots[k].source == keepSource) {
                    winner = roots[k].obj;
                    break;
                }
            }
            for (std::size_t k = i; k < roots.size(); ++k) {
                if (clusterOf[k] != static_cast<int>(i)) {
                    continue;
                }
                if (roots[k].source == keepSource) {
                    // FCPROJECT-PATCH (2026-10-13): fuer eine verschachtelte Erdung die INSTANZ in
                    // diesem Kontext (key), nicht das Objekt aus dem Dokument der Unterbaugruppe -
                    // das wurde spaeter ohne Instanz-Kontext neu aufgeloest und landete bei einer
                    // Unterbaugruppe, die ueber mehrere Eltern im Baum steckt, wieder bei der
                    // falschen Instanz (BG73-Erdung beim Motor der BG75).
                    groundedSet.insert(roots[k].source >= 0 && roots[k].key ? roots[k].key : roots[k].obj);
                }
                else if (ignoredNestedRoots) {
                    ignoredNestedRoots->emplace_back(roots[k].obj, winner);
                }
            }
        }
    }

    // Propagate grounding through active rigid clusters.
    if (!propagateRigid) {
        return groundedSet;
    }
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

    // FCPROJECT-PATCH (2026-10-03, "BG43-Drift", live gefunden per ASMT-Export: 048 tauchte als
    // ZWEI Solver-Koerper auf - "043#048" geerdet aber an keinem Joint, "067#048" an allen
    // Motor-Joints aber nicht geerdet): getGroundedParts() liefert lokale Wurzeln als ROHE
    // Objekte, z.B. BG43s Spiegel von BG67s 048. Die Joints dagegen landen ueber
    // resolvePartForMbD() -> materializeForObjectPartMap() auf dem Solver-Koerper (hier: das
    // echte 048). Mit dem rohen Spiegel als Schluessel entstand ein zweiter, isolierter Koerper -
    // der Motor war im Solve in Wahrheit gar nicht verankert, das Ergebnis hing vom Startzustand
    // ab (Drift ueber mehrere Solves), und computeGroundCorrection() griff ebenfalls ins Leere.
    // Fix: jedes geerdete Objekt ueber DENSELBEN IdentityGraph-Weg auf seinen Solver-Koerper
    // abbilden wie die Joints; Ziel-Placement, Fixierung und Ground-Korrektur haengen damit am
    // selben Koerper. Mehrere Erdungen desselben Koerpers werden nur einmal fixiert.
    IdentityGraph graph(this);
    groundedBodies.clear();
    for (auto obj : groundedParts) {
        if (!obj) {
            continue;
        }

        App::DocumentObject* body = graph.materializeForObjectPartMap(graph.resolveObject(obj));
        if (!body) {
            body = obj;
        }
        if (!groundedBodies.insert(body).second) {
            continue;
        }

        Base::Placement plc = getPlacementFromProp(body, "Placement");
        Base::Placement groundContainerChainPlc = graph.containerChainPlacement(body);
        if (!groundContainerChainPlc.isIdentity()) {
            plc = groundContainerChainPlc * plc;
        }
        if (!groundedTargetObj && explicitlyGroundedTargets.count(obj)) {
            groundedTargetObj = body;
            groundedTargetPlc = plc;
        }
        std::string str = body->getFullName();
        fixGroundedPart(body, plc, str);
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

    // FCPROJECT-PATCH (2026-09-28, live am echten BG37->BG43(Flexibel)->BG67-Projekt gefunden,
    // "Solve failed: iterNo > iterMax" bei verschachtelter, nicht-rigider Unterbaugruppe ohne
    // eigenes verbindendes Gelenk): 'obj' kommt hier bereits als tief aufgeloestes, echtes Objekt
    // aus der rekursiven getGroundedParts() (siehe deren Kommentar) - ohne alreadyResolved=true
    // ueberspringt getMbDData() die containerChainPlacement()-Berechnung komplett (bleibt
    // Identity), das erzeugte MbD-Teil bekommt also die ROHE LOKALE Placement statt der
    // Welt-Position als Startwert. Da die Erdung VOR der Gelenkverarbeitung laeuft, wird dieser
    // falsche Eintrag in objectPartMap gecacht - JEDE spaetere Gelenkreferenz auf dasselbe Teil
    // (z.B. das geerdete Teil ist gleichzeitig auch an einem echten Gelenk beteiligt) findet den
    // bereits falsch platzierten Eintrag wieder, statt selbst korrekt neu aufzuloesen. Genau
    // dieser Startwert-Sprung (typischerweise um die volle Container-Kette daneben) reicht aus,
    // um Newton-Raphson im kombinierten Solve nicht mehr konvergieren zu lassen - waehrend ein
    // eigenstaendiger Solve DERSELBEN Unterbaugruppe (keine Container-Kette noetig) unveraendert
    // funktioniert.
    std::shared_ptr<ASMTPart> mbdPart = getMbDPart(obj, /*alreadyResolved=*/true);

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

bool AssemblyObject::isJointConnectingPartToGround(
    App::DocumentObject* joint,
    const char* propname,
    const std::string& nestingPrefix
)
{
    if (!joint || !isJointTypeConnecting(joint)) {
        return false;
    }

    // FCPROJECT-PATCH (2026-09-20, siehe getJointsOfPart()-Deklaration): resolvePartForMbD()
    // statt der adressierungsblinden getMovingPartFromRef() - fuer einen Subjoint einer
    // duplizierten flexiblen Unterbaugruppe (nestingPrefix != "") liefert die alte Funktion das
    // GETEILTE Template-Objekt statt der instanzeigenen Identitaet, wodurch die unten folgenden
    // isPartGrounded()/isPartConnected()-Aufrufe fuer KEINE der beiden Instanzen mehr zum
    // korrekten getJointsOfPart()-Ergebnis passen.
    App::DocumentObject* part = resolvePartForMbD(joint, propname, nestingPrefix);
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
    // FCPROJECT-PATCH (2026-10-04): Erreichbarkeit NUR ueber 'joint', gemessen an der JETZT
    // gueltigen Erdung - die anderen Joints des Teils werden beim Durchlaufen ausgeblendet statt per
    // Suppressed deaktiviert (siehe isPartConnectedExcluding()).
    const auto groundedObjs = getGroundedParts();
    std::vector<JointRef> otherJoints;
    for (auto& jri : getJointsOfPart(part)) {
        if (jri.joint && jri.joint->getFullName() != joint->getFullName()) {
            otherJoints.push_back(jri);
        }
    }
    return isPartConnectedExcluding(part, groundedObjs, otherJoints);
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
    Base::Console().log(
        "Assembly: removeUnconnectedJoints('{}') - {} geerdete(s) Teil(e), {} erreichbare(s) "
        "Teil(e), pruefe {} Joint(s).\n",
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
                    Base::Console().log(
                        "Assembly: removeUnconnectedJoints('{}') - Joint '{}' entfernt: "
                        "part1='{}' (erreichbar={}), part2='{}' (erreichbar={}).\n",
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
    // FCPROJECT-PATCH (2026-10-09, BG22-Leistung, gdb-Stichproben): bisher rief die Traversierung
    // fuer JEDES erreichte Teil getConnectedParts() auf, das jedes Mal ALLE Joints neu per
    // resolvePartForMbD() aufloeste - Teile x Joints Aufloesungen pro Aufruf. Das Baum-Overlay
    // ("Part_Detached") ruft isPartConnected() fuer jeden Joint im Baum auf; in BG22 lag damit rund
    // die Haelfte der Zeit nach dem Oeffnen in dieser Schleife. Jetzt wird jede Joint-Kante genau
    // einmal aufgeloest, danach nur noch die Nachbarschaftsliste durchlaufen. Reihenfolge und
    // Inhalt der Nachbarn wie in getConnectedParts() (Joint-Reihenfolge, dann rigide Mitglieder),
    // damit connectedParts dieselben ObjRefs in derselben Reihenfolge erhaelt.
    std::unordered_map<App::DocumentObject*, std::vector<ObjRef>> neighbors;
    for (const auto& jr : joints) {
        App::DocumentObject* joint = jr.joint;
        if (!isJointTypeConnecting(joint)) {
            continue;
        }
        App::DocumentObject* obj1 = resolvePartForMbD(joint, "Reference1", jr.nestingPrefix);
        App::DocumentObject* obj2 = resolvePartForMbD(joint, "Reference2", jr.nestingPrefix);
        if (!obj1 || !obj2) {
            continue;
        }
        auto* ref1 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
        auto* ref2 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
        if (ref2) {
            neighbors[obj1].push_back({obj2, ref2});
        }
        if (ref1 && obj2 != obj1) {
            neighbors[obj2].push_back({obj1, ref1});
        }
    }

    std::function<void(App::DocumentObject*)> visit = [&](App::DocumentObject* part) {
        std::vector<ObjRef> next;
        if (auto it = neighbors.find(part); it != neighbors.end()) {
            next = it->second;
        }
        if (const auto* members = getRigidMembers(part)) {
            for (auto* member : *members) {
                if (!member || member == part || isObjInSetOfObjRefs(member, next)) {
                    continue;
                }
                next.push_back({member, nullptr});
            }
        }
        for (auto& nextObjRef : next) {
            if (!isObjInSetOfObjRefs(nextObjRef.obj, connectedParts)) {
                connectedParts.push_back(nextObjRef);
                visit(nextObjRef.obj);
            }
        }
    };
    if (currentObj) {
        visit(currentObj);
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

// FCPROJECT-PATCH (2026-10-04, "Traeger reisst beim Ziehen aus BG37 heraus"): wie
// isPartConnected(), aber mit einer vom Aufrufer VORGEGEBENEN Erdung und ausgeblendeten Joints.
// isJointConnectingPartToGround() deaktivierte bisher alle anderen Joints des Teils per Suppressed
// und fragte isPartConnected() - dabei (a) aenderte es kurzzeitig Joints in fremden Dokumenten
// (Originale einer Unterbaugruppe) und (b) rechnete isPartConnected() die Erdung NEU: ein von der
// aeusseren Erdung abgetrenntes Teil einer flexiblen Unterbaugruppe bekam dadurch die EIGENE Erdung
// seiner Unterbaugruppe zurueck (getGroundedParts(), "Erdung ist Kontext-Eigenschaft") und galt
// faelschlich als verbunden - jeder beliebige Joint wurde so als "verbindet zur Erdung" gemeldet.
bool AssemblyObject::isPartConnectedExcluding(
    App::DocumentObject* obj,
    const std::unordered_set<App::DocumentObject*>& groundedObjs,
    const std::vector<JointRef>& excludedJoints
)
{
    if (!obj) {
        return false;
    }
    ScopedIdentityGraph graphScope(this, scopedGraph);

    App::DocumentObject* canonicalObj = canonicalizeForMbD(obj);

    std::vector<JointRef> joints;
    for (const auto& jr : getJoints()) {
        const bool excluded = std::ranges::any_of(excludedJoints, [&jr](const JointRef& e) {
            return e.joint == jr.joint && e.nestingPrefix == jr.nestingPrefix;
        });
        if (!excluded) {
            joints.push_back(jr);
        }
    }

    std::vector<ObjRef> connectedParts;
    std::unordered_set<App::DocumentObject*> canonicalGroundedObjs;
    for (auto* groundedObj : groundedObjs) {
        if (auto* canonical = canonicalizeForMbD(groundedObj)) {
            canonicalGroundedObjs.insert(canonical);
        }
    }
    for (auto* groundedObj : canonicalGroundedObjs) {
        connectedParts.push_back({groundedObj, nullptr});
    }
    for (auto* groundedObj : canonicalGroundedObjs) {
        traverseAndMarkConnectedParts(groundedObj, connectedParts, joints);
    }
    return std::ranges::any_of(connectedParts, [canonicalObj](const ObjRef& r) {
        return r.obj == canonicalObj;
    });
}

bool AssemblyObject::isPartConnected(App::DocumentObject* obj, bool verboseLog)
{
    if (!obj) {
        return false;
    }
    ScopedIdentityGraph graphScope(this, scopedGraph);

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
    const auto connected = getConnectedPartSet();

    if (verboseLog) {
        std::string names;
        for (auto* part : connected) {
            if (!names.empty()) {
                names += ", ";
            }
            names += part->getFullName();
        }
        Base::Console().log(
            "FCPROJECT-DEBUG isPartConnected: obj='{}' canonicalObj='{}' -> connectedParts=[{}] -> {}\n",
            obj->getFullName().c_str(),
            canonicalObj ? canonicalObj->getFullName().c_str() : "<null>",
            names.c_str(),
            connected.contains(canonicalObj) ? "TRUE" : "FALSE"
        );
    }
    return connected.contains(canonicalObj);
}

std::unordered_set<App::DocumentObject*> AssemblyObject::getConnectedPartSet()
{
    ScopedIdentityGraph graphScope(this, scopedGraph);
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

    std::unordered_set<App::DocumentObject*> result;
    for (auto& objRef : connectedParts) {
        if (objRef.obj) {
            result.insert(objRef.obj);
        }
    }
    return result;
}

std::string AssemblyObject::exportIdentityGraphDot()
{
    IdentityGraph graph(this);
    return graph.exportDot();
}

std::string AssemblyObject::verifyIdentityGraphAgainstAsmt()
{
    IdentityGraph graph(this);
    return graph.verifyAgainstAsmt();
}

std::vector<std::string> AssemblyObject::verifyIdentityGraphHandleConsistency()
{
    IdentityGraph graph(this);
    return graph.verifyHandleConsistency();
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
        setGearJointCarrierMarkerIfAvailable(this, joint, mbdJoint, nestingPrefix);
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
                if (minEnabled || maxEnabled) {
                    activeLimits.push_back(
                        {joint, nestingPrefix, false, minEnabled, maxEnabled, minLength, maxLength}
                    );
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
                if (minEnabled || maxEnabled) {
                    activeLimits.push_back(
                        {joint, nestingPrefix, true, minEnabled, maxEnabled, minAngle, maxAngle}
                    );
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

    // FCPROJECT-PATCH (2026-10-10, Plan plans/2026-10-10-simulation-an-struktur-anpassen.md,
    // Schritt 1): "initialValue" ist der AKTUELLE Gelenkwert in DIESEM Kontext. Bisher kam er aus
    // getJointCurrentValue(), das ueber getGlobalPlacement(nullptr, ref) die ORIGINALE im
    // Dokument des Gelenks misst - im Kontext einer aeusseren BG gilt aber der Stand der Spiegel
    // (Regel C2). Folge war ein Sprung beim ersten Simulationsbild (BG43: -30 Grad). Jetzt wie
    // handleOneSideOfJoint() den Marker relativ zum Teil bestimmen und mit der Startlage des
    // MbD-Koerpers (Instanz samt Container-Kette, offsetPlc) in die Welt bringen.
    auto jcsInContext = [&](const char* refName,
                            const char* plcName) -> std::optional<Base::Placement> {
        return getJcsInContext(joint, refName, plcName, nestingPrefix);
    };

    auto replaceInitialValue =
        [&jcsInContext](std::string& form, App::DocumentObject* jnt, const std::string& mType) {
            if (form.find("initialValue") != std::string::npos) {
                const bool isAngle = mType == "Angular";
                const auto jcs1 = jcsInContext("Reference1", "Placement1");
                const auto jcs2 = jcsInContext("Reference2", "Placement2");
                double val = (jcs1 && jcs2) ? getJointValueFromJcs(*jcs1, *jcs2, isAngle)
                                            : getJointCurrentValue(jnt, isAngle);

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
        if (joint != motionJoint || !motionMatchesJointInstance(pJoint, nestingPrefix)) {
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
            if (joint != motionJoint || motion2 == motion
                || !motionMatchesJointInstance(pJoint, nestingPrefix)) {
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
            .warning("The property {} of Joint {} is bad.\n", propRefName, joint->getFullName());
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
    Base::Placement obj_global_plc = getJcsReferenceGlobalPlacement(ref);
    plc = obj_global_plc * plc;
    // Note part is supposed to be root of ref, so we could use part.Placement directly.
    // FCPROJECT-PATCH (2026-10-11, live an BG22 gefunden: BG75 mit starrem Motor 067 flexibel
    // eingebaut, Marker des Joints Motor<->Buchse 6mm/70 Grad daneben): bei einem verschachtelten
    // Joint ist part der Solver-Koerper in DIESEM Dokument (z.B. BG22s Spiegel 067_001), der
    // Referenzpfad des Joints liegt aber im Dokument des Joints (BG75s 067 -> 054). part kommt
    // darauf nicht vor; getPlacementOf() laeuft dann bis zum Ende des Pfads, und der Marker wird
    // relativ zum referenzierten Teil (Rotor 054) statt zum starren Koerper (067) gesetzt.
    // Bezugsrahmen ist deshalb das Gegenstueck von part auf dem Pfad: das Objekt mit derselben
    // Identitaet (AssemblyLink: dieselbe verlinkte Baugruppe, sonst dasselbe verlinkte Objekt).
    App::DocumentObject* placementTarget = part;
    if (App::DocumentObject* refRoot = ref->getValue(); refRoot && !ref->getSubValues().empty()) {
        const std::vector<App::DocumentObject*> refPath
            = refRoot->getSubObjectList(ref->getSubValues()[0].c_str());
        if (std::find(refPath.begin(), refPath.end(), part) == refPath.end()) {
            auto identityOf = [](App::DocumentObject* o) -> App::DocumentObject* {
                if (auto* asmLink = freecad_cast<AssemblyLink*>(o)) {
                    return asmLink->getLinkedAssembly();
                }
                return o->getLinkedObject(true);
            };
            App::DocumentObject* partIdentity = identityOf(part);
            for (auto* pathObj : refPath) {
                if (pathObj && partIdentity && identityOf(pathObj) == partIdentity) {
                    placementTarget = pathObj;
                    break;
                }
            }
        }
    }
    Base::Placement part_global_plc = getGlobalPlacement(placementTarget, ref);
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
        Base::Console().warning("Reference1 of Joint {} is bad.\n", joint->getFullName());
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
    Base::Rotation adjustedRotation = yawRotation * rot;
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
    App::DocumentObject* part2 = getMovingPartFromRef(joint, "Reference2");

    // Compare the JCS in global coordinates: the slider and this joint may reference
    // different sub-objects of the same part.
    Base::Placement plc1 = getJointSideGlobalPlacement(joint, "Reference1", "Placement1");
    Base::Placement plc2 = getJointSideGlobalPlacement(joint, "Reference2", "Placement2");

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
                plcjt = getJointSideGlobalPlacement(jt, "Reference1", "Placement1");
            }
            else if (jpart2 == part1 || jpart2 == part2) {
                found = (jpart2 == part1) ? 1 : 2;
                plci = (jpart2 == part1) ? plc1 : plc2;
                plcjt = getJointSideGlobalPlacement(jt, "Reference2", "Placement2");
            }

            if (found != 0) {
                // Check that the Z axes of plcjt and plci are parallel. Do not compare
                // pitch/roll: a yaw offset on the slider JCS (e.g. to align the rack) rotates
                // about its local Z axis, which keeps the axis but changes pitch and roll
                // whenever that axis is not the global Z.
                Base::Vector3d axisjt = plcjt.getRotation().multVec(Base::Vector3d(0, 0, 1));
                Base::Vector3d axisi = plci.getRotation().multVec(Base::Vector3d(0, 0, 1));
                if (axisjt.Cross(axisi).Length() < Precision::Confusion()) {
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
// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 3): Altimplementierung TEMPORAER als
// eigene Funktion erhalten, ausschliesslich fuer den Aequivalenz-Verifikationslauf gegen die
// neue, graphbasierte canonicalizeForMbD() (siehe deren neue Definition unten) - wird entfernt,
// sobald diese Phase per voller Testmatrix + Aequivalenzskript bestaetigt ist (siehe Plan-Datei).
App::DocumentObject* AssemblyObject::canonicalizeForMbDLegacy(App::DocumentObject* obj)
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
            return nested->canonicalizeForMbDLegacy(obj);
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
                    "Assembly: canonicalizeForMbD() - kein Quellobjekt fuer Spiegel '{}' in "
                    "objLinkMap von '{}' gefunden.\n",
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

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 3): objectPartMap ist (vor der fuer
// eine spaetere Phase geplanten vollstaendigen IdentityHandle-Umstellung) weiterhin nach rohem
// Zeiger geschluesselt - bei einer echten Instanz-Duplikation (duplicateInstancePath nicht
// leer) MUSS deshalb weiterhin ein PRO INSTANZ eindeutiger Zeiger materialisiert werden (sonst
// kollabieren zwei Duplikat-Instanzen auf denselben MbD-Teil, Bug C laesst gruessen) -
// graph.mirrorsOf() liefert genau diesen instanzeigenen lokalen Spiegel. Ohne Duplikation bleibt
// es beim tiefen ECHTEN Objekt, exakt wie "Adressieren statt Kopieren"
// (docs/ARCHITECTURE.md §5) es fuer den Solver will. Von canonicalizeForMbD() UND
// resolvePartForMbD() gemeinsam genutzt.
namespace
{
// FCPROJECT-PATCH (2026-09-26): eigentliche Logik nach IdentityGraph::materializeForObjectPartMap()
// verschoben (siehe dortiger Kommentar) - EINE gemeinsame Quelle der Wahrheit fuer diese Datei UND
// exportDot(). Duennes Wrapper hier belassen, um alle bestehenden Aufrufstellen unveraendert zu
// lassen.
App::DocumentObject* materializeForObjectPartMap(IdentityGraph& graph, const IdentityHandle& handle)
{
    return graph.materializeForObjectPartMap(handle);
}
}  // namespace

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 3, siehe
// /home/maxx/.claude/plans/enumerated-roaming-river.md): ersetzt die obige, handgeschriebene
// canonicalizeForMbDLegacy() durch den Graphen - IdentityGraph::resolveObject() bildet exakt
// denselben Top-Down-Struktursuche-Algorithmus nach (siehe dessen Kommentar in
// AssemblyIdentityGraph.cpp), aber tiefengenerell statt mit Bug-C's fruehem Aufgeben bei JEDER
// Instanz-Duplikation. Da canonicalizeForMbD() als "zentrale Wahrheitsquelle"
// (docs/ARCHITECTURE.md §4) von sehr vielen Stellen genutzt wird (rebuildRigidClusters(),
// getGroundedParts(), getConnectedParts()/traverseAndMarkConnectedParts(), hasRealObject(),
// isPartConnected(), ...), behebt dieser EINE Austausch §6s "beliebiger Anschlusspunkt"-
// Anforderung strukturell an ALLEN diesen Stellen gleichzeitig, ohne dass sie einzeln angefasst
// werden muessen - sie rufen bereits alle nur canonicalizeForMbD(obj) auf.
App::DocumentObject* AssemblyObject::canonicalizeForMbD(App::DocumentObject* obj)
{
    if (!obj) {
        return obj;
    }
    std::optional<IdentityGraph> ownGraph;
    IdentityGraph& graph = scopedGraph ? *scopedGraph : ownGraph.emplace(this);
    IdentityHandle handle = graph.resolveObject(obj);
    App::DocumentObject* materialized = materializeForObjectPartMap(graph, handle);
    return materialized ? materialized : obj;
}

// FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix, siehe
// patches/assembly-architecture-overview.md, Abschnitt "Teilschritt 2 - Umsetzung"): siehe
// ausfuehrliche Erklaerung (inkl. der bewussten Abweichung beim subPath - wird derzeit nirgends
// an getMbDData()/getMbDPart() weitergereicht) am Deklarationsort in AssemblyObject.h.
// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 1, siehe
// /home/maxx/.claude/plans/enumerated-roaming-river.md): auf IdentityGraph::resolveJointRef()
// umgestellt - behebt den live beobachteten BG37->BG43->BG67-Bug (resolveJointReference()
// uebersetzt laut eigenem Kommentar nur den letzten Sprung hinter einer Instanz-Duplikation;
// resolveJointRef() tut das tiefengenerell, INKLUSIVE des Falls, dass die Joint-Referenz direkt
// auf einen bereits verschachtelten lokalen Spiegel zeigt - siehe refineNestedMirrorTarget() in
// AssemblyIdentityGraph.cpp fuer die volle Herleitung). resolvePartForMbD() selbst muss dank
// dieser vollstaendigen Aufloesung im Graphen nur noch MATERIALISIEREN, nicht mehr selbst
// zweistufig nachkanonisieren wie im Altcode.
//
// objectPartMap ist (vor der fuer Phase 3 geplanten IdentityHandle-Umstellung) weiterhin nach
// rohem Zeiger geschluesselt - bei einer echten Instanz-Duplikation (duplicateInstancePath
// nicht leer) MUSS deshalb weiterhin ein PRO INSTANZ eindeutiger Zeiger zurueckgegeben werden
// (sonst kollabieren zwei Duplikat-Instanzen auf denselben MbD-Teil) - graph.mirrorsOf() liefert
// genau diesen instanzeigenen lokalen Spiegel. Ohne Duplikation bleibt es beim tiefen ECHTEN
// Objekt, exakt wie "Adressieren statt Kopieren" (docs/ARCHITECTURE.md §5) es fuer den Solver
// will.
App::DocumentObject* AssemblyObject::resolvePartForMbD(
    App::DocumentObject* joint,
    const char* propRefName,
    const std::string& nestingPrefix
)
{
    std::optional<IdentityGraph> ownGraph;
    IdentityGraph& graph = scopedGraph ? *scopedGraph : ownGraph.emplace(this);
    IdentityHandle handle = graph.resolveJointRef(joint, propRefName, nestingPrefix);
    if (handle.templateObj) {
        return materializeForObjectPartMap(graph, handle);
    }

    // Defensiver Ruecksfall: unveraendertes Altverhalten fuer jeden Fall, den
    // resolveJointRef() (noch) nicht abdeckt.
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
            "Assembly: Ignoring joint ({}) because its parts are connected by a fixed "
            "joint bundle. This joint is a conflicting or redundant constraint. "
            "[FCProject-Diagnose: Reference1 zeigt auf '{}' (Sub-Pfad '{}'), Reference2 auf "
            "'{}' (Sub-Pfad '{}') - falls die Sub-Pfade unterschiedlich sind, ist das "
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
        // FCPROJECT-PATCH (2026-10-20, starre Gruppe ueber Baugruppengrenze): Mitglieder einer
        // Gruppe aus einer flexiblen Unterbaugruppe liegen in deren Container, ihre Placement ist
        // relativ dazu. Koerper und Versaetze deshalb in Weltkoordinaten (Container-Kette wie im
        // normalen Pfad unten), setNewPlacements() rechnet ueber containerChainPlc zurueck. Fuer
        // Gruppen dieser Baugruppe selbst ist die Kette Identity, also unveraendert.
        std::optional<IdentityGraph> ownGraph;
        IdentityGraph& graph = scopedGraph ? *scopedGraph : ownGraph.emplace(this);
        auto worldOf = [&](App::DocumentObject* obj, Base::Placement& chain) {
            chain = graph.containerChainPlacement(obj);
            return chain * getPlacementFromProp(obj, "Placement");
        };
        Base::Placement repChain;
        Base::Placement repWorld = worldOf(rep, repChain);

        std::shared_ptr<ASMTPart> mbdPart;
        // Liegt der Repraesentant schon mit Versatz in einem (gebuendelten) Koerper, beziehen
        // sich die Mitglieder auf dessen Ursprung: Koerper = repWorld * offset^-1.
        Base::Placement bodyWorld = repWorld;
        const auto repMapped = objectPartMap.find(rep);
        if (repMapped != objectPartMap.end()) {
            mbdPart = repMapped->second.part;
            bodyWorld = repWorld * repMapped->second.offsetPlc.inverse();
        }
        else {
            std::string repName = rep->getFullName();
            mbdPart = makeMbdPart(repName, repWorld);
            mbdAssembly->addPart(mbdPart);
            objectPartMap[rep] = {mbdPart, Base::Placement(), repChain};
        }

        if (const auto* members = getRigidMembers(rep)) {
            for (auto* member : *members) {
                if (!member || objectPartMap.find(member) != objectPartMap.end()) {
                    continue;
                }

                Base::Placement memberChain;
                Base::Placement memberWorld = worldOf(member, memberChain);
                objectPartMap[member] = {mbdPart, bodyWorld.inverse() * memberWorld, memberChain};
            }
        }

        auto mapped = objectPartMap.find(part);
        if (mapped != objectPartMap.end()) {
            return mapped->second;
        }
    }

    // part has not been associated with an ASMTPart before
    std::string str = part->getFullName();

    // FCPROJECT-PATCH (2026-09-19, "zweite BG25-Instanz falsch eingesetzt", siehe
    // containerChainPlc-Deklaration in AssemblyObject.h fuer die volle Begruendung): 'part' kann
    // hier ein bewusst NICHT vollstaendig kanonisiertes, instanzeigenes Spiegelobjekt sein
    // (alreadyResolved=true, Bug C) ODER (seit dem IdentityGraph-Umbau) ein tief verschachteltes,
    // ueber MEHRERE ECHTE Dokumentgrenzen erreichtes echtes Objekt - deren Placement wird
    // aufmultipliziert, um aus 'part's lokaler (Container-relativer) Placement dessen
    // tatsaechliche Weltposition zu machen. Fuer alle unveraenderten Faelle (part liegt nicht im
    // lokalen Baum dieser Instanz, z.B. weil es bereits vollstaendig auf ein echtes, flach
    // liegendes Objekt kanonisiert wurde) bleibt containerChainPlc Identity - dann ist plc wie
    // bisher einfach die rohe lokale Placement.
    //
    // FCPROJECT-PATCH (2026-09-19, live am echten BG37/BG43/BG67-Projekt gefunden): die
    // urspruengliche, rein lokale findLocalGroupPath()-Suche fand ein Objekt nur innerhalb DIESES
    // EINEN Dokuments - fuer einen Joint, der (seit resolveJointRef()'s tiefengenerellerer
    // Aufloesung, siehe refineNestedMirrorTarget()) direkt auf ein Objekt MEHRERE ECHTE
    // Dokumentgrenzen tiefer zeigt, blieb containerChainPlc faelschlich Identity - die
    // Weltposition wurde dadurch falsch berechnet, obwohl die Identitaet selbst (welches Objekt
    // gemeint ist) bereits korrekt war ("Baugruppe intern richtig, aber am falschen Platz").
    // graph.containerChainPlacement() ersetzt das durch eine tiefengenerelle, rekursiv durch
    // beliebig viele echte Dokumentgrenzen absteigende Suche (siehe deren Definition in
    // AssemblyIdentityGraph.cpp).
    Base::Placement containerChainPlc;
    if (alreadyResolved) {
        std::optional<IdentityGraph> ownGraph;
        IdentityGraph& graph = scopedGraph ? *scopedGraph : ownGraph.emplace(this);
        containerChainPlc = graph.containerChainPlacement(part);
    }
    Base::Placement localPlc = getPlacementFromProp(part, "Placement");
    Base::Placement plc = containerChainPlc.isIdentity() ? localPlc : containerChainPlc * localPlc;
    std::shared_ptr<ASMTPart> mbdPart = makeMbdPart(str, plc);
    mbdAssembly->addPart(mbdPart);
    MbDPartData data = {mbdPart, Base::Placement(), containerChainPlc};
    objectPartMap[part] = data;  // Store the association
    Base::Console().log(
        "FCPROJECT-DEBUG getMbDData: NEW mbdPart '{}' for key '{}'\n",
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
            std::vector<JointRef> joints = getJointsOfPart(currentPart);
            for (auto& jr : joints) {
                App::DocumentObject* joint = jr.joint;
                JointType jointType = getJointType(joint);
                if (jointType == JointType::Fixed) {
                    // FCPROJECT-PATCH (2026-09-20, siehe getJointsOfPart()-Deklaration):
                    // resolvePartForMbD() mit demselben nestingPrefix statt der
                    // adressierungsblinden getMovingPartFromRef() - dieselbe Duplikations-
                    // Luecke wie bei getJointOfPartConnectingToGround().
                    App::DocumentObject* part1 = resolvePartForMbD(joint, "Reference1", jr.nestingPrefix);
                    App::DocumentObject* part2 = resolvePartForMbD(joint, "Reference2", jr.nestingPrefix);
                    if (!part1 || !part2) {
                        continue;
                    }
                    App::DocumentObject* partToAdd = currentPart == part1 ? part2 : part1;

                    if (objectPartMap.find(partToAdd) != objectPartMap.end()) {
                        // already added
                        continue;
                    }

                    // FCPROJECT-PATCH (2026-10-11, BG75 "Halteplatte wird mitgezogen"): die hier
                    // bis dahin stehende Ausnahme (2026-09-20, 4299590bf8) - "nicht buendeln, wenn
                    // BEIDE Seiten noch ein eigenes nicht-starres Gelenk haben" - ist entfernt. Ein
                    // Fixed-Gelenk macht beide Teile ohnehin zu EINEM starren Koerper; das Buendeln
                    // aendert die Kinematik nicht, es macht sie nur robust. Die Ausnahme liess
                    // dagegen in BG75 die Halteplatte (Fixed am geerdeten Motor, Slider zur Scheibe)
                    // als eigenen Koerper stehen, der beim Ziehen der Scheibe bis an deren Anschlag
                    // nachgab (Joint003 um 12.8mm verletzt). Der damalige BG22-Fall (zwei fest
                    // verbundene Halterbaugruppen auf verschiedenen Schienen) war seither durch die
                    // Identitaets-/Drag-Fixes behoben; per Drag-Rundumtest ueber BG75/25/32/37/43/22
                    // ohne die Ausnahme nachgeprueft.

                    // FCPROJECT-PATCH (2026-09-20, "BG25 Slider-Drag Achse verschoben,
                    // Teil 2" - siehe todo-bg25-slider-drag-two-instances): 'partToAdd' kann,
                    // genau wie beim "neues Teil"-Zweig oben in getMbDData(), ein Bug-C-
                    // bewahrtes, verschachteltes Spiegelobjekt sein (z.B. Fuehrung331, innerhalb
                    // eines nicht an Identity stehenden Containers) - 'plci' (seine ROHE, lokale
                    // Placement) ist dann NICHT im selben (globalen) Koordinatenraum wie 'plc'
                    // (Rahmens eigene, bereits absolute Placement). Ohne diese Korrektur wich der
                    // hier berechnete offsetPlc um genau den Container-Versatz ab - sichtbar als
                    // Fehlausrichtung zwischen dem starr gebuendelten Fuehrung331 (dieser Zweig)
                    // und dem separat, MIT containerChainPlc registrierten Halterbaugruppe003
                    // (Slider-Gegenstueck) - beide MUESSEN relativ zueinander exakt fluchten.
                    // FCPROJECT-PATCH (2026-09-19, live am echten BG37/BG43/BG67-Projekt
                    // gefunden - siehe dieselbe Korrektur bei getMbDData()'s "neues Teil"-Zweig
                    // oben): graph.containerChainPlacement() statt der rein lokalen
                    // findLocalGroupPath()-Suche - tiefengenerell durch beliebig viele echte
                    // Dokumentgrenzen.
                    std::optional<IdentityGraph> ownGraph;
                    IdentityGraph& graph = scopedGraph ? *scopedGraph : ownGraph.emplace(this);
                    Base::Placement partToAddContainerChainPlc = graph.containerChainPlacement(partToAdd);
                    Base::Placement plciLocal = getPlacementFromProp(partToAdd, "Placement");
                    Base::Placement plci = partToAddContainerChainPlc.isIdentity()
                        ? plciLocal
                        : partToAddContainerChainPlc * plciLocal;
                    MbDPartData partData
                        = {mbdPart, plc.inverse() * plci, partToAddContainerChainPlc};
                    objectPartMap[partToAdd] = partData;  // Store the association
                    Base::Console().log(
                        "FCPROJECT-DEBUG getMbDData: BUNDLE-FIXED key '{}' -> mbdPart '{}' (via joint '{}')\n",
                        partToAdd->getFullName().c_str(),
                        mbdPart->name.c_str(),
                        joint->getFullName().c_str()
                    );

                    // FCPROJECT-PATCH (2026-10-20, starre Gruppe ueber Baugruppengrenze, BG22):
                    // gehoert partToAdd zu einer starren Gruppe, kommt die ganze Gruppe in diesen
                    // Koerper - sonst steckte das per Fixed angebundene Teil (Motor-Teil 091 am
                    // Spindelhalter) im gebuendelten Koerper und der Rest der Gruppe blieb ein
                    // eigener, loser Koerper, der sich beim Ziehen neu anordnete.
                    std::vector<App::DocumentObject*> added {partToAdd};
                    if (const auto* members = getRigidMembers(partToAdd)) {
                        for (auto* member : *members) {
                            if (!member || objectPartMap.find(member) != objectPartMap.end()) {
                                continue;
                            }
                            Base::Placement memberChain = graph.containerChainPlacement(member);
                            Base::Placement memberWorld
                                = memberChain * getPlacementFromProp(member, "Placement");
                            objectPartMap[member] = {mbdPart, plc.inverse() * memberWorld, memberChain};
                            added.push_back(member);
                        }
                    }

                    // Recursively call for partToAdd (and the rest of its rigid group)
                    for (auto* addedPart : added) {
                        self(addedPart, self);
                    }
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
    App::DocumentObject* joint,
    const std::vector<App::DocumentObject*>& alsoIgnore
)
{
    if (!part) {
        return {};
    }

    // FCPROJECT-PATCH (2026-09-29, BG43/BG47 "Buchse loest sich vom Rotor" beim Drag): 'part'
    // kommt von Aufrufern wie getUpstreamMovingPart() bewusst als UI-/gerendertes Objekt herein
    // (siehe dortiger Kommentar - resolveForUi() statt resolveForSolver()), waehrend
    // getConnectedParts() seine Kanten ueber resolvePartForMbD() bildet (kanonische Identitaet,
    // z.B. bei einem direkt referenzierten Rotor der lokale Spiegel in BG43 vs. das kanonische
    // Original in der verschachtelten BG67). Ohne Kanonisierung des Startpunkts hier stimmt
    // 'part' (Vergleichswert in getConnectedParts()' `obj1 == part`) mit KEINEM Joint-Endpunkt
    // ueberein, sobald der Startteil bereits im ALLERERSTEN Joint direkt (nicht erst tiefer in
    // der Traversal) referenziert wird - die Traversal liefert dann faelschlich eine leere
    // Downstream-Liste, obwohl echte, erreichbare Nachbarn existieren (live an BG47s "StarrerVerbund"-
    // Fixed-Joint zum Rotor gefunden: getDownstreamParts(rotor, rotorsEigenesRevolute) lieferte []
    // statt [BG47]). isPartConnected() (dieselbe Traversal-Familie) kanonisiert seinen Eingabewert
    // bereits genauso - hier fehlte die analoge Behandlung.
    App::DocumentObject* canonicalPart = canonicalizeForMbD(part);
    if (!canonicalPart) {
        canonicalPart = part;
    }

    // FCPROJECT-PATCH (2026-10-10, Regel C3 "Erdung ist Kontext-Eigenschaft"): die Erdung VOR dem
    // Deaktivieren des Joints bestimmen. Danach berechnet, fiel eine flexible Unterbaugruppe, die
    // nur ueber diesen Joint mit der aeusseren Erdung verbunden ist, auf ihre EIGENE Erdung zurueck
    // (getGroundedParts(): unverbundene Unter-BG behaelt ihren Anker) und galt damit als
    // "verbunden" - Beispiel BG43: Rotor drehen, Buchse/Gewindestange (BG47 mit eigener Erdung)
    // wurden nicht als mitzuziehend erkannt. Gleiches Prinzip wie isPartConnectedExcluding().
    const auto contextGroundedObjs = getGroundedParts();

    // First we deactivate the joint
    bool state = false;
    if (joint) {
        state = getJointActivated(joint);
        setJointActivated(joint, false);
    }
    std::vector<std::pair<App::DocumentObject*, bool>> alsoIgnoreStates;
    for (auto* ignored : alsoIgnore) {
        if (ignored && ignored != joint) {
            alsoIgnoreStates.emplace_back(ignored, getJointActivated(ignored));
            setJointActivated(ignored, false);
        }
    }

    // FCPROJECT-PATCH (2026-10-10, Drag-Start-Latenz): ein gemeinsamer IdentityGraph fuer alle
    // Aufloesungen unten (erst NACH dem Deaktivieren des Joints angelegt).
    ScopedIdentityGraph graphScope(this, scopedGraph);

    // FCPROJECT-PATCH (Mehrfachinstanz-Fix): direkt der
    // JointRef-Vektor statt extractJointObjects() - siehe isPartConnected() fuer die Begruendung.
    std::vector<JointRef> joints = getJoints();

    std::vector<ObjRef> connectedParts = {{canonicalPart, nullptr}};
    traverseAndMarkConnectedParts(canonicalPart, connectedParts, joints);

    // FCPROJECT-PATCH (2026-10-10, Drag-Start-Latenz): vorher isPartConnected() je gefundenem
    // Teil - jedes Mal eine komplette Suche ab Erdung (plus getGroundedParts()/getJoints()).
    // Dieselbe Suche jetzt EINMAL, danach nur nachschlagen; Ergebnis identisch, da
    // isPartConnected() genau diese Erdung, diese Joints und diese Traversierung verwendet.
    std::vector<ObjRef> groundConnected;
    {
        std::unordered_set<App::DocumentObject*> canonicalGrounded;
        for (auto* groundedObj : contextGroundedObjs) {
            if (auto* canonical = canonicalizeForMbD(groundedObj)) {
                canonicalGrounded.insert(canonical);
            }
        }
        for (auto* groundedObj : canonicalGrounded) {
            groundConnected.push_back({groundedObj, nullptr});
        }
        for (auto* groundedObj : canonicalGrounded) {
            traverseAndMarkConnectedParts(groundedObj, groundConnected, joints);
        }
    }

    std::vector<ObjRef> downstreamParts;
    for (auto& parti : connectedParts) {
        if (parti.obj != canonicalPart
            && !isObjInSetOfObjRefs(canonicalizeForMbD(parti.obj), groundConnected)) {
            downstreamParts.push_back(parti);
        }
    }

    for (const auto& [ignored, ignoredState] : alsoIgnoreStates) {
        setJointActivated(ignored, ignoredState);
    }
    if (joint) {
        setJointActivated(joint, state);
    }

    return downstreamParts;
}

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 2, siehe
// /home/maxx/.claude/plans/enumerated-roaming-river.md): auf IdentityGraph::resolveJointRef()
// umgestellt - die adressierungsblinde getMovingPartFromRef() war die letzte verbliebene Stelle
// im Drag-Modus-Erkennungspfad (siehe ViewProviderAssembly.cpp's Aufrufstelle), die NIE auf
// resolvePartForMbD()/den Graphen umgestellt wurde (unmigrierter Rest derselben Bug-Klasse wie
// die bereits gefixten BG25-Slider-Drag-Stellen). resolveForUi()/materialize() statt
// resolveForSolver(), weil das Ergebnis hier als tatsaechlich zu ziehendes, gerendertes Objekt
// weiterverwendet wird (ViewProviderAssembly.cpp liest direkt dessen Placement-Property) - nicht
// als Solver-Identitaet.
//
// Seit 2026-10-04 mit dem Verschachtelungspfad des gefundenen Joints (frueher bewusst leer, siehe
// Kommentar im Funktionsrumpf) - getJointOfPartConnectingToGround() gibt ihn dafuer zurueck.
App::DocumentObject* AssemblyObject::getUpstreamMovingPart(
    App::DocumentObject* part,
    App::DocumentObject*& joint,
    std::string& name,
    std::vector<App::DocumentObject*> excludeJoints
)
{
    // FCPROJECT-PATCH (2026-10-10, Drag-Start-Latenz): ein gemeinsamer IdentityGraph fuer die
    // ganze (rekursive) Suche entlang der Fixed-Kette.
    ScopedIdentityGraph graphScope(this, scopedGraph);

    if (!part || isPartGrounded(part)) {
        return nullptr;
    }

    excludeJoints.push_back(joint);

    std::string nestingPrefix;
    joint = getJointOfPartConnectingToGround(part, name, excludeJoints, &nestingPrefix);
    JointType jointType = getJointType(joint);
    if (jointType != JointType::Fixed) {
        return part;
    }

    // FCPROJECT-PATCH (2026-10-04, "Traeger reisst beim Ziehen aus BG37 heraus"): die andere Seite
    // mit dem Verschachtelungspfad DIESES Joints aufloesen (bisher bewusst leer) - ein Fixed-Joint
    // INNERHALB einer flexiblen Unterbaugruppe (z.B. BG37: Traeger <-> LM10UU) wurde sonst ohne
    // Pfad falsch aufgeloest, die Fixed-Kette brach ab, findDragMode() fand keinen beweglichen Joint
    // und das Teil liess sich frei aus der Unterbaugruppe ziehen. resolvePartForMbD() liefert, wie
    // beim Solver, die Instanz in diesem Kontext.
    const char* otherProp = (name == "Reference1") ? "Reference2" : "Reference1";
    App::DocumentObject* other = resolvePartForMbD(joint, otherProp, nestingPrefix);
    part = other ? other : getMovingPartFromRef(joint, otherProp);

    return getUpstreamMovingPart(part, joint, name);
}

// FCPROJECT-PATCH (2026-10-11/12, live an BG22 gefunden): welches Gelenk bestimmt die Zieh-Art,
// wenn ein Teil ueber MEHRERE Gelenke mit Masse verbunden ist? getJointOfPartConnectingToGround()
// liefert das erste - das passte zweimal nicht:
//   - Halteplatte (BG75): erstes Gelenk Distance zum Rahmen (kein Zieh-Modus, nur freies
//     Verschieben); die Drehung um die Gewindestange gibt die Zylinderfuehrung vor, ueber Fixed.
//   - Traeger (BG37): erstes Gelenk die Zylinderfuehrung zur BG75-Stange (Verschieben UND
//     Drehen); der Slider an der Fuehrung (ueber Fixed) sperrt aber jede Drehung. Die Maus gab
//     trotzdem Drehziele vor, ab 90 Grad sprang der Traeger auf die zweite Slider-Loesung (180
//     Grad um die eigene Achse, #32477).
// Regel: das Teil muss ALLE seine Verbindungen zur Masse erfuellen; am naechsten an seiner
// tatsaechlichen Bewegung ist das Bewegungsgelenk mit den WENIGSTEN Freiheitsgraden. Fixed wird
// dabei ueber die starre Kette verfolgt (gleiche Regel auf jeder Ebene). Gleichstand: das zuerst
// gefundene (bisheriges Verhalten). Alle uebrigen gefundenen Verbindungen liefert bypassed - sie
// zaehlen beim Bestimmen der mitbewegten Teile nicht als Erdung (getDownstreamParts()).
namespace
{
int dragDegreesOfFreedom(JointType type)
{
    switch (type) {
        case JointType::Revolute:
        case JointType::Slider:
            return 1;
        case JointType::Cylindrical:
            return 2;
        case JointType::Ball:
            return 3;
        default:
            return 6;  // kein eigener Zieh-Modus (Distance, Parallel, ...)
    }
}
}  // namespace

AssemblyObject::DragChoice AssemblyObject::findDragChoice(
    App::DocumentObject* part,
    const std::vector<App::DocumentObject*>& excludeJoints,
    std::set<App::DocumentObject*>& visited,
    std::vector<App::DocumentObject*>& found
)
{
    DragChoice best;
    if (!part || isPartGrounded(part) || !visited.insert(part).second) {
        return best;
    }

    std::vector<App::DocumentObject*> exclude = excludeJoints;
    constexpr int maxJointsPerPart = 64;
    for (int i = 0; i < maxJointsPerPart; ++i) {
        std::string name;
        std::string nestingPrefix;
        App::DocumentObject* joint = getJointOfPartConnectingToGround(part, name, exclude, &nestingPrefix);
        if (!joint) {
            break;
        }
        exclude.push_back(joint);
        found.push_back(joint);

        DragChoice candidate;
        if (getJointType(joint) == JointType::Fixed) {
            const char* otherProp = (name == "Reference1") ? "Reference2" : "Reference1";
            App::DocumentObject* other = resolvePartForMbD(joint, otherProp, nestingPrefix);
            if (!other) {
                other = getMovingPartFromRef(joint, otherProp);
            }
            candidate = findDragChoice(other, {joint}, visited, found);
            if (candidate.joint) {
                candidate.chain.insert(candidate.chain.begin(), joint);
            }
        }
        else {
            candidate.part = part;
            candidate.joint = joint;
            candidate.name = name;
            candidate.dof = dragDegreesOfFreedom(getJointType(joint));
            candidate.chain = {joint};
        }
        if (candidate.joint && candidate.dof < best.dof) {
            best = std::move(candidate);
        }
    }
    return best;
}

App::DocumentObject* AssemblyObject::getDragJointOfPart(
    App::DocumentObject* part,
    App::DocumentObject*& joint,
    std::string& name,
    std::vector<App::DocumentObject*>& bypassed
)
{
    joint = nullptr;
    bypassed.clear();
    ScopedIdentityGraph graphScope(this, scopedGraph);

    std::set<App::DocumentObject*> visited;
    std::vector<App::DocumentObject*> found;
    DragChoice best = findDragChoice(part, {}, visited, found);
    if (!best.joint) {
        return nullptr;
    }
    for (auto* j : found) {
        if (std::find(best.chain.begin(), best.chain.end(), j) == best.chain.end()
            && std::find(bypassed.begin(), bypassed.end(), j) == bypassed.end()) {
            bypassed.push_back(j);
        }
    }
    joint = best.joint;
    name = best.name;
    return best.part;
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
    // FCPROJECT-PATCH (2026-10-09, BG22-Leistung, gdb-Stichproben): vorher durchsuchte jeder
    // Aufruf das ganze Dokument (getObjectsOfType) und pruefte je Treffer per hasObject() die
    // Group - im Overlay-/IdentityGraph-Pfad tausendfach pro Baum-Aktualisierung. Jetzt direkt
    // ueber die eigene Group. Reihenfolge wie bisher die Dokument-Reihenfolge (getObjectsOfType
    // liefert in Anlege-Reihenfolge, getID() steigt mit jedem angelegten Objekt), weil
    // Aufrufer wie getGroundedParts() bei Konflikten die "erste" Unterbaugruppe waehlen.
    std::vector<AssemblyLink*> subAssemblies;
    for (auto* obj : Group.getValues()) {
        if (auto* asmLink = freecad_cast<AssemblyLink*>(obj)) {
            subAssemblies.push_back(asmLink);
        }
    }
    std::ranges::sort(subAssemblies, {}, [](const AssemblyLink* l) { return l->getID(); });

    return subAssemblies;
}

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 0): siehe ausfuehrliche Erklaerung am
// Deklarationsort in AssemblyObject.h. Rein diagnostisch, temporaer fuer die
// Verifikations-Testmatrix (siehe /home/maxx/.claude/plans/enumerated-roaming-river.md).
std::vector<std::string> AssemblyObject::verifyIdentityGraphEquivalence()
{
    std::vector<std::string> report;
    IdentityGraph graph(this);

    auto describe = [](App::DocumentObject* obj) -> std::string {
        return obj ? obj->getFullName() : std::string("<null>");
    };

    auto classify = [&](
                         const std::string& label,
                         App::DocumentObject* oldResolved,
                         App::DocumentObject* newResolved,
                         bool graphSawDuplication,
                         bool oldSawDuplication
                     ) {
        if (newResolved == oldResolved) {
            report.push_back("MATCH " + label + " -> '" + describe(newResolved) + "'");
        }
        else if (graphSawDuplication || oldSawDuplication) {
            // Erwartete Abweichung: canonicalizeForMbD()/resolveJointReference() geben bei
            // Instanz-Duplikation bewusst frueh auf (siehe deren Kommentare), waehrend
            // IdentityGraph tiefengenerell weiteruebersetzt - siehe Plan-Datei fuer die
            // Herleitung, warum das die eigentliche Fehlerbehebung ist.
            report.push_back(
                "DIVERGENCE (by design, dupliziert) " + label + ": alt='" + describe(oldResolved)
                + "' neu='" + describe(newResolved) + "'"
            );
        }
        else {
            report.push_back(
                "MISMATCH " + label + ": alt='" + describe(oldResolved) + "' neu='"
                + describe(newResolved) + "'"
            );
        }
    };

    for (auto* obj : getGroundedParts()) {
        // FCPROJECT-PATCH (2026-09-19, Phase 3): 'newResolved' ruft die ECHTE, jetzt
        // graphbasierte canonicalizeForMbD() direkt auf (nicht nur graph.resolveObject()+
        // materialize(resolveForSolver()), das den mirrorsOf()-Fallback bei Duplikation
        // NICHT mitmacht) - sonst vergleicht dieser Test etwas anderes als das, was
        // tatsaechlich produktiv laeuft.
        App::DocumentObject* oldResolved = canonicalizeForMbDLegacy(obj);
        App::DocumentObject* newResolved = canonicalizeForMbD(obj);
        IdentityHandle handle = graph.resolveObject(obj);
        classify(
            "grounded '" + describe(obj) + "'",
            oldResolved,
            newResolved,
            !handle.duplicateInstancePath.empty(),
            false
        );
    }

    for (const auto& jointRef : getJoints(false, true, false)) {
        for (const char* propName : {"Reference1", "Reference2"}) {
            ResolvedJointRef oldRef
                = resolveJointReference(this, jointRef.joint, propName, jointRef.nestingPrefix);
            if (!oldRef.obj) {
                // Alte Funktion liefert hier ohnehin nichts (z.B. Joint-Typ ohne diese
                // Referenz) - kein sinnvoller Vergleichspunkt.
                continue;
            }
            // FCPROJECT-PATCH (2026-09-19): fairer Vergleich ist nicht die rohe
            // resolveJointReference()-Rueckgabe allein, sondern was resolvePartForMbD() VOR
            // Phase 1 daraus gemacht hat (resolvedViaInstanceMirror unveraendert lassen, sonst
            // per canonicalizeForMbD() weiterkanonisieren) - graph.resolveJointRef() macht
            // dieselbe Nachbehandlung inzwischen INTERN (siehe refineNestedMirrorTarget()), die
            // rohe Altfunktion allein tut das nie.
            App::DocumentObject* oldResolved = oldRef.resolvedViaInstanceMirror
                ? oldRef.obj
                : canonicalizeForMbDLegacy(oldRef.obj);
            IdentityHandle handle
                = graph.resolveJointRef(jointRef.joint, propName, jointRef.nestingPrefix);
            App::DocumentObject* newResolved = graph.materialize(graph.resolveForSolver(handle));
            classify(
                "joint '" + describe(jointRef.joint) + "'." + propName + " (nestingPrefix='"
                    + jointRef.nestingPrefix + "')",
                oldResolved,
                newResolved,
                !handle.duplicateInstancePath.empty(),
                oldRef.resolvedViaInstanceMirror
            );
        }
    }

    return report;
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

    // FCPROJECT-PATCH (2026-09-27, siehe collectGroundedPartsRecursive()-Kommentar): NUR fuer die
    // Entscheidung "braucht dieses Teil einen NEUEN GroundedJoint" massgeblich - erfasst auch
    // Erdungen, die zwei oder mehr Ebenen tiefer in einer nicht-rigiden Unterbaugruppe bereits
    // existieren, damit hier keine redundante Zweit-Erdung fuer dasselbe physische Teil entsteht.
    std::unordered_set<App::DocumentObject*> groundedAnywhere;
    for (auto* subAssembly : getSubAssemblies()) {
        if (!subAssembly || subAssembly->isRigid()) {
            continue;
        }
        AssemblyObject* nested = subAssembly->getLinkedAssembly();
        if (!nested || nested == this) {
            continue;
        }
        collectGroundedPartsRecursive(nested, groundedAnywhere);
    }

    // FCPROJECT-PATCH (2026-10-03, "Erdung ist Kontext-Eigenschaft", Plan
    // plans/2026-10-03-erdung-als-kontext-eigenschaft.md): hier stand bis heute die
    // "Doppel-Anker-Abschaltung" (2026-09-29), die ueberzaehlige verschachtelte Erdungen durch
    // Loeschen des ReadOnly-Flags IM DOKUMENT DER UNTERBAUGRUPPE abschaltete - damit verlor die
    // Unterbaugruppe auch fuer sich allein ihre Erdung. Ersetzt durch die seiteneffektfreie
    // Cluster-Aufloesung in getGroundedParts(): keine Ebene veraendert mehr den Erdungszustand
    // eines Objekts in einem fremden Dokument.

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
        bool hasLocalJoint = (it != groundedMap.end());
        // FCPROJECT-PATCH (2026-09-27): fuer die Erzeugungs-Entscheidung zaehlt eine Erdung
        // AN JEDER TIEFE (lokal ODER zwei+ Ebenen tiefer in einer nicht-rigiden Unterbaugruppe) -
        // siehe collectGroundedPartsRecursive()-Kommentar.
        bool hasJoint = hasLocalJoint || groundedAnywhere.count(part) > 0;

        // Create grounding joint if placement is locked but no joint exists (anywhere in the
        // nesting chain).
        if (isReadOnly && !hasJoint) {
            Base::Console().log(
                "FCPROJECT-DEBUG: syncGroundedJoints('{}') legt NEUEN GroundedJoint an fuer "
                "part='{}' (Dokument '{}').\n",
                getFullName(),
                part->getNameInDocument(),
                part->getDocument() ? part->getDocument()->getName() : "<null>"
            );

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
        // FCPROJECT-PATCH (2026-10-02, live an BG43->BG47 gefunden, Nutzerauftrag "nichts
        // loeschen, deaktivieren"): frueher wurde ein GroundedJoint, dessen Ziel nicht (mehr)
        // ReadOnly ist, hier nach zwei bestaetigenden solve()-Aufrufen geloescht. Das widersprach
        // der Regel direkt oberhalb in dieser Funktion (Doppel-Anker-Erkennung: "nie loeschen,
        // nur abschalten") - sobald die Doppel-Anker-Erkennung ein verschachteltes GroundedJoint
        // deaktiviert (ReadOnly=false setzt), loeschte dieser Zweig es beim naechsten eigenen
        // solve() der betroffenen Unterbaugruppe wieder permanent (live reproduziert: BG47s
        // eigenes GroundedJoint verschwand nach zwei solve()-Aufrufen auf BG47 selbst). Ein
        // GroundedJoint, dessen Ziel nicht ReadOnly ist, hat ohnehin keine Wirkung auf den Solve -
        // es muss nicht zusaetzlich aus dem Dokument entfernt werden, weder wenn die Entsperrung
        // von der Doppel-Anker-Erkennung noch wenn sie von einem manuellen Rechtsklick des
        // Nutzers stammt. Kein Loesch-Zweig mehr noetig.
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
