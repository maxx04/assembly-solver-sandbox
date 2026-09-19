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

#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/DocumentObjectGroup.h>
#include <App/Link.h>
#include <App/PropertyLinks.h>
#include <App/PropertyStandard.h>

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

        IdentityHandle result;
        result.templateObj = obj;
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
IdentityHandle IdentityGraph::resolveObject(App::DocumentObject* obj)
{
    if (!obj) {
        return {};
    }

    std::vector<AssemblyLink*> path;
    if (!findLocalGroupPath(rootAssembly->Group.getValues(), obj, path)) {
        // 'obj' liegt nicht im eigenen lokalen Baum - evtl. die lokale Spiegel-Kopie einer
        // verschachtelten Unterbaugruppe (siehe canonicalizeForMbD()'s GrandTop-Fallback):
        // an jede eigene, nicht-rigide Unterbaugruppe delegieren, deren eigenes Dokument zu
        // 'obj' passt.
        for (auto* asmLink : rootAssembly->getSubAssemblies()) {
            if (!asmLink || asmLink->isRigid()) {
                continue;
            }
            AssemblyObject* nested = asmLink->getLinkedAssembly();
            if (!nested || nested == rootAssembly || nested->getDocument() != obj->getDocument()) {
                continue;
            }
            IdentityGraph nestedGraph(nested);
            return nestedGraph.resolveObject(obj);
        }
        IdentityHandle result;
        result.templateObj = obj;
        return result;
    }

    AssemblyLink* previousLink = nullptr;
    std::vector<AssemblyLink*> duplicatePath;
    for (auto* mirrorLink : path) {
        if (hasSiblingInstances(candidatesFor(rootAssembly, previousLink), mirrorLink)) {
            duplicatePath.push_back(mirrorLink);
        }

        App::DocumentObject* levelReal = mirrorLink;
        if (previousLink) {
            levelReal = previousLink->getSourceForMirror(mirrorLink);
            if (!levelReal) {
                IdentityHandle result;
                result.templateObj = obj;
                result.duplicateInstancePath = duplicatePath;
                return result;
            }
        }
        auto* realAsmLink = freecad_cast<AssemblyLink*>(levelReal);
        if (!realAsmLink || realAsmLink->isRigid()) {
            IdentityHandle result;
            result.templateObj = levelReal;
            result.duplicateInstancePath = duplicatePath;
            return result;
        }
        previousLink = realAsmLink;
    }

    IdentityHandle result;
    if (path.empty()) {
        result.templateObj = obj;
        return result;
    }
    App::DocumentObject* resolved = path.back()->getSourceForMirror(obj);
    result.templateObj = resolved ? resolved : obj;
    result.duplicateInstancePath = duplicatePath;
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
                if (hasSiblingInstances(candidatesFor(rootAssembly, localContext), assemblyLink)) {
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
            const auto* pRigid = obj->getPropertyByName<App::PropertyBool>("Rigid");
            if (pRigid && !pRigid->getValue()) {
                continue;
            }
        }

        IdentityHandle result;
        result.templateObj = obj;
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

}  // namespace Assembly
