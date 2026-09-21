// SPDX-License-Identifier: LGPL-2.1-or-later
/****************************************************************************
 *                                                                          *
 *   Copyright (c) 2026 Ondsel <development@ondsel.com>                     *
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
#include <functional>

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectGroup.h>
#include <App/Link.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>

#include <Base/Placement.h>
#include <Base/Tools.h>

#include "AssemblyIdentityGraph.h"
#include "AssemblyLink.h"
#include "AssemblyObject.h"
#include "AssemblyUtils.h"

namespace Assembly
{

namespace
{
// hasSiblingInstances() erwartet eine Kandidatenliste als vector<App::DocumentObject*> -
// getSubAssemblies() liefert vector<AssemblyLink*>. Kleine Konvertierung, analog zu
// canonicalizeForMbD()'s eigener siblingCandidatesFor()-Lambda.
std::vector<App::DocumentObject*> candidatesFor(AssemblyObject* solvingAssembly, AssemblyLink* currentContainer)
{
    if (currentContainer) {
        return currentContainer->Group.getValues();
    }
    auto subs = solvingAssembly->getSubAssemblies();
    return std::vector<App::DocumentObject*>(subs.begin(), subs.end());
}

// Top-Down-Struktursuche, analog zu AssemblyObject.cpp's gleichnamiger (dort anonymer, hier
// nicht wiederverwendbarer) Hilfsfunktion - siehe canonicalizeForMbD()'s Kommentar fuer die
// Begruendung (Group-Mitgliedschaft statt InList-Aufstieg, da InList auch referenzierende
// Joints faelschlich als "Elternknoten" treffen kann).
bool findLocalGroupPath(
    const std::vector<App::DocumentObject*>& objects,
    App::DocumentObject* target,
    std::vector<AssemblyLink*>& outPath
)
{
    for (auto* candidate : objects) {
        if (!candidate) {
            continue;
        }
        if (candidate == target) {
            return true;
        }
        if (auto* asmLink = freecad_cast<AssemblyLink*>(candidate)) {
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
// FCPROJECT-PATCH (2026-09-19, live am echten BG37/BG43/BG67-Projekt gefunden): findLocalGroupPath()
// alleine findet 'target' nur, wenn es innerhalb DESSELBEN Dokuments wie 'scope' liegt - fuer ein
// tief verschachteltes, ueber MEHRERE ECHTE Dokumentgrenzen erreichtes Objekt (z.B. ein Blatt in
// BG67s eigenem Dokument, erreicht ueber BG37->BG43->BG67) liefert es faelschlich "nicht
// gefunden", selbst wenn 'target' strukturell durchaus erreichbar ist. findContainerChain()
// generalisiert das: zuerst lokal versuchen (deckt den bisherigen, haeufigsten Fall
// unveraendert ab), sonst REKURSIV in JEDE eigene, nicht-rigide Unterbaugruppe absteigen und dort
// erneut versuchen - dadurch spielt es keine Rolle, wie viele echte Dokumentgrenzen zwischen
// 'scope' und 'target' liegen. 'chain' sammelt dabei JEDE durchquerte AssemblyLink-Instanz,
// aeusserste zuerst - das Produkt ihrer Placement-Werte ist exakt die Transformation, die
// 'target's lokale Placement in Weltkoordinaten relativ zu 'scope' umrechnet.
bool findContainerChain(
    AssemblyObject* scope,
    App::DocumentObject* target,
    std::vector<AssemblyLink*>& chain
)
{
    std::vector<AssemblyLink*> localPath;
    if (findLocalGroupPath(scope->Group.getValues(), target, localPath)) {
        chain.insert(chain.end(), localPath.begin(), localPath.end());
        return true;
    }
    for (auto* asmLink : scope->getSubAssemblies()) {
        if (!asmLink || asmLink->isRigid()) {
            continue;
        }
        AssemblyObject* nested = asmLink->getLinkedAssembly();
        if (!nested || nested == scope) {
            continue;
        }
        chain.push_back(asmLink);
        if (findContainerChain(nested, target, chain)) {
            return true;
        }
        chain.pop_back();
    }
    return false;
}

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau Phase 1, "sechste Baustelle unter aeusserer
// Duplikation"): resolveJointRef()/resolve() koennen einen "Blatt"-Kandidaten finden, der SELBST
// noch ein lokaler Spiegel INNERHALB des zuletzt gekreuzten Containers ('localContext') ist -
// z.B. ein Joint, der direkt auf "MidLink.SubLink.BoxB" zeigt, aber nur ueber EIN nestingPrefix
// ("MidLink.") erreicht wird (der Joint selbst lebt bereits in "Mid", referenziert von dort aus
// direkt Mids EIGENEN Spiegel von Subs BoxB - "SubLink.BoxB" ist dabei KEIN nestingPrefix-Sprung,
// sondern bereits Teil des in der Referenz gespeicherten lokalen Pfads). Ohne diese Verfeinerung
// bliebe das Ergebnis bei diesem noch-nicht-vollstaendig aufgeloesten Zwischen-Spiegel stehen -
// bei zusaetzlicher Duplikation von 'localContext' selbst (z.B. Mid ist die duplizierte Ebene)
// fuehrt das dazu, dass materializeForObjectPartMap() weder das tiefe echte Objekt noch einen
// gueltigen, ueber mirrorsOf() auffindbaren lokalen Spiegel bekommt (mirrorsOf() sucht nur
// innerhalb der GRAND-lokalen Instanz, "SubLink.BoxB" als MID-Dokument-Objekt liegt da nie).
//
// Ablauf: 'obj' liegt (strukturell, per findLocalGroupPath) irgendwo innerhalb
// localContext->getLinkedAssembly()'s EIGENEM Dokument. Jede dabei durchquerte Ebene ist ein
// ECHTES (nicht gespiegeltes) Objekt AUS SICHT dieses Dokuments - wird per FORWARD objLinkMap
// (Quelle -> lokaler Spiegel DIESER Instanz) auf den naechsttieferen lokalen Kontext uebersetzt,
// GENAU DIE GEGENRICHTUNG zu resolveObject()'s getSourceForMirror()-Walk (der von einem LOKALEN
// Spiegel aus RUECKWAERTS zur Quelle aufloest - hier ist der Ausgangspunkt bereits die Quelle,
// weil wir per Konstruktion in einem GETEILTEN/Vorlage-Dokument stehen). 'obj' selbst kann dabei
// SELBST schon ein lokaler Spiegel sein (z.B. Mids eigener Spiegel von Subs BoxB) - wird zuerst
// per getSourceForMirror() auf die ECHTE Quelle zurueckgefuehrt, DANN ueber den inzwischen
// aufgebauten lokalen Kontext forward uebersetzt.
//
// path.empty() (obj ist bereits ein DIREKTES Kind von localContext->getLinkedAssembly()'s
// eigener Gruppe) ist bewusst ein Nullop: das bedeutet, 'obj' ist bereits GENAU das, was der
// bisherige Walk (nestingPrefix-Kette) erwartungsgemaess liefern sollte (z.B. ein Joint, der
// INNERHALB von Sub selbst definiert ist und Subs eigenes BoxB direkt referenziert) - eine
// weitere Vorwaerts-Uebersetzung wuerde hier FAELSCHLICH einen noch tieferen Spiegel liefern,
// obwohl "Adressieren statt Kopieren" (docs/ARCHITECTURE.md §5) hier explizit das tiefe ECHTE
// Objekt haben will.
App::DocumentObject* refineNestedMirrorTarget(
    AssemblyLink* localContext,
    App::DocumentObject* obj,
    std::vector<AssemblyLink*>& duplicatePath
)
{
    AssemblyObject* scope = localContext->getLinkedAssembly();
    if (!scope) {
        return obj;
    }

    std::vector<AssemblyLink*> path;
    if (!findLocalGroupPath(scope->Group.getValues(), obj, path)) {
        return obj;
    }
    if (path.empty()) {
        return obj;
    }

    for (auto* realLink : path) {
        auto it = localContext->objLinkMap.find(realLink);
        if (it == localContext->objLinkMap.end()) {
            return obj;
        }
        auto* localMirror = freecad_cast<AssemblyLink*>(it->second);
        if (!localMirror) {
            return obj;
        }
        if (hasSiblingInstances(localContext->Group.getValues(), localMirror)) {
            duplicatePath.push_back(localMirror);
        }
        localContext = localMirror;
    }

    App::DocumentObject* realSource = path.back()->getSourceForMirror(obj);
    if (!realSource) {
        realSource = obj;
    }

    // FCPROJECT-PATCH (2026-09-19, Korrektur nach Verifikations-Fehlschlag gegen
    // test_fixed_double_nested_flex & Co.): Vorwaerts-Uebersetzung auf den lokalen Spiegel nur
    // anwenden, wenn IRGENDWO auf dem GESAMTEN bisherigen Pfad (vor UND waehrend dieser
    // Verfeinerung, siehe 'duplicatePath' - vom Aufrufer u.U. schon vorbelegt) tatsaechlich eine
    // Instanz-Duplikation gefunden wurde. Ohne jede Duplikation will "Adressieren statt
    // Kopieren" (docs/ARCHITECTURE.md §5) explizit das tiefe ECHTE Objekt - ein erster Versuch,
    // hier IMMER vorwaerts zu uebersetzen, lieferte bei jedem NICHT duplizierten Fall
    // faelschlich einen (unnoetigen) lokalen Spiegel statt 'realSource' selbst.
    if (duplicatePath.empty()) {
        return realSource;
    }
    auto it = localContext->objLinkMap.find(realSource);
    return it != localContext->objLinkMap.end() ? it->second : realSource;
}
}  // namespace

bool IdentityHandle::operator==(const IdentityHandle& other) const
{
    return templateObj == other.templateObj && duplicateInstancePath == other.duplicateInstancePath;
}

std::size_t IdentityHandleHash::operator()(const IdentityHandle& h) const
{
    std::size_t seed = std::hash<const void*> {}(h.templateObj);
    for (auto* link : h.duplicateInstancePath) {
        // Boost-style hash_combine, ohne die Boost-Abhaengigkeit hier zu brauchen.
        seed ^= std::hash<const void*> {}(link) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
}

IdentityGraph::IdentityGraph(AssemblyObject* root)
    : rootAssembly(root)
{}

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau, Phase 0): generalisiert
// AssemblyUtils::getMovingPartFromSel()'s Segment-fuer-Segment-Namens-Walk. 'localContext' dient
// NUR der Buchfuehrung (korrekter lokaler Kontext fuer hasSiblingInstances() + begehbare
// Instanz-Zeiger fuer duplicateInstancePath) - der eigentliche Walk ('obj'/'doc') bleibt ROH wie
// im Original, siehe ausfuehrliche Begruendung am selben Muster in resolveJointRef() (ein
// frueherer Versuch, HIER ebenfalls durchgehend zu uebersetzen, lieferte bei jedem NICHT
// duplizierten verschachtelten Fall faelschlich den aeusseren lokalen Spiegel statt des tiefen
// echten/gerenderten Objekts).
IdentityHandle IdentityGraph::resolve(App::DocumentObject* obj, const std::string& subPath)
{
    if (!obj) {
        return {};
    }

    App::Document* doc = obj->getDocument();
    auto names = Base::Tools::splitSubName(subPath);
    names.insert(names.begin(), obj->getNameInDocument());

    bool assemblyPassed = false;
    AssemblyLink* localContext = nullptr;
    App::DocumentObject* previousObj = nullptr;
    std::vector<AssemblyLink*> duplicatePath;

    for (const auto& objName : names) {
        obj = doc->getObject(objName.c_str());
        if (!obj && previousObj) {
            // Gleicher Fallback wie getMovingPartFromSel(): dieser Name gehoert zu einem
            // Top-Dokument-Spiegel, der als Kind des zuletzt aufgeloesten Objekts lebt.
            if (auto* prevAsGroup = freecad_cast<AssemblyLink*>(previousObj)) {
                for (auto* candidate : prevAsGroup->Group.getValues()) {
                    if (candidate && objName == candidate->getNameInDocument()) {
                        obj = candidate;
                        doc = obj->getDocument();
                        break;
                    }
                }
            }
        }
        if (!obj) {
            continue;
        }
        previousObj = obj;

        // 'localObj' ist NUR die Buchfuehrungs-Variable - beeinflusst NICHT den rohen Walk.
        App::DocumentObject* localObj = obj;
        if (localContext) {
            auto it = localContext->objLinkMap.find(obj);
            if (it != localContext->objLinkMap.end()) {
                localObj = it->second;
            }
        }

        if (obj->isLink()) {
            if (auto* linkedObj = obj->getLinkedObject()) {
                doc = linkedObj->getDocument();
            }
        }

        if (obj == rootAssembly) {
            assemblyPassed = true;
            continue;
        }
        if (!assemblyPassed) {
            continue;
        }

        if (obj->isDerivedFrom<App::DocumentObjectGroup>()) {
            continue;
        }
        if (obj->isLinkGroup()) {
            continue;
        }

        if (auto* assemblyLink = freecad_cast<AssemblyLink*>(localObj)) {
            const bool rigid = assemblyLink->isRigid();
            if (!rigid) {
                if (auto* linkedAssembly = assemblyLink->getLinkedAssembly()) {
                    if (hasSiblingInstances(candidatesFor(rootAssembly, localContext), assemblyLink)) {
                        duplicatePath.push_back(assemblyLink);
                    }
                    localContext = assemblyLink;
                    doc = linkedAssembly->getDocument();
                }
                continue;
            }
            // Rigid: wie ein Blatt behandeln, faellt durch zum "gefunden"-Fall unten.
        }

        // FCPROJECT-PATCH (2026-09-19, "sechste Baustelle unter aeusserer Duplikation" - siehe
        // ausfuehrlichen Kommentar an refineNestedMirrorTarget()): dasselbe Muster wie in
        // resolveJointRef().
        IdentityHandle result;
        if (localContext) {
            result.templateObj = refineNestedMirrorTarget(localContext, obj, duplicatePath);
        }
        else {
            IdentityHandle canonical = resolveObject(obj);
            result.templateObj = canonical.templateObj ? canonical.templateObj : obj;
            duplicatePath = canonical.duplicateInstancePath;
        }
        result.duplicateInstancePath = duplicatePath;
        return result;
    }

    return {};
}

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau, Phase 0): generalisiert
// AssemblyObject::canonicalizeForMbD() - siehe deren ausfuehrlichen Kommentar am
// Definitionsort (AssemblyObject.cpp) fuer die volle Herleitung des Algorithmus, den diese
// Fassung nachbildet. Einziger Verhaltensunterschied: statt bei der ERSTEN gefundenen
// Instanz-Duplikation sofort mit dem unveraenderten Eingabe-'obj' aufzugeben, wird die
// Duplikation in duplicateInstancePath vermerkt und die Uebersetzung fortgesetzt.
namespace
{
// FCPROJECT-PATCH (2026-09-19, live am echten BG37/BG43/BG67-Projekt gefunden, siehe
// findContainerChain() fuer dieselbe Korrektur bei containerChainPlacement()): die
// urspruengliche Fassung delegierte nur EINE Ebene tief (nur wenn eine UNMITTELBARE
// Unterbaugruppe von 'scope' SELBST bereits 'obj's Dokument ist) - fuer ein Objekt, das ueber
// ZWEI ODER MEHR echte Dokumentgrenzen erreicht wird (z.B. ein Blatt in BG67s eigenem Dokument,
// erreicht ueber BG37->BG43->BG67), scheiterte das und 'obj' kam unveraendert zurueck, obwohl es
// strukturell durchaus erreichbar ist. Diese Fassung probiert stattdessen REKURSIV jede eigene,
// nicht-rigide Unterbaugruppe (unabhaengig davon, ob deren EIGENES Dokument direkt passt) und
// gibt explizit zurueck, ob 'obj' irgendwo im durchsuchten Unterbaum gefunden wurde - der
// bisherige, indirekte "IdentityHandle mit obj unveraendert" Rueckgabewert konnte "nicht
// gefunden" nicht zuverlaessig von "gefunden, aber keine Uebersetzung noetig" unterscheiden.
bool resolveObjectIn(AssemblyObject* scope, App::DocumentObject* obj, IdentityHandle& outHandle)
{
    std::vector<AssemblyLink*> path;
    if (findLocalGroupPath(scope->Group.getValues(), obj, path)) {
        AssemblyLink* previousLink = nullptr;
        std::vector<AssemblyLink*> duplicatePath;
        for (auto* mirrorLink : path) {
            if (hasSiblingInstances(candidatesFor(scope, previousLink), mirrorLink)) {
                duplicatePath.push_back(mirrorLink);
            }

            App::DocumentObject* levelReal = mirrorLink;
            if (previousLink) {
                levelReal = previousLink->getSourceForMirror(mirrorLink);
                if (!levelReal) {
                    outHandle.templateObj = obj;
                    outHandle.duplicateInstancePath = duplicatePath;
                    return true;
                }
            }
            auto* realAsmLink = freecad_cast<AssemblyLink*>(levelReal);
            if (!realAsmLink || realAsmLink->isRigid()) {
                outHandle.templateObj = levelReal;
                outHandle.duplicateInstancePath = duplicatePath;
                return true;
            }
            previousLink = realAsmLink;
        }

        if (path.empty()) {
            outHandle.templateObj = obj;
            return true;
        }
        // FCPROJECT-PATCH (2026-09-21, live an der echten CNC3018_022-Datei gefunden, per
        // IdentityGraph::exportDot() erstmals sichtbar gemacht - siehe
        // [[todo-mirrortosourcemap-missing-for-nested-assemblylink]] fuer die volle Herleitung):
        // die Schleife oben prueft hasSiblingInstances() nur fuer jede CONTAINER-Ebene in 'path'
        // (die Kette der AssemblyLinks, die durchquert werden MUESSEN, um 'obj' zu erreichen) -
        // 'obj' SELBST kann aber ZUSAETZLICH innerhalb path.back()s eigener Gruppe dupliziert
        // sein (z.B. Slot1 vs. Slot2 einer Schiene - beide "CNC3018_023_A_Halterbaugruppe",
        // dieselbe Vorlage, zwei Instanzen NEBENEINANDER in derselben FuerungsBaugruppe). Ohne
        // diesen Check kollabieren beide Slots auf dieselbe IdentityHandle (gleiches templateObj
        // via getSourceForMirror(), gleiches duplicateInstancePath OHNE diese letzte Ebene) -
        // genau das liess zwei tatsaechlich verschiedene physische Teile im Diagramm (und im
        // Solver, siehe canonicalizeForMbD()) als EINE Identitaet erscheinen.
        if (auto* objAsLink = freecad_cast<AssemblyLink*>(obj)) {
            if (hasSiblingInstances(path.back()->Group.getValues(), objAsLink)) {
                duplicatePath.push_back(objAsLink);
            }
        }
        App::DocumentObject* resolved = path.back()->getSourceForMirror(obj);
        outHandle.templateObj = resolved ? resolved : obj;
        outHandle.duplicateInstancePath = duplicatePath;
        return true;
    }

    for (auto* asmLink : scope->getSubAssemblies()) {
        if (!asmLink || asmLink->isRigid()) {
            continue;
        }
        AssemblyObject* nested = asmLink->getLinkedAssembly();
        if (!nested || nested == scope) {
            continue;
        }
        if (resolveObjectIn(nested, obj, outHandle)) {
            return true;
        }
    }

    // FCPROJECT-PATCH (2026-09-20, live am echten BG37/BG67-Projekt gefunden: Motor-Getriebezug
    // komplett "unreachable", Solve friert alle Placements ein): 'obj' wurde NIRGENDS in der
    // Group-Struktur gefunden (weder direkt noch in einer nicht-rigiden Unterbaugruppe) - kann
    // ein FREISTEHENDES App::Link sein, das NUR als Joint-Referenz-Ziel existiert und NIE in
    // irgendein Group eingehaengt wurde (live bestaetigt: Joint008s Reference2-Ziel "048_Link"
    // hat als einzigen InList-Eintrag den Joint selbst, ist kein Mitglied von rootAssembly.Group).
    // So ein Link zeigt aber trotzdem, wie jeder andere Spiegel, per LinkedObject auf das
    // eigentliche native Objekt (hier: das gleichnamige Link INNERHALB der Unterbaugruppe, das
    // deren eigene interne Joints bereits als kanonisch verwenden) - dem EINEN Schritt folgen und
    // ERNEUT im selben Scope aufloesen (findet das Ziel dann ganz normal ueber die
    // Group-Traversierung oben, oder eskaliert selbst weiter in getSubAssemblies()). 'obj' bleibt
    // dabei unveraendert - getLinkedObject() fuehrt immer zu einem ANDEREN Objekt (keine Zyklen in
    // FreeCADs Link-Graphen moeglich), die Rekursion terminiert also. Ohne diese Aufloesung
    // liefern der aeussere Anker-Joint und die INTERNEN Joints der Unterbaugruppe zwei
    // verschiedene Zeiger fuer dasselbe physische Teil - genau das brach die Erreichbarkeits-
    // Traversierung (removeUnconnectedJoints()) fuer den GESAMTEN Getriebezug.
    if (obj->isLink()) {
        if (auto* linked = obj->getLinkedObject(false)) {
            if (linked != obj && resolveObjectIn(scope, linked, outHandle)) {
                return true;
            }
        }
    }
    return false;
}
}  // namespace

IdentityHandle IdentityGraph::resolveObject(App::DocumentObject* obj)
{
    if (!obj) {
        return {};
    }
    IdentityHandle result;
    if (resolveObjectIn(rootAssembly, obj, result)) {
        return result;
    }
    result.templateObj = obj;
    return result;
}

Base::Placement IdentityGraph::containerChainPlacement(App::DocumentObject* obj)
{
    if (!obj) {
        return {};
    }
    std::vector<AssemblyLink*> chain;
    if (!findContainerChain(rootAssembly, obj, chain)) {
        return {};
    }
    Base::Placement result;
    for (auto* link : chain) {
        result = result * link->Placement.getValue();
    }
    return result;
}

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau, Phase 0): generalisiert
// AssemblyUtils::resolveJointReference() - siehe deren selbst-dokumentierte Ein-Sprung-Grenze
// (AssemblyUtils.cpp ~1067-1073). Gleiches Prinzip wie resolve() oben, aber ausgehend von einer
// Joint-Referenz-Property statt einer GUI-Selektions-Adresse (kein assemblyPassed-Gate noetig,
// da nestingPrefix den Walk bereits "innerhalb" von rootAssembly beginnen laesst).
IdentityHandle IdentityGraph::resolveJointRef(
    App::DocumentObject* joint,
    const char* propName,
    const std::string& nestingPrefix
)
{
    if (!joint || !propName) {
        return {};
    }

    const auto* prop = joint->getPropertyByName<App::PropertyXLinkSub>(propName);
    if (!prop) {
        return {};
    }

    App::DocumentObject* refObj = prop->getValue();
    if (!refObj) {
        return {};
    }

    const std::vector<std::string> subs = prop->getSubValues();
    const std::string localSub = subs.empty() ? std::string() : subs[0];

    std::string fullAddress = nestingPrefix + refObj->getNameInDocument();
    if (!localSub.empty()) {
        fullAddress += "." + localSub;
    }

    const auto names = Base::Tools::splitSubName(fullAddress);
    if (names.empty()) {
        return {};
    }

    App::Document* doc = rootAssembly->getDocument();
    if (!doc) {
        return {};
    }

    // FCPROJECT-PATCH (2026-09-19, Korrektur nach Verifikations-Fehlschlag gegen
    // test_fixed_nested_flex & Co.): ZWEI getrennte Zustaende werden parallel mitgefuehrt, nicht
    // nur einer:
    // - 'obj'/'doc' (unten): der ROHE, ueber die geteilten/verlinkten Dokumente laufende Walk -
    //   GENAU wie AssemblyUtils::resolveJointReference()'s bestehender Walk. Sein Endergebnis
    //   wird als IdentityHandle::templateObj uebernommen - das tiefste ECHTE Objekt, exakt was
    //   "Adressieren statt Kopieren" (docs/ARCHITECTURE.md §5) fuer den Solver haben will, WENN
    //   keine Duplikation vorliegt (ein erster Versuch, hier IMMER ueber objLinkMap zu
    //   uebersetzen, ergab bei jedem NICHT duplizierten verschachtelten Fall faelschlich den
    //   AEUSSEREN lokalen Spiegel statt des tiefen echten Objekts - siehe Git-Historie dieses
    //   Kommentars/Commits fuer den Befund).
    // - 'localContext' (AssemblyLink*): NUR fuer Buchfuehrung - der zuletzt gekreuzte Container,
    //   IMMER instanzaufgeloest (transitiv uebersetzt), genutzt um (a) hasSiblingInstances() im
    //   jeweils korrekten lokalen Kontext zu pruefen (unabhaengig davon, ob dieser konkrete
    //   Sprung selbst dupliziert ist) und (b) JEDE tatsaechlich als dupliziert erkannte Ebene mit
    //   einem echten, direkt begehbaren lokalen Zeiger in duplicateInstancePath einzutragen.
    AssemblyLink* localContext = nullptr;
    std::vector<AssemblyLink*> duplicatePath;

    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& objName = names[i];
        if (objName.empty()) {
            break;
        }

        App::DocumentObject* obj = doc->getObject(objName.c_str());
        if (!obj) {
            // Bewusst defensiv wie im Original: kein Rateversuch, der Aufrufer faellt auf
            // getMovingPartFromRef() zurueck.
            return {};
        }

        // 'localObj' ist NUR die Buchfuehrungs-Variable (siehe Kommentar oben) - beeinflusst
        // NICHT den rohen Walk (doc-Umschaltung/'obj' bleiben unveraendert).
        App::DocumentObject* localObj = obj;
        if (localContext) {
            auto it = localContext->objLinkMap.find(obj);
            if (it != localContext->objLinkMap.end()) {
                localObj = it->second;
            }
        }

        if (obj->isLink()) {
            if (auto* linkedObj = obj->getLinkedObject()) {
                doc = linkedObj->getDocument();
            }
        }

        if (auto* assemblyLink = freecad_cast<AssemblyLink*>(localObj)) {
            if (auto* linkedAssembly = assemblyLink->getLinkedAssembly()) {
                doc = linkedAssembly->getDocument();
                // FCPROJECT-PATCH (2026-09-20, live am echten BG22-Projekt gefunden: verschachtelte
                // Duplikation - 3x FuehrungsBaugruppe, je 2x Halterbaugruppe - bricht die
                // Erreichbarkeit): candidatesFor(rootAssembly, localContext) geht bei localContext
                // == nullptr (erster Sprung) IMMER von rootAssembly->getSubAssemblies() als
                // Geschwister-Vergleichsbasis aus - das setzt voraus, dass 'assemblyLink' ein
                // DIREKTES Kind von rootAssembly ist. Trifft NICHT zu, wenn die Joint-Referenz
                // direkt einen von einer NICHT-rigiden Elternbaugruppe promoteten Spiegel
                // referenziert, der strukturell eine oder mehrere Ebenen TIEFER liegt (z.B. ein
                // Rahmen-Joint, der direkt auf "Halterbaugruppe" zeigt, obwohl diese tatsaechlich
                // INNERHALB einer dazwischenliegenden "FuehrungsBaugruppe"-Instanz lebt) - dann
                // wird gegen die FALSCHEN Geschwister (rootAssembly's eigene Unterbaugruppen statt
                // die tatsaechlichen Geschwister-Instanzen INNERHALB der Zwischenebene) geprueft,
                // hasSiblingInstances() liefert faelschlich false, und die dortige Duplikation
                // bleibt unerkannt - mit der Folge, dass diese Route und die Route ueber die
                // eigenen internen Joints der Zwischenbaugruppe (die die Duplikation SEHR wohl
                // erkennt) auf zwei verschiedene Identitaeten fuer dasselbe physische Teil laufen.
                // Fix: bei localContext == nullptr zuerst pruefen, WO 'assemblyLink' strukturell
                // tatsaechlich liegt (top-down, wie resolveObjectIn()) - liegt es tiefer als ein
                // direktes rootAssembly-Kind, dessen ECHTEN unmittelbaren Container als
                // Geschwister-Vergleichsbasis verwenden statt rootAssembly selbst.
                std::vector<App::DocumentObject*> siblingCandidates;
                if (!localContext) {
                    std::vector<AssemblyLink*> structuralPath;
                    if (findLocalGroupPath(rootAssembly->Group.getValues(), assemblyLink, structuralPath)
                        && !structuralPath.empty()) {
                        siblingCandidates = structuralPath.back()->Group.getValues();
                    }
                }
                if (siblingCandidates.empty()) {
                    siblingCandidates = candidatesFor(rootAssembly, localContext);
                }
                if (hasSiblingInstances(siblingCandidates, assemblyLink)) {
                    duplicatePath.push_back(assemblyLink);
                }
                localContext = assemblyLink;
            }
        }

        if (obj->isDerivedFrom<App::DocumentObjectGroup>()) {
            continue;
        }
        if (obj->isLinkGroup()) {
            continue;
        }
        if (obj->isDerivedFrom<AssemblyLink>()) {
            // FCPROJECT-PATCH (2026-09-20, live am echten BG22-Projekt gefunden: verschachtelte
            // Duplikation - 3x FuehrungsBaugruppe, je 2x Halterbaugruppe - bricht die
            // Erreichbarkeit): 'obj' ist hier der ROHE, geteilte/verlinkte Walk-Zeiger - bei einer
            // Unterbaugruppe, die SELBST innerhalb einer weiteren Ebene dupliziert vorkommt (hier:
            // Halterbaugruppe, zweimal INNERHALB jeder FuehrungsBaugruppe-Instanz), ist das der
            // EINE geteilte Vorlage-Zeiger, unabhaengig davon, ueber welche der 3 aeusseren
            // FuehrungsBaugruppe-Instanzen man ihn erreicht - sein "Rigid"-Flag gehoert aber zu
            // GENAU DIESER einen Vorlage-Definition, nicht zwingend zu dem, was die AEUSSERE
            // Instanz fuer ihre EIGENE, promotete Spiegel-Kopie separat gesetzt hat. Ein direkter
            // Joint auf Baugruppen-Ebene (nestingPrefix="", localContext bereits beim ERSTEN
            // Sprung gesetzt) liest 'obj' == 'localObj' (kein Unterschied) und stoppt korrekt an
            // der rigiden Grenze; ein ueber subJoints() mit nestingPrefix hochgezogener interner
            // Joint derselben Unterbaugruppe erreicht dieselbe Vorlage 'obj' aber NACH einem
            // Container-Sprung, waehrend 'localObj' bereits auf die instanzeigene Spiegel-Kopie
            // uebersetzt ist - NUR 'localObj' traegt deren tatsaechlich fuer DIESE Instanz
            // gueltiges Rigid-Flag. Ohne diese Umstellung liest der Rigid-Check hier
            // widerspruechlich mal die Vorlage, mal (an anderer Stelle, s.o. der
            // AssemblyLink-Cast/Duplikations-Check) den Spiegel - mit der Folge, dass ein Joint,
            // der DIREKT auf den aeusseren Spiegel zeigt, an der rigiden Grenze stoppt, waehrend
            // der interne, ueber subJoints() hochgezogene Joint DURCH dieselbe rigide Grenze
            // hindurch bis zum tiefsten echten Teil weiterlaeuft - zwei verschiedene Identitaeten
            // fuer dasselbe physische Teil, genau das brach die Erreichbarkeits-Traversierung.
            const auto* pRigid = localObj->getPropertyByName<App::PropertyBool>("Rigid");
            if (pRigid && !pRigid->getValue()) {
                continue;
            }
        }

        // FCPROJECT-PATCH (2026-09-19, "sechste Baustelle unter aeusserer Duplikation"): 'obj'
        // kann SELBST noch ein noch nicht vollstaendig aufgeloester lokaler Spiegel sein (siehe
        // refineNestedMirrorTarget()-Kommentar) - zwei Faelle, je nachdem ob ueberhaupt schon
        // eine Container-Grenze gekreuzt wurde:
        //
        // FCPROJECT-PATCH (2026-09-21, live an der echten CNC3018_022-Datei gefunden, siehe
        // [[todo-mirrortosourcemap-missing-for-nested-assemblylink]]): DRITTER Fall ergaenzt -
        // 'localContext' kann bei einem DIREKTEN Joint-Bezug (nestingPrefix="") schon beim
        // ERSTEN Sprung auf 'obj' SELBST gesetzt worden sein (obj ist bereits das gefundene
        // AssemblyLink, keine tiefere Verschachtelung mehr noetig). refineNestedMirrorTarget()
        // ist aber NUR fuer den Fall gedacht, dass 'obj' TIEFER liegt als 'localContext' (sucht
        // per findLocalGroupPath() INNERHALB localContext->getLinkedAssembly()) - bei
        // localContext == obj sucht es 'obj' faelschlich innerhalb SEINER EIGENEN verlinkten
        // Baugruppe (findet sich dort nie, da obj selbst kein Kind von sich selbst ist) und gibt
        // obj unveraendert zurueck, OHNE es ueber seinen EIGENEN, umschliessenden Kontext auf die
        // geteilte Identitaet zu reduzieren (getSourceForMirror() wird nie erreicht). Das liess
        // z.B. eine top-level rigide AssemblyLink-Instanz (direkt vom aeusseren Joint
        // referenziert) und dieselbe physische Baugruppe, ueber einen INNEREN Joint (mit
        // nestingPrefix) erreicht, auf zwei verschiedene Identitaeten laufen. Fix: in GENAU
        // diesem Fall stattdessen resolveObject(obj) nutzen (identische Top-Down-Struktursuche +
        // getSourceForMirror(), wie im "kein localContext"-Zweig unten) statt
        // refineNestedMirrorTarget().
        IdentityHandle result;
        if (localContext && freecad_cast<AssemblyLink*>(obj) != localContext) {
            result.templateObj = refineNestedMirrorTarget(localContext, obj, duplicatePath);
        }
        else {
            // Kein nestingPrefix-Sprung noetig, um 'obj' zu finden (typisch: ein Joint im
            // JointGroup DIESER Baugruppe referenziert direkt einen - evtl. mehrfach
            // verschachtelten - lokalen Spiegel, z.B. "MidLink.SubLink.BoxB") - resolveObject()
            // loest das bereits vollstaendig auf (Top-Down-Struktursuche + getSourceForMirror()).
            IdentityHandle canonical = resolveObject(obj);
            result.templateObj = canonical.templateObj ? canonical.templateObj : obj;
            duplicatePath = canonical.duplicateInstancePath;
        }
        result.duplicateInstancePath = duplicatePath;
        return result;
    }

    return {};
}

SolverHandle IdentityGraph::resolveForSolver(const IdentityHandle& h)
{
    return SolverHandle(h);
}

UiHandle IdentityGraph::resolveForUi(const IdentityHandle& h)
{
    auto mirrors = mirrorsOf(h);
    App::DocumentObject* local = mirrors.empty() ? h.templateObj : mirrors.front();
    return UiHandle(h, local);
}

App::DocumentObject* IdentityGraph::materialize(const SolverHandle& h) const
{
    return h.templateObj();
}

App::DocumentObject* IdentityGraph::materialize(const UiHandle& h) const
{
    return h.localObj();
}

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau, Phase 0): ersetzt
// collectLocalMirrorCandidates()/hasRealObject()'s Brute-Force-Abgleich durch eine gezielte
// rekursive Suche, die bei der TIEFSTEN instanz-aufgeloesten Container-Ebene aus 'h' startet
// (statt immer bei rootAssembly selbst), damit auch ein Spiegel mehrere Verschachtelungsebenen
// tief zuverlaessig gefunden wird - das ist die direkte Behebung des BG37->BG43->BG67-Bugs
// (elf nie aktualisierte Getriebeteil-Spiegel).
namespace
{
// FCPROJECT-PATCH (2026-09-19, live am echten BG37/BG43/BG67-Projekt gefunden): urspruenglich
// stieg dies NUR in asmLink->Group ab (den lokalen Spiegel-Kindern DIESER Instanz, alle im
// SELBEN Dokument wie 'obj' selbst). Das findet NIE einen NATIVEN Spiegel, der EINE Ebene
// TIEFER im tatsaechlich VERLINKTEN Dokument entsteht (z.B. BG43s EIGENER, dort nativ lebender
// Spiegel eines BG67-Blatts, erzeugt durch BG43s EIGENE, LOKALE AssemblyLink-Instanz) - und
// GENAU DIESER native Spiegel wird NIE anderweitig synchronisiert, wenn BG43 als verschachtelte,
// unter einem flexiblen Elternteil liegende Baugruppe ihren EIGENEN solve()-Aufruf
// uebersprungen bekommt (siehe isNestedUnderFlexibleParent()) - nur der AEUSSERSTE
// solve()-Aufruf (der hier suchende) bekommt je die Chance, ihn zu aktualisieren. Deshalb
// zusaetzlich in asmLink->getLinkedAssembly()->Group absteigen - das echte, verlinkte
// Dokument selbst durchsuchen, nicht nur die eigene lokale Spiegelkopie davon.
void collectCandidates(App::DocumentObject* obj, std::vector<App::DocumentObject*>& out)
{
    if (!obj) {
        return;
    }
    out.push_back(obj);
    if (auto* asmLink = freecad_cast<AssemblyLink*>(obj)) {
        if (!asmLink->isRigid()) {
            for (auto* child : asmLink->Group.getValues()) {
                collectCandidates(child, out);
            }
            if (auto* nested = asmLink->getLinkedAssembly()) {
                for (auto* child : nested->Group.getValues()) {
                    collectCandidates(child, out);
                }
            }
        }
        return;
    }
    if (auto* group = freecad_cast<App::DocumentObjectGroup*>(obj)) {
        for (auto* child : group->Group.getValues()) {
            collectCandidates(child, out);
        }
    }
}
}  // namespace

std::vector<App::DocumentObject*> IdentityGraph::mirrorsOf(const IdentityHandle& h)
{
    std::vector<App::DocumentObject*> result;
    if (!h.templateObj) {
        return result;
    }

    // FCPROJECT-PATCH (2026-09-20, live am echten BG22-Projekt gefunden: verschachtelte
    // Duplikation - 3x FuehrungsBaugruppe, je 2x Halterbaugruppe - bricht die Erreichbarkeit):
    // wenn 'h.templateObj' selbst schon die GETEILTE VORLAGE der TIEFSTEN duplicateInstancePath-
    // Ebene ist (z.B. weil eine Rigid-Grenze den Walk GENAU AUF dieser verschachtelten
    // Unterbaugruppen-Ebene gestoppt hat, statt weiter in ihre Kinder hinein), dann IST dieser
    // letzte Eintrag SELBST bereits der gesuchte instanzeigene Spiegel - eine Suche INNERHALB
    // seiner eigenen Kinder (weiter unten) findet ihn NIE, weil deren Identitaet zwangslaeufig
    // TIEFER liegt als seine eigene. Ohne diesen Fall liefert mirrorsOf() hier faelschlich leer
    // zurueck, materializeForObjectPartMap() faellt auf den GETEILTEN Vorlage-Zeiger zurueck
    // (statt der instanzeigenen Spiegel-Kopie), und zwei verschiedene Instanzen derselben
    // verschachtelten Baugruppe kollabieren auf denselben MbD-Teil - genau das brach die
    // Erreichbarkeits-Traversierung fuer den GESAMTEN Zweig hinter dieser Ebene.
    if (h.duplicateInstancePath.size() >= 2) {
        AssemblyLink* deepest = h.duplicateInstancePath.back();
        AssemblyLink* container = h.duplicateInstancePath[h.duplicateInstancePath.size() - 2];
        if (container->getSourceForMirror(deepest) == h.templateObj) {
            return {deepest};
        }
    }

    std::vector<App::DocumentObject*> searchRoots;
    if (h.duplicateInstancePath.empty()) {
        searchRoots = rootAssembly->Group.getValues();
    }
    else {
        searchRoots = h.duplicateInstancePath.back()->Group.getValues();
    }

    std::vector<App::DocumentObject*> candidates;
    for (auto* root : searchRoots) {
        collectCandidates(root, candidates);
    }

    for (auto* candidate : candidates) {
        if (candidate == h.templateObj) {
            // Das Objekt selbst liegt schon lokal (nicht verschachtelt/nicht dupliziert) -
            // gilt als sein eigener Spiegel.
            result.push_back(candidate);
            continue;
        }
        IdentityHandle candidateHandle = resolveObject(candidate);
        if (candidateHandle == h) {
            result.push_back(candidate);
        }
    }
    return result;
}

bool IdentityGraph::isGrounded(const IdentityHandle& h)
{
    if (!groundedBuilt) {
        for (auto* obj : rootAssembly->getGroundedParts()) {
            groundedSet.insert(resolveObject(obj));
        }
        groundedBuilt = true;
    }
    return groundedSet.find(h) != groundedSet.end();
}

std::vector<IdentityHandle> IdentityGraph::groundedHandles()
{
    if (!groundedBuilt) {
        isGrounded({});  // erzwingt den Aufbau, Ergebnis wird verworfen
    }
    return std::vector<IdentityHandle>(groundedSet.begin(), groundedSet.end());
}

void IdentityGraph::ensureJointEdgesBuilt()
{
    if (jointEdgesBuilt) {
        return;
    }
    edges.clear();
    for (const auto& jointRef : rootAssembly->getJoints(false, true, false)) {
        JointEdge edge;
        edge.joint = jointRef.joint;
        edge.nestingPrefix = jointRef.nestingPrefix;
        edge.type = getJointType(jointRef.joint);
        edge.a = resolveJointRef(jointRef.joint, "Reference1", jointRef.nestingPrefix);
        edge.b = resolveJointRef(jointRef.joint, "Reference2", jointRef.nestingPrefix);
        edges.push_back(std::move(edge));
    }
    jointEdgesBuilt = true;
}

const std::vector<IdentityGraph::JointEdge>& IdentityGraph::jointEdges()
{
    ensureJointEdgesBuilt();
    return edges;
}

std::vector<IdentityHandle> IdentityGraph::neighbors(const IdentityHandle& h)
{
    ensureJointEdgesBuilt();
    std::vector<IdentityHandle> result;
    for (const auto& edge : edges) {
        if (edge.a == h) {
            result.push_back(edge.b);
        }
        else if (edge.b == h) {
            result.push_back(edge.a);
        }
    }
    return result;
}

void IdentityGraph::invalidate()
{
    jointEdgesBuilt = false;
    edges.clear();
    groundedBuilt = false;
    groundedSet.clear();
}

namespace
{
// DOT-Bezeichner duerfen keine Anfuehrungszeichen/Backslashes enthalten - Labels (Teile-/
// Joint-Namen) koennen beides theoretisch enthalten (Nutzer-vergebene Labels sind frei).
std::string dotEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// Menschlich lesbares Knoten-Label: der interne Name des tiefsten echten Objekts (z.B.
// "Halterbaugruppe001"), NICHT dessen Label. Nutzerklarstellung 2026-09-21 (live an der echten
// BG22/23-Datei gefunden): zwei innerhalb DERSELBEN AssemblyLink-Instanz mehrfach eingefuegte
// Kopien (z.B. Slot1/Slot2 einer FuehrungsBaugruppe, beide "CNC3018_023_A_Halterbaugruppe") tragen
// als Label ABSICHTLICH denselben Text (Bug-C-Konvention, siehe docs/ARCHITECTURE.md §4.1) - im
// Diagramm erschienen dadurch mehrere Knoten identisch als "Haltebaugruppe" ohne jede
// Unterscheidung. Der interne Name (getNameInDocument(), dokumentweit eindeutig, z.B.
// "Halterbaugruppe"/"Halterbaugruppe001") loest das zuverlaessig auf - dieselbe Regel gilt bereits
// fuer Joint- und Cluster-Beschriftungen (siehe dortige Kommentare).
std::string handleLabel(const IdentityHandle& h)
{
    if (!h.templateObj) {
        return "<null>";
    }
    return h.templateObj->getNameInDocument();
}

// FCPROJECT-PATCH (2026-09-21, Nutzerklarstellung: "RigidGroup in Assembly nenne ich alle Parts
// die sind geerdet [gemeint: starr aneinanderhaengend], z.B. in BG25 das sind ein Halter und
// Fuehrung"): einfache Union-Find fuer die doppelt umrandeten Rigid-Cluster - vereinigt NICHT nur
// explizite RigidGroupJoint-Mitglieder, sondern JEDE transitive Kette reiner Fixed-Kanten (genau
// die Bündelung, die addConnectedFixedParts() beim Ziehen ohnehin schon als EINEN starren
// MbD-Koerper behandelt, siehe AssemblyObject.cpp). Rein fuer die Darstellung - keine Wirkung auf
// den Solver.
struct DisjointSet
{
    std::unordered_map<int, int> parent;

    int find(int x)
    {
        auto [it, inserted] = parent.emplace(x, x);
        if (inserted || it->second == x) {
            return it->second;
        }
        it->second = find(it->second);
        return it->second;
    }

    void unite(int a, int b)
    {
        int ra = find(a);
        int rb = find(b);
        if (ra != rb) {
            parent[ra] = rb;
        }
    }
};

// FCPROJECT-PATCH (2026-09-21, Nutzerauftrag: "kannst du rekursiv Assembly umrahmen und Parts
// drin haben?"): Baum aus der vollen AssemblyLink-Container-Kette jedes Knotens (via
// findContainerChain(), NICHT ueber IdentityHandle::duplicateInstancePath - letztere laesst
// bewusst NICHT-duplizierte Container-Ebenen aus, siehe deren Deklaration in
// AssemblyIdentityGraph.h; fuer eine reine Verschachtelungs-DARSTELLUNG zaehlt dagegen JEDE
// Ebene, dupliziert oder nicht). Ein Knoten ohne Container (z.B. Rahmen/Ursprung direkt in der
// Wurzel-Baugruppe) landet in directNodeIds der Wurzel selbst, ausserhalb jeder Umrahmung.
struct ClusterTree
{
    std::vector<int> directNodeIds;
    std::unordered_map<AssemblyLink*, ClusterTree> children;
};

void insertIntoClusterTree(ClusterTree& root, const std::vector<AssemblyLink*>& chain, int nodeId)
{
    ClusterTree* cur = &root;
    for (auto* link : chain) {
        cur = &cur->children[link];
    }
    cur->directNodeIds.push_back(nodeId);
}

void emitClusterTree(
    std::string& out,
    const ClusterTree& node,
    int& clusterCounter,
    const std::function<void(std::string&, int)>& emitNodeLine,
    const std::unordered_map<int, int>& rigidGroupOfNode
)
{
    // Nutzerklarstellung 2026-09-21: "Label" (FreeCADs Label-Property, bei mehrfach eingefuegten
    // AssemblyLinks ABSICHTLICH mehrdeutig, siehe handleLabel()) und "Name" (getNameInDocument(),
    // dokumentweit eindeutig) sind zwei verschiedene Dinge - hier bewusst NUR der interne Name
    // (z.B. "Assembly002"), NICHT das (mehrdeutige) Label ("ass_fuehrung").
    for (auto& [link, child] : node.children) {
        out += "  subgraph cluster_" + std::to_string(clusterCounter++) + " {\n";
        // Nutzerbefund 2026-09-21: bei wenig Platz ueberlappte die Cluster-Beschriftung optisch
        // mit dem inneren Rigid-Cluster direkt darunter (keine echte doppelte Beschriftung im
        // DOT-Quelltext - nur fehlender vertikaler Abstand, den Graphviz' 'margin'-Attribut bei
        // verschachtelten Clustern trotz hohem Wert nicht zuverlaessig reserviert). Eine
        // nachgestellte Leerzeile im Label selbst erzwingt die zusaetzliche Zeilenhoehe
        // zuverlaessig.
        // Nutzerbefund 2026-09-21: bei "t" (oben) ueberlappte das aeussere Label wiederholt mit
        // dem inneren Rigid-Cluster, der bei dieser Baumstruktur meist selbst oben im Layout
        // landet - "b" (unten) haelt zuverlaessig Abstand.
        out += "    label=\"" + dotEscape(link ? link->getNameInDocument() : "?") + "\";\n";
        out += "    fontname=\"sans-serif Bold\";\n";
        out += "    style=\"dashed,bold\";\n";
        out += "    penwidth=2;\n";
        emitClusterTree(out, child, clusterCounter, emitNodeLine, rigidGroupOfNode);
        out += "  }\n";
    }

    // Rigide zusammenhaengende Teile (transitive Fixed-Kette UND/ODER explizite
    // RigidGroupJoint-Mitgliedschaft, siehe DisjointSet oben) bekommen ihren eigenen, doppelt
    // umrandeten (peripheries=2, durchgezogen statt gestrichelt) inneren Cluster - auch root-level,
    // ohne jede AssemblyLink-Umrahmung, da diese Partitionierung auf JEDER Baumebene gleich
    // angewendet wird. Eine "Gruppe" mit nur einem Mitglied AUF DIESER EBENE (z.B. weil die
    // anderen Mitglieder in einem ANDEREN Container liegen - siehe BG22s Kreuz-Joint zwischen zwei
    // verschiedenen Schienen-Instanzen) wird NICHT eingerahmt, sonst entstuende ein bedeutungsloser
    // Rahmen um ein einzelnes Teil.
    std::unordered_map<int, std::vector<int>> byRigidGroup;
    std::vector<int> ungrouped;
    for (int id : node.directNodeIds) {
        auto it = rigidGroupOfNode.find(id);
        if (it != rigidGroupOfNode.end()) {
            byRigidGroup[it->second].push_back(id);
        }
        else {
            ungrouped.push_back(id);
        }
    }
    for (auto& [groupId, ids] : byRigidGroup) {
        (void)groupId;
        if (ids.size() < 2) {
            ungrouped.insert(ungrouped.end(), ids.begin(), ids.end());
            continue;
        }
        // Nutzerauftrag 2026-09-21: doppelt umrandet wie ein geerdeter Knoten
        // (peripheries=2) - Graphviz' 'peripheries'-Attribut wird fuer CLUSTER (anders als fuer
        // Knoten) nicht zuverlaessig als zwei sichtbare Linien gerendert (live bestaetigt: nur
        // eine einzelne, nicht sichtbar doppelte Linie). Robusterer Ersatz: zwei ineinander
        // verschachtelte Cluster mit sichtbarem Zwischenraum (unterschiedlicher margin) - das
        // ergibt zuverlaessig zwei konzentrische Rahmen. WICHTIG: DOT vererbt ein auf einem
        // umschliessenden Cluster gesetztes 'label' an verschachtelte Kind-Cluster, die es nicht
        // selbst ueberschreiben - ohne das explizite 'label="";' HIER rendert Graphviz die
        // AssemblyLink-Beschriftung der AEUSSEREN Umrahmung (falls vorhanden) zusaetzlich auf
        // BEIDEN Rigid-Rahmen erneut (live beobachtet: derselbe Text bis zu dreifach gestapelt).
        out += "  subgraph cluster_" + std::to_string(clusterCounter++) + " {\n";
        out += "    label=\"\";\n";
        out += "    style=solid;\n";
        out += "    penwidth=2;\n";
        out += "    margin=12;\n";
        out += "  subgraph cluster_" + std::to_string(clusterCounter++) + " {\n";
        out += "    label=\"\";\n";
        out += "    style=solid;\n";
        out += "    penwidth=2;\n";
        out += "    margin=4;\n";
        for (int id : ids) {
            emitNodeLine(out, id);
        }
        out += "  }\n";
        out += "  }\n";
    }
    for (int id : ungrouped) {
        emitNodeLine(out, id);
    }
}
}  // namespace

std::string IdentityGraph::exportDot()
{
    ensureJointEdgesBuilt();
    if (!groundedBuilt) {
        isGrounded({});  // erzwingt den Aufbau des Erdungs-Sets als Nebeneffekt
    }

    std::unordered_map<IdentityHandle, int, IdentityHandleHash> nodeIds;
    auto nodeIdFor = [&](const IdentityHandle& h) -> int {
        auto it = nodeIds.find(h);
        if (it != nodeIds.end()) {
            return it->second;
        }
        int id = static_cast<int>(nodeIds.size());
        nodeIds.emplace(h, id);
        return id;
    };

    // Erst alle Knoten sammeln - aus den Joint-Kanten UND aus den geerdeten Handles, damit auch
    // ein geerdetes, aber (noch) joint-loses Teil im Diagramm sichtbar bleibt statt stillschweigend
    // zu fehlen.
    for (const auto& edge : edges) {
        nodeIdFor(edge.a);
        nodeIdFor(edge.b);
    }
    for (const auto& g : groundedSet) {
        nodeIdFor(g);
    }

    // Nutzerklarstellung 2026-09-21: "RigidGroup" meint hier NICHT nur das explizite
    // RigidGroupJoint-Feature, sondern JEDE Kette rein starrer (Fixed-)Verbindungen - z.B. in
    // BG25 sind Halter und Fuehrung ueber genau so eine Kette starr aneinandergehaengt, ohne
    // dass dafuer je eine explizite "Starre Verbindung" angelegt wurde. Union-Find ueber BEIDE
    // Quellen: (1) jede Fixed-JointEdge, (2) explizite RigidGroupJoint-Mitgliedschaft
    // (rootAssembly->getRigidGroups(), siehe rebuildRigidClusters()s identische kanonisierende
    // Verarbeitung von "ObjectsToRigidGroup"). Muss vor dem ClusterTree-Aufbau unten laufen, da
    // resolveObject() hier moeglicherweise neue, bisher unbekannte Knoten registriert.
    DisjointSet rigidDsu;
    for (const auto& edge : edges) {
        if (edge.type == JointType::Fixed) {
            rigidDsu.unite(nodeIdFor(edge.a), nodeIdFor(edge.b));
        }
    }
    for (auto* rigidGroupObj : rootAssembly->getRigidGroups()) {
        if (!rigidGroupObj) {
            continue;
        }
        auto* prop = dynamic_cast<App::PropertyLinkList*>(
            rigidGroupObj->getPropertyByName("ObjectsToRigidGroup")
        );
        if (!prop) {
            continue;
        }
        auto members = prop->getValues();
        std::vector<int> memberNodeIds;
        for (auto* member : members) {
            if (!member) {
                continue;
            }
            memberNodeIds.push_back(nodeIdFor(resolveObject(member)));
        }
        for (std::size_t i = 1; i < memberNodeIds.size(); ++i) {
            rigidDsu.unite(memberNodeIds[0], memberNodeIds[i]);
        }
    }

    // Nach ALLEN Unions (inkl. der von getRigidGroups() ggf. neu registrierten Knoten) nach
    // Wurzel gruppieren - nur echte Mehrfach-Mitgliedschaften (>=2 an dieser Stelle) werden
    // unten ueberhaupt als Kandidat fuer einen doppelt umrandeten Cluster behandelt (die
    // eigentliche "nur >=2 auf DERSELBEN Baumebene"-Entscheidung faellt aber erst in
    // emitClusterTree(), siehe dortiger Kommentar).
    std::unordered_map<int, std::vector<int>> rigidGroupsByRoot;
    for (const auto& [handle, id] : nodeIds) {
        (void)handle;
        rigidGroupsByRoot[rigidDsu.find(id)].push_back(id);
    }
    std::unordered_map<int, int> rigidGroupOfNode;
    int rigidGroupCounter = 0;
    for (auto& [root, ids] : rigidGroupsByRoot) {
        (void)root;
        if (ids.size() < 2) {
            continue;
        }
        int groupId = rigidGroupCounter++;
        for (int id : ids) {
            rigidGroupOfNode[id] = groupId;
        }
    }

    std::string out;
    out += "graph IdentityGraph {\n";
    out += "  rankdir=LR;\n";
    out += "  node [shape=box, style=filled, fontname=\"sans-serif\", fillcolor=\"#dee2e6\"];\n";

    // Rekursive Umrahmung (Nutzerauftrag 2026-09-21): jeder Knoten wird per findContainerChain()
    // seiner vollen AssemblyLink-Kette zugeordnet und in verschachtelten DOT-subgraph-cluster-
    // Bloecken emittiert, statt als flache Liste - macht sichtbar, WELCHE Teile in WELCHER
    // (ggf. mehrfach verschachtelten) Unterbaugruppen-Instanz stecken.
    ClusterTree clusterRoot;
    std::unordered_map<int, IdentityHandle> handleById;
    // FCPROJECT-PATCH (2026-09-21, live an der echten CNC3018_022-Datei gefunden): seit dem
    // Identitaets-Fix (siehe [[todo-mirrortosourcemap-missing-for-nested-assemblylink]]) teilen
    // ALLE Instanzen einer Schiene fuer denselben Slot dasselbe 'templateObj' (z.B. immer
    // "CNC3018_023_A_Halterbaugruppe", NIE mehr die frueher zufaellig unterscheidbaren
    // Top-Level-Namen "...004"/"...005") - handleLabel() alleine liefert deshalb fuer alle 3
    // Schienen-Instanzen denselben Text. Die instanzeigene, lokale Spiegelkopie ('localObj',
    // ohnehin schon fuer die Cluster-Zuordnung berechnet) traegt dagegen weiterhin den
    // instanzeigenen, eindeutigen Namen - als Label bevorzugt, 'handleLabel()' nur als Rueckfall.
    std::unordered_map<int, App::DocumentObject*> localObjById;
    for (const auto& [handle, id] : nodeIds) {
        handleById[id] = handle;
        App::DocumentObject* localObj = materialize(resolveForUi(handle));
        localObjById[id] = localObj;
        std::vector<AssemblyLink*> chain;
        if (localObj) {
            findContainerChain(rootAssembly, localObj, chain);
        }
        insertIntoClusterTree(clusterRoot, chain, id);
    }

    auto emitNodeLine = [&](std::string& text, int id) {
        const IdentityHandle& handle = handleById[id];
        App::DocumentObject* localObj = localObjById[id];
        std::string label = localObj ? localObj->getNameInDocument() : handleLabel(handle);
        bool grounded = groundedSet.find(handle) != groundedSet.end();
        text += "  n" + std::to_string(id) + " [label=\"" + dotEscape(label) + "\"";
        if (grounded) {
            text += ", fillcolor=\"#b7e4c7\", peripheries=2";
        }
        text += "];\n";
    };
    int clusterCounter = 0;
    emitClusterTree(out, clusterRoot, clusterCounter, emitNodeLine, rigidGroupOfNode);

    // Nutzerklarstellung 2026-09-21 (siehe Kommentar oben bei den Cluster-Labels): Kanten-Label
    // ist der interne Name (getNameInDocument(), dokumentweit eindeutig, z.B. "Joint003"), NICHT
    // das (bei aktivem DuplicateLabels moeglicherweise mehrdeutige) Label.
    for (const auto& edge : edges) {
        int idA = nodeIdFor(edge.a);
        int idB = nodeIdFor(edge.b);
        std::string jointName = edge.joint ? edge.joint->getNameInDocument() : "?";
        bool rigid = (edge.type == JointType::Fixed);
        out += "  n" + std::to_string(idA) + " -- n" + std::to_string(idB) + " [label=\""
            + dotEscape(jointName) + "\", style=" + (rigid ? "bold" : "dashed") + "];\n";
    }

    out += "}\n";
    return out;
}

}  // namespace Assembly
