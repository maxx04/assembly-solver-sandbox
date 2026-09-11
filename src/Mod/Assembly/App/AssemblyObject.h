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


#pragma once

#include <boost/signals2.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

#include <App/FeaturePython.h>
#include <App/Part.h>
#include <App/PropertyLinks.h>

#include <OndselSolver/enum.h>

namespace MbD
{
class ASMTPart;
class ASMTAssembly;
class ASMTJoint;
class ASMTMarker;
class ASMTPart;
}  // namespace MbD

namespace App
{
class PropertyXLinkSub;
}  // namespace App

namespace Base
{
class Placement;
class Rotation;
}  // namespace Base


namespace Assembly
{

class AssemblyLink;
class JointGroup;
class ViewGroup;
enum class JointType;


struct ObjRef
{
    App::DocumentObject* obj;
    App::PropertyXLinkSub* ref;
};

// FCPROJECT-PATCH (Migrationsschritt 3 "Adressieren statt Kopieren", siehe docs/ARCHITECTURE.md
// Abschnitt 5): Ersatz fuer die bisherige, separate jointNestingPrefixMap - getJoints() liefert
// das nestingPrefix jetzt direkt im Rueckgabewert, statt es parallel in einer Member-Map
// nachzuschlagen. Bewusst KEIN Missbrauch von AssemblyUtils::ResolvedJointRef: dessen subPath
// bedeutet etwas anderes (der nach der Teile-Aufloesung verbleibende Geometrie-Rest, z.B.
// "Face1"), waehrend nestingPrefix eine Punkt-getrennte Kette von AssemblyLink-Namen ist (siehe
// resolveJointReference()s eigener Kommentar). "" bedeutet: der Joint liegt direkt in der
// AssemblyObject-Instanz, die getJoints() aufgerufen hat.
struct JointRef
{
    App::DocumentObject* joint = nullptr;
    std::string nestingPrefix;
};

// Kleine, gemeinsame Hilfsfunktion fuer alle Aufrufer, die (noch) nur die reine Joint-Liste
// brauchen (Reihenfolge/Inhalt identisch zu getJoints()s vorherigem Rueckgabetyp) - haelt den
// Migrationsschritt-3-Diff an den meisten Aufrufstellen auf eine einzige Zeile begrenzt.
AssemblyExport std::vector<App::DocumentObject*> extractJointObjects(
    const std::vector<JointRef>& refs
);

class AssemblyExport AssemblyObject: public App::Part
{
    PROPERTY_HEADER_WITH_OVERRIDE(Assembly::AssemblyObject);

public:
    AssemblyObject();
    ~AssemblyObject() override;

    PyObject* getPyObject() override;

    /// returns the type name of the ViewProvider
    const char* getViewProviderName() const override
    {
        return "AssemblyGui::ViewProviderAssembly";
    }

    App::DocumentObjectExecReturn* execute() override;
    void onChanged(const App::Property* prop) override;
    /* Solve the assembly. It will update first the joints, solve, update placements of the parts
    and redraw the joints Args : enableRedo : This store initial positions to enable undo while
    being in an active transaction (joint creation).*/
    int solve(bool enableRedo = false);
    int generateSimulation(App::DocumentObject* sim);
    int updateForFrame(size_t index);
    size_t numberOfFrames();
    void preDrag(std::vector<App::DocumentObject*> dragParts);
    void doDragStep();
    void postDrag();
    void savePlacementsForUndo();
    void undoSolve();
    void clearUndo();

    void exportAsASMT(std::string fileName);
    bool requiresRigidSolveForMove(const std::vector<App::DocumentObject*>& movedParts);

    Base::Placement getMbdPlacement(std::shared_ptr<MbD::ASMTPart> mbdPart);
    bool validateNewPlacements();
    void setNewPlacements();
    static void redrawJointPlacements(std::vector<App::DocumentObject*> joints);
    static void redrawJointPlacement(App::DocumentObject* joint);

    // This makes sure that LinkGroups or sub-assemblies have identity placements.
    void ensureIdentityPlacements();
    // Make sure grounded joints reflect Placement read-only states
    void syncGroundedJoints();

    // Ondsel Solver interface
    std::shared_ptr<MbD::ASMTAssembly> makeMbdAssembly();
    void create_mbdSimulationParameters(App::DocumentObject* sim);
    std::shared_ptr<MbD::ASMTPart> makeMbdPart(
        std::string& name,
        Base::Placement plc = Base::Placement(),
        double mass = 1.0
    );
    std::shared_ptr<MbD::ASMTPart> getMbDPart(App::DocumentObject* obj);
    // To help the solver, during dragging, we are bundling parts connected by a fixed joint.
    // So several assembly components are bundled in a single ASMTPart.
    // So we need to store the plc of each bundled object relative to the bundle origin (first obj
    // of objectPartMap).
    struct MbDPartData
    {
        std::shared_ptr<MbD::ASMTPart> part;
        Base::Placement offsetPlc;  // This is the offset within the bundled parts
    };
    MbDPartData getMbDData(App::DocumentObject* part);
    std::shared_ptr<MbD::ASMTMarker> makeMbdMarker(std::string& name, Base::Placement& plc);
    std::vector<std::shared_ptr<MbD::ASMTJoint>> makeMbdJoint(App::DocumentObject* joint);
    std::shared_ptr<MbD::ASMTJoint> makeMbdJointOfType(App::DocumentObject* joint, JointType jointType);
    std::shared_ptr<MbD::ASMTJoint> makeMbdJointDistance(App::DocumentObject* joint);
    std::string handleOneSideOfJoint(
        App::DocumentObject* joint,
        const char* propRefName,
        const char* propPlcName,
        const std::string& markerName = std::string()
    );
    void getRackPinionMarkers(
        App::DocumentObject* joint,
        std::string& markerNameI,
        std::string& markerNameJ
    );
    int slidingPartIndex(App::DocumentObject* joint);

    void jointParts(std::vector<App::DocumentObject*> joints);
    JointGroup* getJointGroup() const;
    ViewGroup* getExplodedViewGroup() const;
    template<typename T>
    T* getGroup();

    // FCPROJECT-PATCH (16, nachgebessert): verboseLog-Parameter ergaenzt, default false. Grund:
    // getJoints() wird nicht nur von solve() aufgerufen, sondern auch von isPartConnected()/
    // getJointsOfPart() - und DIE laufen waehrend einer interaktiven Zieh-Bewegung (preDrag())
    // potenziell auf JEDEM Mausereignis. Das Debug-Logging aus Fix 16 lief zunaechst unbedingt mit
    // und hat dadurch beim Draggen im 3D-Fenster massiv CPU gekostet (Report-View-Textausgabe ist
    // pro Aufruf nicht billig). Jetzt nur noch fuer den direkten solve()-Aufruf aktiviert.
    //
    // FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix, siehe
    // patches/assembly-architecture-overview.md, Abschnitt "Teilschritt 2 - Konkreter
    // Detailplan"/"Teilschritt 2 - Umsetzung"): nestingPrefix-Parameter ergaenzt, default "" (leer -
    // JEDER bestehende Aufrufer, der ihn nicht angibt, ist dadurch unveraendert). Der subJoints-Zweig
    // (Implementierung) steigt damit rekursiv in die ECHTE verlinkte AssemblyObject-Instanz jeder
    // flexiblen Unter-AssemblyLink ab (AssemblyLink::getLinkedAssembly()) statt wie bisher nur die
    // bereits KOPIERTEN Joints aus der AssemblyLink-eigenen JointGroup zu lesen.
    //
    // FCPROJECT-PATCH (Migrationsschritt 3 "Adressieren statt Kopieren", siehe
    // docs/ARCHITECTURE.md Abschnitt 5): Rueckgabetyp von vector<DocumentObject*> auf
    // vector<JointRef> umgestellt - jeder Joint traegt sein nestingPrefix jetzt direkt im
    // Rueckgabewert mit, statt es (wie vorher) zusaetzlich in die Member-Map
    // jointNestingPrefixMap zu schreiben. Reiner Typ-Umbau, KEINE Verhaltensaenderung: dieselben
    // Joints, dieselbe Reihenfolge, dasselbe nestingPrefix pro Joint wie zuvor - nur anders
    // transportiert. jointNestingPrefixMap selbst bleibt als Lookup-Struktur fuer
    // resolvePartForMbD() bestehen, wird aber jetzt von den drei "obersten" Aufrufern
    // (solve()/generateSimulation()/exportAsASMT()) direkt nach diesem Aufruf aus dem
    // Rueckgabewert befuellt, statt tief in der Rekursion hier.
    std::vector<JointRef> getJoints(
        bool delBadJoints = false,
        bool subJoints = true,
        bool verboseLog = false,
        const std::string& nestingPrefix = std::string()
    );
    std::vector<App::DocumentObject*> getGroundedJoints();
    std::vector<App::DocumentObject*> getRigidGroups();
    std::vector<App::DocumentObject*> getJointsOfObj(App::DocumentObject* obj);
    std::vector<App::DocumentObject*> getJointsOfPart(App::DocumentObject* part);
    App::DocumentObject* getJointOfPartConnectingToGround(
        App::DocumentObject* part,
        std::string& name,
        const std::vector<App::DocumentObject*>& excludeJoints = {}
    );
    std::unordered_set<App::DocumentObject*> getGroundedParts();
    std::unordered_set<App::DocumentObject*> fixGroundedParts();
    void fixGroundedPart(App::DocumentObject* obj, Base::Placement& plc, std::string& jointName);

    bool isJointConnectingPartToGround(App::DocumentObject* joint, const char* partPropName);
    bool isJointTypeConnecting(App::DocumentObject* joint);

    bool isObjInSetOfObjRefs(App::DocumentObject* obj, const std::vector<ObjRef>& pairs);
    void removeUnconnectedJoints(
        std::vector<App::DocumentObject*>& joints,
        std::unordered_set<App::DocumentObject*> groundedObjs
    );
    void traverseAndMarkConnectedParts(
        App::DocumentObject* currentPart,
        std::vector<ObjRef>& connectedParts,
        const std::vector<App::DocumentObject*>& joints
    );
    std::vector<ObjRef> getConnectedParts(
        App::DocumentObject* part,
        const std::vector<App::DocumentObject*>& joints
    );
    bool isPartGrounded(App::DocumentObject* part);
    bool isPartConnected(App::DocumentObject* part);

    std::vector<ObjRef> getDownstreamParts(
        App::DocumentObject* part,
        App::DocumentObject* joint = nullptr
    );
    App::DocumentObject* getUpstreamMovingPart(
        App::DocumentObject* part,
        App::DocumentObject*& joint,
        std::string& name,
        std::vector<App::DocumentObject*> excludeJoints = {}
    );

    double getObjMass(App::DocumentObject* obj);
    void setObjMasses(std::vector<std::pair<App::DocumentObject*, double>> objectMasses);

    std::vector<AssemblyLink*> getSubAssemblies();

    // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix,
    // 2026-09-03, Nutzerentscheidung "oben nach unten"): das Gegenstueck zu getSubAssemblies() -
    // sucht ueber getInList() (funktioniert dank App::PropertyXLink dokumentuebergreifend) nach
    // einer FLEXIBLEN AssemblyLink, die auf DIESE Instanz zeigt. Existiert eine, loest eine
    // aeussere, umfassendere AssemblyObject-Instanz (via getJoints()/getGroundedParts()s
    // subJoints-Rekursion, Teilschritt 2/2e) diese Baugruppe ohnehin komplett mit - der eigene
    // solve()-Aufruf in execute() wird dann bewusst ausgelassen (siehe dort), damit nicht zwei
    // voneinander unabhaengige Solves fuer denselben physischen Teilbaum um dieselben echten
    // Placement-Properties konkurrieren (Befund 3s eigentliche Ursache, siehe
    // assembly-architecture-overview.md, Abschnitt "NEUER, noch offener Befund"). Rigide
    // AssemblyLinks zaehlen NICHT - eine rigide Unterbaugruppe loest sich weiterhin selbst, die
    // aeussere Ebene behandelt sie nur als ein einziges starres Teil.
    // Betrifft ausschliesslich den Recompute-Pfad (execute()); interaktives Ziehen
    // (ViewProviderAssembly::preDrag()/doDragStep()) ruft solve() weiterhin direkt auf der gerade
    // im Bearbeiten-Modus aktiven Instanz auf, unveraendert.
    bool isNestedUnderFlexibleParent() const;

    // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix, 2026-09-03,
    // live durch Nutzer-Maus-Drag aufgedeckt - dritte Baustelle nach getMovingPartFromSel()/
    // isPartConnected()): App::Part::hasObject() (GroupExtension::hasObject()) vergleicht nur
    // rohe Pointer innerhalb der eigenen, lokalen Group - findet ein ECHTES, ueber eine
    // verschachtelte flexible AssemblyLink erreichtes Objekt (seit dem getMovingPartFromSel()-Fix
    // das, was tatsaechlich aus der Selektion zurueckkommt) NIE, selbst wenn es logisch
    // "innerhalb" dieser Baugruppe liegt. `ViewProviderAssembly::canDragObjectIn3d()` nutzte
    // bisher genau diese lokale Pruefung als Zulassungs-Check fuer interaktives Ziehen - ein
    // korrekt aufgeloestes, aber fremddokument-reales Objekt wurde dadurch komplett vom Ziehen
    // ausgeschlossen (kein preDrag()/isPartConnected()-Aufruf mehr im Log sichtbar), obwohl
    // Solver-seitig laengst alles korrekt verdrahtet ist. Diese Methode ergaenzt den lokalen
    // Fast-Path um genau die Kandidatensuche, die auch syncLocalMirrorPlacement() nutzt: obj gilt
    // als "enthalten", wenn ein lokaler Spiegel-Kandidat existiert, der ueber
    // canonicalizeForMbD() auf exakt dieses obj abbildet.
    bool hasRealObject(App::DocumentObject* obj);

    std::vector<App::DocumentObject*> getMotionsFromSimulation(App::DocumentObject* sim);

    bool isMbDJointValid(App::DocumentObject* joint);

    bool isEmpty() const;
    int numberOfComponents() const;

    void updateSolveStatus();
    inline int getLastDoF() const
    {
        return lastDoF;
    }
    inline bool getLastHasConflicts() const
    {
        return lastHasConflict;
    }
    inline bool getLastHasRedundancies() const
    {
        return lastHasRedundancies;
    }
    inline bool getLastHasPartialRedundancies() const
    {
        return lastHasPartialRedundancies;
    }
    inline bool getLastHasMalformedConstraints() const
    {
        return lastHasMalformedConstraints;
    }
    inline int getLastSolverStatus() const
    {
        return lastSolverStatus;
    }
    inline const std::vector<std::string>& getLastConflicting() const
    {
        return lastConflictingJoints;
    }
    inline const std::vector<std::string>& getLastRedundant() const
    {
        return lastRedundantJoints;
    }
    inline const std::vector<std::string>& getLastPartiallyRedundant() const
    {
        return lastPartialRedundantJoints;
    }
    inline const std::vector<std::string>& getLastMalformed() const
    {
        return lastMalformedJoints;
    }
    fastsignals::signal<void()> signalSolverUpdate;

private:
    void rebuildRigidClusters();
    App::DocumentObject* getRigidRepresentative(App::DocumentObject* part) const;
    const std::vector<App::DocumentObject*>* getRigidMembers(App::DocumentObject* part) const;
    void syncActiveRigidGroupPlacements();
    void updateRigidPlacementCache();

    // FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix, siehe
    // patches/assembly-architecture-overview.md, Abschnitt "Teilschritt 2 - Umsetzung"): loest die
    // fuer den MbD-Solver relevante Teil-Identitaet einer Joint-Referenz adressierungsbewusst auf
    // (AssemblyUtils::resolveJointReference(), mit dem in jointNestingPrefixMap fuer diesen Joint
    // hinterlegten nestingPrefix), statt ueber die alte, sub-pfad-blinde
    // AssemblyUtils::getMovingPartFromRef(). Faellt defensiv auf getMovingPartFromRef() zurueck,
    // falls die Aufloesung fehlschlaegt (kein bekannter Eintrag in jointNestingPrefixMap, oder
    // resolveJointReference() selbst liefert nullptr) - garantiert dadurch, dass jeder bisher
    // funktionierende (nicht verschachtelte) Fall exakt sein bisheriges Verhalten behaelt.
    App::DocumentObject* resolvePartForMbD(App::DocumentObject* joint, const char* propRefName);

    // FCPROJECT-PATCH (Befund 3, Teilschritt 2e "adressieren statt kopieren", solver-root-cause-
    // fix, siehe patches/assembly-architecture-overview.md): resolvePartForMbD() loest Joint-
    // Referenzen bereits auf die ECHTEN, tief verschachtelten Objekte auf - aber
    // getGroundedParts()/getAssemblyComponents() liefern weiterhin die LOKALEN Spiegel-Kopien
    // (AssemblyLink::Group, synchronisiert ueber die alte Kopier-Pipeline). Fuer ein und dasselbe
    // reale Teil existieren dadurch ZWEI verschiedene Pointer, je nachdem ueber welchen Weg man es
    // erreicht - objectPartMap (pointer-keyed) legt fuer beide eigene, voneinander getrennte
    // MbD-Teile an, wodurch ein geerdetes Teil und der Joint, der es eigentlich bewegen soll, NIE
    // im selben MbD-Constraint-Graphen landen (vermutliche Kernursache von Befund 3: der Joint
    // wird beim Ziehen komplett ignoriert). canonicalizeForMbD() macht daraus wieder EINEN
    // Pointer: liegt 'obj' als lokale Spiegel-Kopie innerhalb dieser AssemblyObject-Instanz (Walk
    // ueber InList bis 'this' erreicht wird), wird derselbe Namenspfad stattdessen durch die
    // ECHTEN, ueber AssemblyLink::getLinkedAssembly() erreichten verschachtelten AssemblyObject-
    // Instanzen aufgeloest (gleiches Funktionsprinzip wie getJoints()' subJoints-Rekursion). Liegt
    // 'obj' NICHT in diesem lokalen Baum (z.B. schon ein von resolvePartForMbD() geliefertes
    // echtes Objekt, oder bereits top-level lokal == real), wird 'obj' unveraendert
    // zurueckgegeben - garantiert dadurch, dass jeder bisher funktionierende (nicht
    // verschachtelte) Fall exakt sein bisheriges Verhalten behaelt. Bekannte Einschraenkung: eine
    // Rigid Group, deren Mitglieder innerhalb einer verschachtelten flexiblen AssemblyLink liegen,
    // wird dadurch nicht mehr ueber ihren lokalen Spiegel-Pointer gefunden (siehe
    // getRigidRepresentative() in getMbDData()) - Rigid Group ist laut Nutzer ohnehin aktuell
    // separat als buggy bekannt (project_fcproject_redundant_fixed_joint_rigidgroup_fix-Memory)
    // und wird hier bewusst nicht mitgeloest.
    App::DocumentObject* canonicalizeForMbD(App::DocumentObject* obj);

    // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix, 2026-09-03,
    // live durch Nutzer-Maus-Drag aufgedeckt): AssemblyLink::synchronizeComponents() spiegelt die
    // Placement eines lokalen Spiegel-Objekts (das, was tatsaechlich in der 3D-Ansicht gerendert
    // und mit der Maus gezogen wird) nur bei Rigid=true vom echten Quellobjekt zurueck - bei
    // flexiblen Unterbaugruppen fehlt dieser Ruecksync komplett, wodurch ein korrekt geloestes
    // Solver-Ergebnis nie sichtbar wird. Ein Sync-Versuch direkt in AssemblyLink.cpp scheitert an
    // der Ausfuehrungsreihenfolge (dessen execute() laeuft VOR dem solve() der Baugruppe, siehe
    // ausfuehrliche Begruendung am Implementierungsort in AssemblyObject.cpp) - stattdessen hier,
    // direkt im Anschluss an setNewPlacements()' Schreiben des kanonischen (echten) Werts, wo der
    // frisch geloeste Wert garantiert aktuell ist.
    void syncLocalMirrorPlacement(App::DocumentObject* realObj, const Base::Placement& plc);

    std::shared_ptr<MbD::ASMTAssembly> mbdAssembly;

    std::unordered_map<App::DocumentObject*, MbDPartData> objectPartMap;
    // FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix): pro
    // Original-Joint (Pointer-Identitaet) das nestingPrefix, mit dem getJoints() ihn beim
    // rekursiven Abstieg in verschachtelte, flexible AssemblyLinks gefunden hat (leer "" fuer
    // einen Joint, der direkt in dieser AssemblyObject-Instanz liegt). Wird von
    // resolvePartForMbD() gelesen.
    //
    // FCPROJECT-PATCH (Migrationsschritt 3 "Adressieren statt Kopieren"): getJoints() selbst
    // befuellt diese Map NICHT mehr (das nestingPrefix steckt jetzt direkt in deren
    // JointRef-Rueckgabewert, s. Deklarationsort) - stattdessen befuellen die drei "obersten"
    // Aufrufer (solve()/generateSimulation()/exportAsASMT()) sie direkt nach ihrem jeweiligen
    // getJoints()-Aufruf aus dem Rueckgabewert. Lifecycle unveraendert identisch zu
    // objectPartMap gehalten - geleert an genau denselben Stellen wie objectPartMap.clear(),
    // NICHT bei jedem getJoints()-Aufruf selbst, weil ein waehrend des Draggens ausgeloester
    // getJoints()-Aufruf (isPartConnected()/getJointsOfPart()) die fuer den GERADE LAUFENDEN
    // solve() gueltigen Eintraege nicht loeschen darf.
    std::unordered_map<App::DocumentObject*, std::string> jointNestingPrefixMap;
    std::unordered_map<App::DocumentObject*, App::DocumentObject*> rigidRepByPart;
    std::unordered_map<App::DocumentObject*, std::vector<App::DocumentObject*>> rigidMembersByRep;
    std::unordered_map<App::DocumentObject*, Base::Placement> rigidPlacementCache;
    std::vector<std::pair<App::DocumentObject*, double>> objMasses;
    std::vector<App::DocumentObject*> draggedParts;
    std::vector<App::DocumentObject*> motions;

    std::vector<std::pair<App::DocumentObject*, Base::Placement>> previousPositions;

    // FCPROJECT-PATCH (Root-Cause-Fix, solver-root-cause-fix): syncGroundedJoints() loeschte
    // bisher ein GroundedJoint-Objekt sofort beim ERSTEN solve()-Aufruf, der ein
    // nicht-ReadOnly Placement bei gleichzeitig noch existierendem GroundedJoint sieht. Das
    // triff faelschlich zu, wenn der ganz frueh (waehrend/kurz nach dem Dokument-Restore per
    // onChanged(&Group) ausgeloeste) solve()-Aufruf schneller laeuft als
    // GroundedJoint.onDocumentRestored() (Python), das das ReadOnly-Flag neu setzt - eine
    // echte Race Condition, siehe patches/bugreport-groundedjoint-deletion-race/Questions.md.
    // Dieses Set verlangt eine ZWEITE Bestaetigung in einem SPAETEREN solve()-Aufruf, bevor
    // tatsaechlich geloescht wird - harmlos fuer den echten Anwendungsfall (Nutzer hebt die
    // Sperre manuell auf: der inkonsistente Zustand bleibt ueber mehrere solve()-Aufrufe
    // hinweg bestehen), verhindert aber den einmaligen Race-Treffer beim Laden.
    std::unordered_set<App::DocumentObject*> pendingGroundedJointRemoval;

    bool bundleFixed;

    int lastDoF;
    bool lastHasConflict;
    bool lastHasRedundancies;
    bool lastHasPartialRedundancies;
    bool lastHasMalformedConstraints;
    int lastSolverStatus;

    std::vector<std::string> lastRedundantJoints;
    std::vector<std::string> lastConflictingJoints;
    std::vector<std::string> lastPartialRedundantJoints;
    std::vector<std::string> lastMalformedJoints;
};

}  // namespace Assembly
