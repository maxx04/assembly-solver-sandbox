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
#include <optional>
#include <set>
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
class IdentityGraph;
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

// Starre Gruppe (RigidGroupJoint) samt Verschachtelungspfad, wie JointRef.
struct RigidGroupRef
{
    App::DocumentObject* group = nullptr;
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
    // FCPROJECT-PATCH (2026-09-13, "Erdung driftet trotz eingefrorenem Ziel" - siehe
    // docs/ARCHITECTURE.md §2.1a): liefert die starre Korrektur-Transformation, die das
    // tatsaechlich geloeste Placement des (per Definition genau einen) explizit geerdeten
    // Teils dieser Ebene exakt auf seinen eingefrorenen Zielwert (GroundedJoint.
    // GroundedPlacement) zurueckbiegt - Identity, wenn keine Abweichung besteht oder keine
    // explizite Erdung vorliegt. Wird in setNewPlacements() auf ALLE geloesten Placements
    // dieser Ebene angewendet, nicht nur auf das geerdete Teil selbst - das haelt saemtliche,
    // vom Solver bereits korrekt berechneten RELATIVEN Positionen zwischen den Teilen exakt
    // erhalten (eine global-starre Transformation aendert keine Relativgeometrie).
    Base::Placement computeGroundCorrection();
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
    // FCPROJECT-PATCH (Bug C, "Instanz-Identitaet innerhalb einer duplizierten flexiblen
    // Unterbaugruppe", 2026-09-16): 'alreadyResolved' Default false = unveraendertes
    // Altverhalten (kanonisiert 'obj'/'part' intern via canonicalizeForMbD()). true nur von
    // resolvePartForMbD()-Aufrufern (handleOneSideOfJoint()/isMbDJointValid()/
    // getRackPinionMarkers()) gesetzt: deren Eingabe ist bereits vollstaendig aufgeloest (echtes
    // kanonisches Objekt ODER, bei Duplikation, bewusst ein instanzeigener Spiegel) - ein erneutes
    // canonicalizeForMbD() waere im Normalfall nur redundant, wuerde aber im Spiegel-Fall die
    // Bug-C-Ersetzung sofort per getSourceForMirror() rueckgaengig machen (siehe
    // docs/ARCHITECTURE.md §4.1 "Bug C").
    std::shared_ptr<MbD::ASMTPart> getMbDPart(App::DocumentObject* obj, bool alreadyResolved = false);
    // Adressierungsbewusste Teil-Aufloesung einer Joint-Referenz (Herleitung am Ende des
    // private-Abschnitts). Oeffentlich fuer die Traegersuche koaxialer Zahnraeder.
    App::DocumentObject* resolvePartForMbD(
        App::DocumentObject* joint,
        const char* propRefName,
        const std::string& nestingPrefix
    );
    // To help the solver, during dragging, we are bundling parts connected by a fixed joint.
    // So several assembly components are bundled in a single ASMTPart.
    // So we need to store the plc of each bundled object relative to the bundle origin (first obj
    // of objectPartMap).
    struct MbDPartData
    {
        std::shared_ptr<MbD::ASMTPart> part;
        Base::Placement offsetPlc;  // This is the offset within the bundled parts
        // FCPROJECT-PATCH (2026-09-19, "zweite BG25-Instanz falsch eingesetzt" - siehe
        // requirement-flexible-subassembly-any-anchor-point/Folgefund): Identitaet 'part' kann
        // (Bug C, alreadyResolved=true) ein NICHT-kanonisiertes, bewusst instanzeigenes
        // Spiegelobjekt sein, das strukturell innerhalb einer oder mehrerer AssemblyLink-Container
        // verschachtelt liegt (findLocalGroupPath() von 'this' aus). Dessen eigene Placement-
        // Property ist dann NICHT die Welt-/Absolut-Position, sondern relativ zu diesen
        // umschliessenden Containern - deren eigene Placement kann (z.B. wenn der Nutzer eine
        // zweite Instanz im Baum manuell verschiebt, um Ueberlappung zu vermeiden) von der
        // Identitaet abweichen. containerChainPlc haelt das Produkt der Container-Placements
        // (aeusserster zuerst) fest, mit dem eine vom Solver in ABSOLUTEN Weltkoordinaten
        // berechnete Placement in setNewPlacements() zurueck in die lokale, Container-relative
        // Placement dieses Spiegelobjekts umgerechnet wird - identisch zu dem, was
        // App::DocumentObject::getGlobalPlacement() beim Lesen bereits tut, hier nur in die
        // Gegenrichtung angewendet. Identity (Default) = unveraendertes Verhalten fuer alle
        // bisherigen, nicht verschachtelten oder voll kanonisierten Faelle.
        Base::Placement containerChainPlc;
    };
    MbDPartData getMbDData(App::DocumentObject* part, bool alreadyResolved = false);
    std::shared_ptr<MbD::ASMTMarker> makeMbdMarker(std::string& name, Base::Placement& plc);
    // FCPROJECT-PATCH (Mehrfachinstanz-Fix - siehe
    // docs/ARCHITECTURE.md Abschnitt 4/5): nestingPrefix-Parameter ergaenzt. Ohne ihn wurde die
    // ASMT-Joint-/Marker-Benennung (siehe Definitionsort) allein aus joint->getFullName()
    // gebildet - kollidiert, sobald DERSELBE reale Joint ueber ZWEI verschiedene
    // AssemblyLink-Instanzen desselben verlinkten Dokuments erreicht wird (zwei Eintraege in
    // getJoints()' Rueckgabe mit demselben joint-Zeiger, aber unterschiedlichem nestingPrefix -
    // siehe jointInstanceName()). "" (Default) entspricht exakt dem alten, nicht verschachtelten
    // Verhalten.
    std::vector<std::shared_ptr<MbD::ASMTJoint>> makeMbdJoint(
        App::DocumentObject* joint,
        const std::string& nestingPrefix = std::string()
    );
    std::shared_ptr<MbD::ASMTJoint> makeMbdJointOfType(App::DocumentObject* joint, JointType jointType);
    std::shared_ptr<MbD::ASMTJoint> makeMbdJointDistance(App::DocumentObject* joint);
    std::string handleOneSideOfJoint(
        App::DocumentObject* joint,
        const char* propRefName,
        const char* propPlcName,
        const std::string& markerName = std::string(),
        const std::string& nestingPrefix = std::string()
    );
    void getRackPinionMarkers(
        App::DocumentObject* joint,
        std::string& markerNameI,
        std::string& markerNameJ,
        const std::string& nestingPrefix = std::string()
    );
    int slidingPartIndex(App::DocumentObject* joint);

    // FCPROJECT-PATCH (Mehrfachinstanz-Fix): Parametertyp von
    // vector<DocumentObject*> auf vector<JointRef> umgestellt - jointNestingPrefixMap (siehe
    // deren Entfernung unten) transportierte das nestingPrefix bisher separat und kollabierte
    // dabei zwei Instanzen desselben Joints auf einen einzigen Eintrag; jetzt reist es mit jedem
    // JointRef direkt mit, siehe getJoints().
    void jointParts(const std::vector<JointRef>& joints);
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
    // transportiert. jointNestingPrefixMap selbst blieb zunaechst als separate Lookup-Struktur
    // fuer resolvePartForMbD() bestehen - seit dem Mehrfachinstanz-Fix (docs/ARCHITECTURE.md
    // Abschnitt 4/5) ist sie ERSATZLOS entfernt, siehe deren vormaligen Deklarationsort weiter
    // unten fuer die Begruendung (NICHT zu verwechseln mit dem separat, ebenfalls bewusst
    // zurueckgestellten Teilschritt 3.3 aus Abschnitt 5 - einem eigenen, andersartigen
    // `getGlobalPlacement()`-Experiment). Der JointRef-Rueckgabewert hier ist seitdem der
    // EINZIGE Transportweg fuer das nestingPrefix.
    std::vector<JointRef> getJoints(
        bool delBadJoints = false,
        bool subJoints = true,
        bool verboseLog = false,
        const std::string& nestingPrefix = std::string()
    );
    std::vector<App::DocumentObject*> getGroundedJoints();
    // fixMembers=false: Mitgliederliste nur pruefen, nie bereinigen (fuer starre Gruppen aus
    // Unterbaugruppen, deren Dokument von aussen nicht veraendert werden darf).
    std::vector<App::DocumentObject*> getRigidGroups(bool fixMembers = true);
    // FCPROJECT-PATCH (2026-10-20, starre Gruppe ueber Baugruppengrenze): starre Gruppen dieser
    // Baugruppe und - wie getJoints() die Gelenke - rekursiv aller flexibel eingebauten
    // Unterbaugruppen, je mit nestingPrefix. Eine starr eingebaute Unterbaugruppe ist ohnehin EIN
    // Koerper, ihre inneren Gruppen zaehlen nicht.
    std::vector<RigidGroupRef> getRigidGroupInstances(const std::string& nestingPrefix = std::string());
    std::vector<App::DocumentObject*> getJointsOfObj(App::DocumentObject* obj);
    // FCPROJECT-PATCH (2026-09-20, "BG25 Slider-Drag beide Instanzen" - siehe
    // todo-bg25-slider-drag-two-instances): Rueckgabetyp von vector<DocumentObject*> auf
    // vector<JointRef> umgestellt - diese GUI-Drag-Traversal (getJointOfPartConnectingToGround(),
    // isJointConnectingPartToGround()) wurde bei der urspruenglichen "Adressieren statt
    // Kopieren"-Migration (Sept. 2026) NIE auf resolvePartForMbD()/nestingPrefix umgestellt und
    // verglich Joint-Endpunkte weiterhin ueber die adressierungsblinde getMovingPartFromRef() +
    // ein nacktes canonicalizeForMbD() ohne Instanzkontext. Bei EINER Instanz einer flexiblen
    // Unterbaugruppe kollabierten Eingabe-Teil und Joint-Endpunkt zufaellig auf denselben
    // kanonischen Zeiger; sobald der Container selbst dupliziert ist (zwei BG25-Instanzen,
    // Bug C "Identitaet behalten"), bleibt das Eingabe-Teil bewusst instanzeigen, waehrend der
    // naive Endpunkt-Vergleich bis zum GETEILTEN Template-Objekt durchkanonisiert - beide
    // Zeiger stimmen dann fuer KEINE der beiden Instanzen mehr ueberein, ein interner Subjoint
    // (z.B. der Slider innerhalb BG25) wird fuer gar keine Instanz mehr gefunden. Das
    // nestingPrefix jedes Joints (siehe getJoints()) ist jetzt Teil des Rueckgabewerts, damit
    // die Aufrufer resolvePartForMbD() statt der alten Funktionen nutzen koennen.
    std::vector<JointRef> getJointsOfPart(App::DocumentObject* part);
    // FCPROJECT-PATCH (2026-10-04): 'nestingPrefixOut' (optional) bekommt den Verschachtelungspfad
    // des gefundenen Joints - noetig, um dessen andere Seite korrekt aufzuloesen (getUpstreamMovingPart()).
    App::DocumentObject* getJointOfPartConnectingToGround(
        App::DocumentObject* part,
        std::string& name,
        const std::vector<App::DocumentObject*>& excludeJoints = {},
        std::string* nestingPrefixOut = nullptr
    );
    App::DocumentObject* getJointOfPartForUngroundedDrag(App::DocumentObject* part, std::string& name);
    // FCPROJECT-PATCH (2026-10-03, "Erdung ist Kontext-Eigenschaft"): reine Funktion des
    // Kontexts dieser Ebene, siehe Kommentar in der Implementierung. 'ignoredNestedRoots'
    // (optional) bekommt die verschachtelten Erdungen, die im Kontext dieser Ebene ignoriert
    // werden, jeweils mit der massgeblichen Erdung ihres Clusters (nur fuer die Meldung in solve()).
    // propagateRigid=false (2026-10-20): Erdung nicht ueber die starren Gruppen DIESER Ebene
    // verteilen - fuer die Abfrage durch eine einbauende Baugruppe, die selbst ueber ihre (auch
    // die verschachtelten) Gruppen verteilt.
    std::unordered_set<App::DocumentObject*> getGroundedParts(
        std::vector<std::pair<App::DocumentObject*, App::DocumentObject*>>* ignoredNestedRoots = nullptr,
        bool propagateRigid = true
    );
    std::unordered_set<App::DocumentObject*> fixGroundedParts();
    void fixGroundedPart(App::DocumentObject* obj, Base::Placement& plc, std::string& jointName);

    // nestingPrefix-Parameter (2026-09-20, siehe getJointsOfPart()-Deklaration oben): Default ""
    // haelt den bestehenden Python-Aufruf aus JointObject.py (immer fuer einen Top-Level-Joint,
    // nestingPrefix implizit leer) unveraendert; nur der interne C++-Aufruf aus
    // getJointOfPartConnectingToGround() fuer einen ueber subJoints() erreichten Subjoint gibt
    // sein eigenes nestingPrefix mit.
    bool isJointConnectingPartToGround(
        App::DocumentObject* joint,
        const char* partPropName,
        const std::string& nestingPrefix = std::string()
    );
    bool isJointTypeConnecting(App::DocumentObject* joint);

    bool isObjInSetOfObjRefs(App::DocumentObject* obj, const std::vector<ObjRef>& pairs);
    // FCPROJECT-PATCH (Mehrfachinstanz-Fix - siehe
    // docs/ARCHITECTURE.md Abschnitt 4/5): Parametertyp von vector<DocumentObject*> auf
    // vector<JointRef> umgestellt, aus demselben Grund wie bei jointParts() oben - das
    // nestingPrefix jedes Joints wird hier fuer resolvePartForMbD() gebraucht (Reference1/2-
    // Aufloesung), und jointNestingPrefixMap konnte das fuer zwei Instanzen desselben realen
    // Joints nicht mehr eindeutig bereithalten.
    void removeUnconnectedJoints(
        std::vector<JointRef>& joints,
        std::unordered_set<App::DocumentObject*> groundedObjs
    );
    void traverseAndMarkConnectedParts(
        App::DocumentObject* currentPart,
        std::vector<ObjRef>& connectedParts,
        const std::vector<JointRef>& joints
    );
    std::vector<ObjRef> getConnectedParts(
        App::DocumentObject* part,
        const std::vector<JointRef>& joints
    );
    bool isPartGrounded(App::DocumentObject* part);
    // FCPROJECT-PATCH (Teilschritt 3.1b): verboseLog-Parameter ergaenzt, damit die
    // FCPROJECT-DEBUG-Logs in der Definition (urspruenglich fuer die Befund-3-Live-Diagnose
    // 2026-09-03 unbedingt eingebaut) nicht mehr in jedem preDrag()-Mausereignis unbedingt
    // laufen. Default false, kein bestehender Aufrufer muss angepasst werden.
    bool isPartConnected(App::DocumentObject* part, bool verboseLog = false);
    // Kanonische Teile (canonicalizeForMbD()), die von der Erdung aus ueber Gelenke und starre
    // Gruppen erreichbar sind - einmal berechnet fuer viele Abfragen (preDrag()).
    std::unordered_set<App::DocumentObject*> getConnectedPartSet();
    // FCPROJECT-PATCH (2026-10-04): Erreichbarkeit von 'part' aus einer VORGEGEBENEN Erdung, mit
    // ausgeblendeten Joints - ohne Joints zu deaktivieren (siehe isJointConnectingPartToGround()).
    bool isPartConnectedExcluding(
        App::DocumentObject* part,
        const std::unordered_set<App::DocumentObject*>& groundedObjs,
        const std::vector<JointRef>& excludedJoints
    );

    // FCPROJECT-PATCH (2026-09-21, Nutzerauftrag [[todo-identitygraph-visualization]]): duenner
    // Python-Zugang zu IdentityGraph::exportDot() - baut den Graphen dieser Baugruppe frisch auf
    // und liefert die fertige Graphviz-DOT-Textdarstellung.
    std::string exportIdentityGraphDot();

    // FCPROJECT-PATCH (2026-09-21, Nutzerauftrag "asmt als zusaetzliche Kontrolle nutzen"):
    // duenner Python-Zugang zu IdentityGraph::verifyAgainstAsmt() - siehe dortigen Kommentar fuer
    // Zweck und Einschraenkungen.
    std::string verifyIdentityGraphAgainstAsmt();

    // FCPROJECT-PATCH (2026-10-03): duenner Python-Zugang zu IdentityGraph::verifyHandleConsistency().
    std::vector<std::string> verifyIdentityGraphHandleConsistency();

    // FCPROJECT-PATCH (Fix-Ansatz D, "flexible Unterbaugruppe als Ganzes ziehbar, wenn
    // unverbunden", Nutzerauftrag 2026-09-16/17): isPartConnected(containerObj) allein reicht
    // NICHT aus, um zu pruefen, ob eine flexible AssemblyLink "als Ganzes unverbunden" ist - ein
    // Joint referenziert immer ein KIND im Container (z.B. eine Halterbaugruppe darin), NIE den
    // Container selbst, daher liefert isPartConnected(container) IMMER false, unabhaengig davon,
    // ob eines seiner Kinder laengst korrekt extern angeschlossen ist (live beobachtet: nach
    // Anlegen eines Joints auf ein Kind blieb die ganze Instanz trotzdem frei ziehbar). Steigt
    // rekursiv durch jede verschachtelte flexible AssemblyLink ab (rigide werden wie ein Blatt
    // behandelt - sie sind fuer den Solver bereits die atomare Einheit) und prueft JEDES Blatt
    // einzeln per isPartConnected() - true nur, wenn WIRKLICH kein einziges Kind irgendwo
    // angeschlossen ist.
    bool isSubAssemblyFullyUnconnected(App::DocumentObject* obj);

    // FCPROJECT-PATCH (2026-10-11): alsoIgnore - weitere Gelenke, die beim Bestimmen der
    // mitbewegten Teile nicht als Verbindung zur Masse zaehlen (siehe findDragMode(): das
    // umgangene Nicht-Bewegungsgelenk, z.B. Distance).
    std::vector<ObjRef> getDownstreamParts(
        App::DocumentObject* part,
        App::DocumentObject* joint = nullptr,
        const std::vector<App::DocumentObject*>& alsoIgnore = {}
    );
    App::DocumentObject* getUpstreamMovingPart(
        App::DocumentObject* part,
        App::DocumentObject*& joint,
        std::string& name,
        std::vector<App::DocumentObject*> excludeJoints = {}
    );
    // FCPROJECT-PATCH (2026-10-11/12): Zieh-Gelenk fuer ein angeklicktes Teil - das
    // Bewegungsgelenk mit den wenigsten Freiheitsgraden unter allen Verbindungen zur Masse
    // (Fixed ueber die starre Kette verfolgt). Liefert das Teil an diesem Gelenk, sonst nullptr;
    // bypassed: die uebrigen gefundenen Verbindungen (fuer getDownstreamParts()). Siehe
    // ViewProviderAssembly::findDragMode().
    App::DocumentObject* getDragJointOfPart(
        App::DocumentObject* part,
        App::DocumentObject*& joint,
        std::string& name,
        std::vector<App::DocumentObject*>& bypassed
    );

    double getObjMass(App::DocumentObject* obj);
    void setObjMasses(std::vector<std::pair<App::DocumentObject*, double>> objectMasses);

    // FCPROJECT-PATCH (Bug C, "Instanz-Identitaet innerhalb einer duplizierten flexiblen
    // Unterbaugruppe", 2026-09-16): const gemacht (aendert kein Verhalten, hasObject() und
    // getDocument() sind bereits const) - wird jetzt auch von AssemblyUtils::hasSiblingInstances()
    // ueber einen 'const AssemblyObject*' aufgerufen (resolveJointReference()/getMovingPartFromSel()
    // erhalten die loesende Baugruppe nur als const-Zeiger).
    std::vector<AssemblyLink*> getSubAssemblies() const;

    // FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 0, siehe
    // /home/maxx/.claude/plans/enumerated-roaming-river.md): rein diagnostische
    // Aequivalenz-Pruefung fuer die Verifikations-Testmatrix - vergleicht IdentityGraph::resolve()/
    // resolveJointRef() gegen canonicalizeForMbD()/AssemblyUtils::resolveJointReference() fuer
    // JEDES geerdete Teil und JEDE Joint-Referenz dieser Baugruppe (inkl. subJoints). Liefert eine
    // Liste menschenlesbarer Zeilen zurueck: pro geprueftem Objekt entweder "MATCH ...",
    // "DIVERGENCE (by design, dupliziert) ..." (erwartete Abweichung, siehe Kommentar am
    // Definitionsort) oder "MISMATCH ..." (unerwartete Abweichung - ein Bug im neuen Graphen).
    // Wird entfernt, sobald die eigentlichen Aufrufstellen in Phase 1+ auf den Graphen umgestellt
    // sind und dieser Vergleich gegenstandslos wird.
    std::vector<std::string> verifyIdentityGraphEquivalence();

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

    // FCPROJECT-PATCH (2026-09-25, "ich kann nicht aktive Gruppe bearbeiten" - Nutzeranforderung):
    // isNestedUnderFlexibleParent() (s.o.) laesst execute() JEDE nested Instanz uebersprungen, ganz
    // gleich ob der Nutzer sie gerade aktiv im Bearbeiten-Modus hat oder nicht - dadurch blockiert
    // ein singulaeres/scheiterndes Solve der AEUSSEREN Baugruppe (z.B. CNC3018_037_A_TraegerBau-
    // gruppe_Z's 8-Joint-Doppelschienen-Singularitaet) auch das Bearbeiten einer inneren, fuer
    // sich genommen voellig unproblematischen Unterbaugruppe (empirisch bestaetigt: deren eigener
    // solve() gibt isoliert 0/Erfolg zurueck). ViewProviderAssembly::setEdit()/unsetEdit() setzen
    // dieses Flag, solange der Nutzer GENAU DIESE Instanz aktiv bearbeitet - execute() behandelt
    // sie dann wie eine echte Top-Level-Instanz (eigener solve()-Aufruf statt Ueberspringen),
    // unabhaengig vom Zustand einer aeusseren Baugruppe. Betrifft nur den Anzeige-/Bearbeiten-Kom-
    // fort waehrend der aktiven Bearbeitung dieser einen Instanz - die WELT-Position (deren
    // Placement relativ zu allem ausserhalb) haengt weiterhin von der aeusseren Baugruppe ab und
    // wird durch dieses Flag nicht veraendert.
    void setActiveEditContext(bool active)
    {
        activeEditContext = active;
    }

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

    // FCPROJECT-PATCH (Mehrfachinstanz-Fix): nestingPrefix-Parameter
    // ergaenzt, direkt an resolvePartForMbD() durchgereicht (siehe dort) statt aus
    // jointNestingPrefixMap gelesen. "" (Default) entspricht dem alten, nicht verschachtelten
    // Verhalten.
    bool isMbDJointValid(App::DocumentObject* joint, const std::string& nestingPrefix = std::string());

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
    // FCPROJECT-PATCH (2026-10-11): siehe lastRedundancyGroups
    inline const std::vector<std::string>& getLastRedundancyGroups() const
    {
        return lastRedundancyGroups;
    }
    // FCPROJECT-PATCH (2026-10-14): siehe lastRedundancyStates
    inline const std::vector<std::string>& getLastRedundancyStates() const
    {
        return lastRedundancyStates;
    }
    // FCPROJECT-PATCH (2026-10-13): siehe lastLimitViolations
    inline const std::vector<std::string>& getLastLimitViolations() const
    {
        return lastLimitViolations;
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
    // FCPROJECT-PATCH (2026-10-12): Kandidat fuer getDragJointOfPart() - Teil am Bewegungsgelenk,
    // das Gelenk, sein Referenzname, Freiheitsgrade und die Gelenkkette vom angeklickten Teil.
    struct DragChoice
    {
        App::DocumentObject* part = nullptr;
        App::DocumentObject* joint = nullptr;
        std::string name;
        int dof = 1000;
        std::vector<App::DocumentObject*> chain;
    };
    DragChoice findDragChoice(
        App::DocumentObject* part,
        const std::vector<App::DocumentObject*>& excludeJoints,
        std::set<App::DocumentObject*>& visited,
        std::vector<App::DocumentObject*>& found
    );

    // FCPROJECT-PATCH (2026-10-04, BG22-Haenger in der GUI): fuer die Dauer EINES Vorgangs
    // (isPartConnected(), solve()) gemeinsam genutzter IdentityGraph - siehe ScopedIdentityGraph in
    // AssemblyObject.cpp. Reiner Laufzeitzustand, nicht persistent.
    IdentityGraph* scopedGraph = nullptr;

    void rebuildRigidClusters();
    App::DocumentObject* getRigidRepresentative(App::DocumentObject* part) const;
    const std::vector<App::DocumentObject*>* getRigidMembers(App::DocumentObject* part) const;
    void syncActiveRigidGroupPlacements();
    void updateRigidPlacementCache();
    void prepareMbdForIslandDrag(std::vector<App::DocumentObject*> dragParts);

    // FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix, siehe
    // patches/assembly-architecture-overview.md, Abschnitt "Teilschritt 2 - Umsetzung"): loest die
    // fuer den MbD-Solver relevante Teil-Identitaet einer Joint-Referenz adressierungsbewusst auf
    // (AssemblyUtils::resolveJointReference(), mit dem uebergebenen nestingPrefix), statt ueber
    // die alte, sub-pfad-blinde AssemblyUtils::getMovingPartFromRef(). Faellt defensiv auf
    // getMovingPartFromRef() zurueck, falls die Aufloesung fehlschlaegt (resolveJointReference()
    // liefert nullptr) - garantiert dadurch, dass jeder bisher funktionierende (nicht
    // verschachtelte) Fall exakt sein bisheriges Verhalten behaelt.
    //
    // FCPROJECT-PATCH (Mehrfachinstanz-Fix - siehe
    // docs/ARCHITECTURE.md Abschnitt 4/5): nestingPrefix wird jetzt vom Aufrufer direkt
    // uebergeben statt aus der (entfernten) jointNestingPrefixMap gelesen - diese Map war nach
    // Joint-ZEIGER geschluesselt und konnte deshalb fuer denselben realen Joint, ueber zwei
    // verschiedene AssemblyLink-Instanzen desselben verlinkten Dokuments erreicht, nur EIN
    // nestingPrefix gleichzeitig halten (der zweite Eintrag ueberschrieb den ersten
    // kommentarlos) - eine der beiden Instanzen wurde dadurch mit dem FALSCHEN Prefix aufgeloest.
    // (Deklaration resolvePartForMbD(): public, bei getMbDPart().)

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
    // FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 3): TEMPORAER, nur fuer den
    // Aequivalenz-Verifikationslauf gegen die neue, graphbasierte canonicalizeForMbD() - siehe
    // Definition in AssemblyObject.cpp.
    App::DocumentObject* canonicalizeForMbDLegacy(App::DocumentObject* obj);

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

    // FCPROJECT-PATCH (2026-09-13, "Erdung driftet trotz Solve-Erfolg" - siehe
    // docs/ARCHITECTURE.md §2.1a): der EINE (per Definition genau eine, unabhaengig von der
    // Teileanzahl) explizit per GroundedJoint geerdete Zielwert dieses solve()-Durchlaufs -
    // live in fixGroundedParts() eingelesen (unveraendertes Alt-Verhalten), aber hier
    // gemerkt, damit computeGroundCorrection() danach saemtliche geloesten Placements exakt
    // darauf zurueckbiegen kann. Das macht die Erdung unbedingt exakt, selbst wenn OndselSolvers
    // Redundanz-Elimination (generische Gauss-Elimination mit Voll-Pivotisierung, kennt keine
    // Objekt-Identitaet) ausgerechnet die Erdungs-eigenen Gleichungen verwirft. groundedTargetObj
    // bleibt nullptr, wenn keine explizite Erdung existiert (z.B. waehrend eines Zwischenzustands)
    // - computeGroundCorrection() liefert dann Identity, unveraendertes Verhalten.
    App::DocumentObject* groundedTargetObj = nullptr;
    Base::Placement groundedTargetPlc;
    // FCPROJECT-PATCH (2026-10-27, jointtype-matrix belt__nested): alle in fixGroundedParts()
    // fixierten Solver-Koerper dieses Durchlaufs. setNewPlacements() schreibt sie nie - geerdet
    // heisst fest. computeGroundCorrection() biegt nur groundedTargetObj exakt zurueck; hat die
    // Redundanz-Elimination nach einem divergierten Newton die Drehgleichungen der Erdungen
    // verworfen, bekamen weitere geerdete Koerper (z.B. das Origin) sonst eine verdrehte Lage,
    // die mitgespeichert wurde.
    std::unordered_set<App::DocumentObject*> groundedBodies;

    std::unordered_map<App::DocumentObject*, MbDPartData> objectPartMap;
    // FCPROJECT-PATCH (Mehrfachinstanz-Fix - siehe
    // docs/ARCHITECTURE.md Abschnitt 4/5): die vormalige jointNestingPrefixMap (Original-Joint-
    // Zeiger -> nestingPrefix) ist ERSATZLOS entfernt - sie war nach Joint-Zeiger geschluesselt
    // und konnte deshalb, sobald DERSELBE reale Joint ueber ZWEI AssemblyLink-Instanzen desselben
    // verlinkten Dokuments erreicht wurde (zwei JointRef-Eintraege mit gleichem joint-Zeiger,
    // aber unterschiedlichem nestingPrefix - siehe getJoints()), nur EINEN der beiden Werte
    // halten (der zweite ueberschrieb den ersten kommentarlos). Betraf zusaetzlich den Aufbau der
    // MbD-Joint/Marker-Namen (bare joint->getFullName(), siehe jointInstanceName()) - derselbe
    // reale Joint landete dadurch ZWEIMAL mit IDENTISCHEM Namen im Solver. Das nestingPrefix wird
    // seit diesem Fix ausschliesslich ueber den JointRef-Rueckgabewert von getJoints() explizit
    // durch die gesamte Aufrufkette gereicht (resolvePartForMbD()/isMbDJointValid()/
    // handleOneSideOfJoint()/getRackPinionMarkers()/makeMbdJoint(), sowie jointParts()/
    // removeUnconnectedJoints()/traverseAndMarkConnectedParts()/getConnectedParts() ueber
    // vector<JointRef> statt vector<DocumentObject*>) - keine separate Member-Map mehr noetig.
    std::unordered_map<App::DocumentObject*, App::DocumentObject*> rigidRepByPart;
    std::unordered_map<App::DocumentObject*, std::vector<App::DocumentObject*>> rigidMembersByRep;
    std::unordered_map<App::DocumentObject*, Base::Placement> rigidPlacementCache;
    std::vector<std::pair<App::DocumentObject*, double>> objMasses;
    std::vector<App::DocumentObject*> draggedParts;
    std::vector<App::DocumentObject*> motions;

    std::vector<std::pair<App::DocumentObject*, Base::Placement>> previousPositions;

    bool bundleFixed;

    // Siehe setActiveEditContext()-Deklaration weiter oben.
    bool activeEditContext {false};

    // True while solve() is running; breaks the solve()/updateSolveStatus() cycle.
    bool solveInProgress {false};

    int lastDoF;
    bool lastHasConflict;
    bool lastHasRedundancies;
    bool lastHasPartialRedundancies;
    bool lastHasMalformedConstraints;
    int lastSolverStatus;

    std::vector<std::string> lastRedundantJoints;
    // FCPROJECT-PATCH (2026-10-11): je ueberbestimmter Gruppe eine Zeile "Verursacher (Typ),
    // zusammen mit: ..." aus System::redundancyGroups (siehe updateSolveStatus()).
    std::vector<std::string> lastRedundancyGroups;

    // FCPROJECT-PATCH (2026-10-13, Nutzerentscheidung "Aenderung im Original"): Grenzen, die der
    // letzte Solve angelegt hat (makeMbdJoint()), und je verletzt gebliebener Grenze eine Zeile
    // "Gelenk (Typ): Wert ausserhalb min..max" (reportUnsatisfiedLimits()).
    struct LimitCheck
    {
        App::DocumentObject* joint;
        std::string nestingPrefix;
        bool isAngle;
        bool minEnabled;
        bool maxEnabled;
        double min;
        double max;
    };
    std::vector<LimitCheck> activeLimits;

    // FCPROJECT-PATCH (2026-10-14, Plan plans/2026-10-14-redundanz-mit-toleranz.md, Regel C7):
    // Redundanzen des letzten Solves, nur im Speicher. Solange alle gemerkten Gelenke noch
    // unveraendert im Modell stecken, nimmt der naechste Solve dieselben Bedingungen vorab heraus
    // (OndselSolver sucht sonst bei jedem Solve von vorn - ~80% der Solve-Zeit an BG22).
    struct RedundancyCache
    {
        std::set<std::string> signatures;
        std::set<std::pair<std::string, std::size_t>> removed;
        // davon ueber der Toleranz (bekannter Widerspruch aus einem frischen Solve)
        std::set<std::pair<std::string, std::size_t>> conflicts;
        std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    };
    bool newRedundancyConflict = false;
    RedundancyCache redundancyCache;
    // solve() aus preDrag() buendelt Fixed-Ketten (bundleFixed) - ein anderes Modell, eigenes
    // Gedaechtnis, sonst ueberschreiben sich beide gegenseitig.
    RedundancyCache bundledRedundancyCache;
    RedundancyCache dragRedundancyCache;
    RedundancyCache& currentRedundancyCache()
    {
        return bundleFixed ? bundledRedundancyCache : redundancyCache;
    }
    bool redundancyCacheApplied = false;
    // je herausgenommener Bedingung eine Zeile "Gelenk: Abweichung ..., geschluckt|Konflikt"
    std::vector<std::string> lastRedundancyStates;
    std::set<std::string> mbdJointSignatures() const;
    bool applyRedundancyCache(const RedundancyCache& cache);
    void setJointPriorities(const std::vector<JointRef>& jointRefs);
    std::vector<std::string> lastLimitViolations;
    std::optional<Base::Placement> getJcsInContext(
        App::DocumentObject* joint,
        const char* refName,
        const char* plcName,
        const std::string& nestingPrefix
    );
    void reportUnsatisfiedLimits();
    std::vector<std::string> lastConflictingJoints;
    std::vector<std::string> lastPartialRedundantJoints;
    std::vector<std::string> lastMalformedJoints;
};

}  // namespace Assembly
