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

#pragma once

// FCPROJECT-PATCH (2026-09-19, IdentityGraph-Umbau, siehe
// /home/maxx/.claude/plans/enumerated-roaming-river.md und docs/ARCHITECTURE.md §4/§6):
// Phase 0 - rein additive Einfuehrung. Diese Klasse wird von KEINER bestehenden Aufrufstelle
// genutzt; sie ersetzt vorerst nichts. Zweck: die komplette Verschachtelungs-/Spiegel-/
// Duplikat-/Erdungs-Struktur einer Baugruppe EINMAL, vollstaendig und tiefengenerell (nicht nur
// den letzten Sprung hinter einer Instanz-Duplikation, siehe die self-dokumentierte Luecke in
// AssemblyUtils::resolveJointReference()) als expliziten Graphen aufzubauen, der spaeter (Phase
// 1-5) als Zwischenschicht zwischen den FreeCAD-Joints und dem OndselSolver alle bisher
// verstreuten Verschachtelungs-Traversierungen ersetzt.

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Mod/Assembly/AssemblyGlobal.h>

namespace App
{
class DocumentObject;
}  // namespace App

namespace Base
{
class Placement;
}  // namespace Base

namespace Assembly
{

class AssemblyObject;
class AssemblyLink;
enum class JointType;
class IdentityGraph;

// Wert-basierte Knoten-Identitaet. Ein roher App::DocumentObject* reicht nicht: derselbe
// physische Teil kann ueber zwei verschiedene Aufloesungs-Routen (kurzer Bug-C-Spiegel-Pfad vs.
// getJoints()' subJoints-Rekursion durch das geteilte Vorlage-Dokument) zu zwei verschiedenen
// Zeigern aufgeloest werden - das war die Wurzelursache mehrerer Bugs (siehe
// docs/ARCHITECTURE.md §6 "beliebiger Anschlusspunkt" und der Live-Fund BG37->BG43->BG67).
//
// templateObj: das tiefste, ueber die geteilten/verlinkten Dokumente erreichte reale Objekt -
// entspricht semantisch dem, was AssemblyObject::canonicalizeForMbD() heute zurueckliefert, WENN
// keine Instanz-Duplikation im Spiel ist.
//
// duplicateInstancePath: die Kette der INSTANZ-AUFGELOESTEN AssemblyLink-Container (aeusserster
// zuerst), bei denen hasSiblingInstances() zutraf - NUR diese Ebenen tragen zur Identitaet bei.
// Damit kollabieren zwei unterschiedlich lange Routen zum selben Handle, wenn keine Duplikation
// vorliegt, UND zwei echte Duplikat-Instanzen bleiben zuverlaessig auseinandergehalten, selbst
// wenn die Duplikation mehrere Ebenen tief auftritt (z.B. eine duplizierte Unterbaugruppe
// enthaelt selbst wieder eine an dieser Stelle duplizierte weitere Unterbaugruppe).
struct AssemblyExport IdentityHandle
{
    App::DocumentObject* templateObj = nullptr;
    std::vector<AssemblyLink*> duplicateInstancePath;

    bool operator==(const IdentityHandle& other) const;
    bool operator!=(const IdentityHandle& other) const
    {
        return !(*this == other);
    }
};

struct AssemblyExport IdentityHandleHash
{
    std::size_t operator()(const IdentityHandle& h) const;
};

// ENTSCHIEDEN (siehe Plan §2 "Handle-Modi"): zwei eigene, nicht ineinander konvertierbare
// Handle-Typen statt eines Handle-Typs mit Laufzeit-Flag - eine Verwechslung zwischen
// "Identitaet fuer den Solver" (das geteilte, kanonische Objekt - Masse/Geometrie/Joint-
// Definitionen sind darueber gemeinsam) und "Identitaet fuer die GUI/das Ziehen" (die konkrete,
// instanzeigene lokale Kopie, die tatsaechlich gerendert und angefasst wird) wird dadurch zu
// einem Compile-Fehler statt einem Laufzeit-Bug. Beide sind NUR ueber IdentityGraph konstruierbar
// (privater Konstruktor + friend), ein Aufrufer kann also nie eines aus einem beliebigen
// App::DocumentObject* selbst zusammenbauen.
class AssemblyExport SolverHandle
{
public:
    App::DocumentObject* templateObj() const
    {
        return handle.templateObj;
    }
    const IdentityHandle& identity() const
    {
        return handle;
    }

private:
    friend class IdentityGraph;
    explicit SolverHandle(IdentityHandle h)
        : handle(std::move(h))
    {}
    IdentityHandle handle;
};

class AssemblyExport UiHandle
{
public:
    App::DocumentObject* localObj() const
    {
        return obj;
    }
    const IdentityHandle& identity() const
    {
        return handle;
    }

private:
    friend class IdentityGraph;
    UiHandle(IdentityHandle h, App::DocumentObject* o)
        : handle(std::move(h))
        , obj(o)
    {}
    IdentityHandle handle;
    App::DocumentObject* obj = nullptr;
};

class AssemblyExport IdentityGraph
{
public:
    explicit IdentityGraph(AssemblyObject* root);

    // DER zentrale Resolver fuer eine GUI-/Selektions-Adresse (Top-Level-Objekt + optionaler,
    // Punkt-getrennter Rest-Pfad, wie ihn getMovingPartFromSel() heute entgegennimmt). Anders als
    // die Altfunktion wird JEDE gekreuzte Instanz-Duplikations-Grenze uebersetzt, nicht nur die
    // letzte - siehe .cpp fuer die Herleitung anhand des Live-Bugs BG37->BG43->BG67.
    IdentityHandle resolve(App::DocumentObject* obj, const std::string& subPath = {});

    // Gegenstueck fuer den canonicalizeForMbD()-Anwendungsfall: 'obj' ist bereits ein konkreter
    // Zeiger (keine Selektions-Pfad-Zeichenkette wie bei resolve()/getMovingPartFromSel() - der
    // Aufrufer hat z.B. ein Objekt aus getGroundedParts() oder direkt aus einer Joint-Property).
    // Sucht 'obj' stattdessen ueber eine TOP-DOWN-Struktursuche (wie findLocalGroupPath()) und
    // uebersetzt den gefundenen Pfad vorwaerts - tiefengenerelle Neufassung von
    // canonicalizeForMbD(), die (anders als diese) bei einer Instanz-Duplikation nicht sofort
    // aufgibt, sondern die Duplikation in duplicateInstancePath vermerkt und weiteruebersetzt.
    IdentityHandle resolveObject(App::DocumentObject* obj);

    // FCPROJECT-PATCH (2026-09-19, live am echten BG37/BG43/BG67-Projekt gefunden, siehe
    // getMbDData()s bisherige containerChainPlc-Berechnung): liefert das Produkt der
    // Placement-Werte JEDER AssemblyLink-Ebene, die zwischen rootAssembly und 'obj' liegt -
    // gebraucht, um die WELT-Placement eines tief verschachtelten, ueber MEHRERE ECHTE
    // Dokumentgrenzen erreichten Objekts korrekt aus dessen LOKALER Placement zu berechnen.
    // Identity, wenn 'obj' nirgendwo unterhalb von rootAssembly gefunden wird (z.B. bereits
    // top-level/flach). Tiefengenerell (beliebig viele echte Dokumentgrenzen, nicht nur eine wie
    // die urspruengliche, rein lokale findLocalGroupPath()-basierte Berechnung).
    Base::Placement containerChainPlacement(App::DocumentObject* obj);

    // Gegenstueck fuer eine Joint-Referenz (Reference1/Reference2 + nestingPrefix), ersetzt
    // AssemblyUtils::resolveJointReference()'s Kernschleife - siehe dortigen Kommentar
    // (AssemblyUtils.cpp ~1067-1073) fuer die selbst-dokumentierte Ein-Sprung-Grenze, die hier
    // behoben wird.
    IdentityHandle resolveJointRef(
        App::DocumentObject* joint,
        const char* propName,
        const std::string& nestingPrefix
    );

    SolverHandle resolveForSolver(const IdentityHandle& h);
    UiHandle resolveForUi(const IdentityHandle& h);

    App::DocumentObject* materialize(const SolverHandle& h) const;
    App::DocumentObject* materialize(const UiHandle& h) const;

    // Jeder lokale Spiegel-Kandidat (auf JEDER Tiefe), der auf 'h' aufloest - ersetzt
    // collectLocalMirrorCandidates() + Brute-Force-Abgleich in syncLocalMirrorPlacement().
    std::vector<App::DocumentObject*> mirrorsOf(const IdentityHandle& h);

    bool isGrounded(const IdentityHandle& h);
    std::vector<IdentityHandle> groundedHandles();

    struct JointEdge
    {
        App::DocumentObject* joint = nullptr;
        std::string nestingPrefix;
        IdentityHandle a;
        IdentityHandle b;
        JointType type;
    };
    const std::vector<JointEdge>& jointEdges();
    std::vector<IdentityHandle> neighbors(const IdentityHandle& h);

    // FCPROJECT-PATCH (2026-09-21, Nutzerauftrag [[todo-identitygraph-visualization]]):
    // Graphviz/DOT-Export des kompletten Graphen - ein Knoten pro IdentityHandle (aus
    // jointEdges() UND groundedHandles(), damit auch ein geerdetes, aber joint-loses Teil
    // sichtbar bleibt), eine Kante pro JointEdge (Fixed fett/durchgezogen = starr, alles andere
    // gestrichelt = echter Freiheitsgrad), geerdete Knoten farblich hervorgehoben. Genau die Art
    // Abbruch einer Erreichbarkeits-Traversierung, die beim BG22-Fund
    // (mirrorsOf()/Duplikat-in-Duplikat) erst per Log-Grep-Archaeologie gefunden wurde, waere in
    // dieser Visualisierung sofort als fehlende Kante/isolierter Knoten sichtbar. Reiner
    // Text-Export (kein FreeCAD-/Qt-Abhaengigkeit) - Ausgabe laesst sich direkt in einen
    // Graphviz-Renderer (z.B. `dot -Tsvg`) oder einen Online-Viewer einfuegen.
    std::string exportDot();

    // Nutzerauftrag 2026-09-21 (".asmt als zusaetzliche Kontrolle nutzen"): exportiert die
    // Baugruppe ueber die ECHTE Solver-Pipeline (AssemblyObject::exportAsASMT(), dieselbe, die ein
    // echter solve() durchlaeuft) in eine temporaere .asmt-Datei, parst deren Teile-/Gelenk-Zahlen
    // und vergleicht sie mit dem, was DIESER Graph unabhaengig davon selbst zusammengetragen hat -
    // eine unabhaengige Gegenkontrolle der eigenen Knoten-/Kanten-Buchhaltung (nicht der
    // Resolver-Logik selbst, siehe ausfuehrliche Einschraenkung am Definitionsort). Gedacht als
    // Diagnosehilfe (Fehlersuche, Ergaenzung zu exportDot()), kein automatischer Test.
    std::string verifyAgainstAsmt();

    // Verwirft den zwischengespeicherten Aufbau (Joint-Kanten, Erdungs-Set). resolve()/
    // resolveJointRef() selbst lesen immer live von der Dokumentstruktur und brauchen dies nicht.
    void invalidate();

private:
    void ensureJointEdgesBuilt();

    AssemblyObject* rootAssembly;
    bool jointEdgesBuilt = false;
    std::vector<JointEdge> edges;

    bool groundedBuilt = false;
    std::unordered_set<IdentityHandle, IdentityHandleHash> groundedSet;
};

}  // namespace Assembly
