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

#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>

#include <Mod/Assembly/AssemblyGlobal.h>

#include <App/FeaturePython.h>
#include <App/Part.h>

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

/// Whether a generated link element is excluded from assembly operations.
bool isSuppressedLinkElement(const App::DocumentObject* obj);

// This enum has to be the same as the one in JointObject.py
enum class JointType
{
    Fixed,
    Revolute,
    Cylindrical,
    Slider,
    Ball,
    Distance,
    Parallel,
    Perpendicular,
    Angle,
    RackPinion,
    Screw,
    Gears,
    Belt,
};

enum class GeometryType
{
    Point = 0,

    // Edges
    Line = 1,
    Curve = 2,
    Circle = 3,

    // Faces
    Place = 4,
    Cylinder = 5,
    Sphere = 6,
    Cone = 7,
    Torus = 8,
};

enum class DistanceType
{
    PointPoint,

    LineLine,
    LineCircle,
    CircleCircle,

    PlanePlane,
    PlaneCylinder,
    PlaneSphere,
    PlaneCone,
    PlaneTorus,
    CylinderCylinder,
    CylinderSphere,
    CylinderCone,
    CylinderTorus,
    ConeCone,
    ConeTorus,
    ConeSphere,
    TorusTorus,
    TorusSphere,
    SphereSphere,

    PointPlane,
    PointCylinder,
    PointSphere,
    PointCone,
    PointTorus,

    LinePlane,
    LineCylinder,
    LineSphere,
    LineCone,
    LineTorus,

    CurvePlane,
    CurveCylinder,
    CurveSphere,
    CurveCone,
    CurveTorus,

    PointLine,
    PointCurve,

    Other,
};

class AssemblyObject;
class AssemblyLink;
class JointGroup;

AssemblyExport void swapJCS(const App::DocumentObject* joint);

AssemblyExport bool isEdgeType(
    const App::DocumentObject* obj,
    const std::string& elName,
    const GeomAbs_CurveType type
);
AssemblyExport bool isFaceType(
    const App::DocumentObject* obj,
    const std::string& elName,
    const GeomAbs_SurfaceType type
);
AssemblyExport double getFaceRadius(const App::DocumentObject* obj, const std::string& elName);
AssemblyExport double getEdgeRadius(const App::DocumentObject* obj, const std::string& elName);

AssemblyExport DistanceType getDistanceType(App::DocumentObject* joint);
AssemblyExport JointGroup* getJointGroup(const App::Part* part);

AssemblyExport std::vector<App::DocumentObject*> getAssemblyComponents(const AssemblyObject* assembly);

// getters to get from properties
AssemblyExport void setJointActivated(const App::DocumentObject* joint, bool val);
AssemblyExport bool getJointActivated(const App::DocumentObject* joint);
AssemblyExport double getJointAngle(const App::DocumentObject* joint);
AssemblyExport double getJointDistance(const App::DocumentObject* joint);
AssemblyExport double getJointDistance2(const App::DocumentObject* joint);
AssemblyExport JointType getJointType(const App::DocumentObject* joint);
AssemblyExport std::string getElementFromProp(const App::DocumentObject* obj, const char* propName);
AssemblyExport std::string getElementTypeFromProp(const App::DocumentObject* obj, const char* propName);
AssemblyExport App::DocumentObject* getObjFromProp(
    const App::DocumentObject* joint,
    const char* propName
);
AssemblyExport App::DocumentObject* getObjFromRef(App::DocumentObject* obj, const std::string& sub);
AssemblyExport App::DocumentObject* getObjFromRef(const App::PropertyXLinkSub* prop);
AssemblyExport App::DocumentObject* getObjFromJointRef(
    const App::DocumentObject* joint,
    const char* propName
);
AssemblyExport App::DocumentObject* getLinkedObjFromRef(
    const App::DocumentObject* joint,
    const char* propName
);
// Get the moving part from a selection, which has the full path.
// FCPROJECT-PATCH (Teilschritt 3.1b): verboseLog-Parameter ergaenzt, damit die
// FCPROJECT-DEBUG-Logs in der Definition (urspruenglich fuer die Befund-3-Live-Diagnose
// 2026-09-03 unbedingt eingebaut) nicht mehr bei jedem Selektions-/Zieh-Ereignis unbedingt
// laufen. Default false, kein bestehender Aufrufer (Commands.cpp, ViewProviderAssembly.cpp)
// muss angepasst werden.
AssemblyExport App::DocumentObject* getMovingPartFromSel(
    const AssemblyObject* assemblyObject,
    App::DocumentObject* obj,
    const std::string& sub,
    bool verboseLog = false
);
AssemblyExport App::DocumentObject* getMovingPartFromRef(const App::PropertyXLinkSub* prop);
AssemblyExport App::DocumentObject* getMovingPartFromRef(App::DocumentObject* joint, const char* pName);

// FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix, siehe
// patches/assembly-architecture-overview.md, Abschnitt "2. Wiederverwendung von
// getMovingPartFromSel()"): rein diagnostische/adressierungsbewusste Erweiterung, aendert fuer
// sich genommen nichts am Solver-Verhalten - genutzt von AssemblyObject::resolvePartForMbD().
//
// ResolvedJointRef ist das Ergebnis, ein Reference1/Reference2 einer (ggf. bereits mehrfach
// verschachtelten) Joint-Referenz nicht nur bis zum blossen Top-Level-Objekt aufzuloesen (wie
// getMovingPartFromRef()), sondern - analog zu getMovingPartFromSel()s Segment-fuer-Segment-Walk,
// der transparent durch flexible AssemblyLink-Zwischenstufen hindurchlaeuft - bis zum tatsaechlich
// individuellen Teil samt des dabei noch nicht konsumierten Rest-Sub-Pfads.
struct ResolvedJointRef
{
    App::DocumentObject* obj = nullptr;
    std::string subPath;

    // FCPROJECT-PATCH (Bug C, "Instanz-Identitaet innerhalb einer duplizierten flexiblen
    // Unterbaugruppe", 2026-09-16): siehe docs/ARCHITECTURE.md §4.1 "Bug C" fuer die volle
    // Herleitung. Wird true, wenn 'obj' NICHT der rohe, im geteilten Dokument gefundene Zeiger ist,
    // sondern per Vorwaertslookup in AssemblyLink::objLinkMap durch den Spiegel DIESER konkreten
    // aeusseren Instanz ersetzt wurde (siehe hasSiblingInstances() unten) - Aufrufer duerfen 'obj'
    // in diesem Fall NICHT mehr durch AssemblyObject::canonicalizeForMbD() schicken, das wuerde die
    // Ersetzung sofort wieder rueckgaengig machen (getSourceForMirror() uebersetzt den Spiegel
    // zurueck auf den geteilten, instanzblinden Zeiger). Bleibt "" nestingPrefix oder keine
    // Duplikation vor, bleibt dieses Feld immer false - keine Verhaltensaenderung fuer die
    // bestehende, nicht-duplizierte Testmatrix.
    bool resolvedViaInstanceMirror = false;
};

// FCPROJECT-PATCH (Bug C, 2026-09-16): true, wenn 'asmLink' eine von MEHREREN
// Assembly::AssemblyLink-Geschwistern in 'solvingAssembly' ist, die alle dieselbe
// getLinkedAssembly() (dasselbe echte, verlinkte Dokument) spiegeln - der Nachweis "diese Instanz
// ist tatsaechlich dupliziert", nicht bloss irgendeine verschachtelte Unterbaugruppe (der
// gewoehnliche, bereits korrekt behandelte Tiefen-Fall). Dient als Torwaechter fuer
// resolveJointReference()/getMovingPartFromSel(), damit die dortige Spiegel-Substitution fuer die
// gesamte bestehende, nicht-duplizierte Testmatrix niemals greift (dort hat jedes verlinkte
// Dokument genau eine Instanz).
AssemblyExport bool hasSiblingInstances(const AssemblyObject* solvingAssembly, AssemblyLink* asmLink);

// FCPROJECT-PATCH (Bug C, 2026-09-17): generische Fassung fuer verschachtelte Duplikation -
// prueft dieselbe Bedingung wie oben, aber gegen eine beliebige Kandidatenliste statt zwingend
// gegen solvingAssembly->getSubAssemblies(). Noetig, weil eine Duplikation nicht nur auf der
// AEUSSERSTEN Ebene auftreten kann (zwei AssemblyLink-Instanzen direkt in 'this'), sondern auch
// EINE EBENE TIEFER: eine der bereits mehrfach vorhandenen Instanzen kann selbst wieder eine
// bereits im verlinkten Dokument dupliziert vorliegende Unterbaugruppe spiegeln (z.B. FreeCAD-
// Datei BG25 hat intern schon zwei eigene Halterbaugruppe-Instanzen - wird BG25 selbst nochmal
// eingefuegt, muss JEDE Ebene einzeln auf Duplikation geprueft werden, nicht nur die aeusserste).
// candidates sollte hierfuer 'parentLink->Group.getValues()' sein (die lokalen Spiegel-Kinder
// DIESER konkreten Instanz), analog zu getSubAssemblies() auf der obersten Ebene.
AssemblyExport bool hasSiblingInstances(
    const std::vector<App::DocumentObject*>& candidates,
    AssemblyLink* asmLink
);

// Loest Reference1/Reference2 (die pName-Property, ein App::PropertyXLinkSub) des uebergebenen
// Joint-Objekts auf - relativ zu solvingAssembly, unter der Annahme, dass 'joint' unter dem Pfad
// 'nestingPrefix' (eine Punkt-getrennte Kette von AssemblyLink-Namen, jeweils mit
// abschliessendem Punkt, z.B. "Assembly001.unterAssembly.") innerhalb von solvingAssembly liegt -
// dieser Pfad wird VOR dem eigentlichen Objektnamen der Referenz eingefuegt, sodass der
// entstehende Adress-String bereits relativ "innerhalb" von solvingAssembly beginnt (anders als
// getMovingPartFromSel(), das einen kompletten, ab Dokumentwurzel beginnenden Pfad bekommt und
// deshalb ein zusaetzliches assemblyPassed-Gate braucht - hier nicht noetig, weil nestingPrefix so
// konstruiert wird, dass der Walk schon "dahinter" beginnt).
//
// Liefert bei jedem Fehler (nullptr-Parameter, leere/fehlende Referenz, ein Pfadsegment laesst
// sich nicht als Dokumentobjekt in solvingAssembly finden) ein leeres ResolvedJointRef{} zurueck -
// bewusst defensiv, der Aufrufer (resolvePartForMbD()) faellt in diesem Fall auf die alte
// getMovingPartFromRef() zurueck.
AssemblyExport ResolvedJointRef resolveJointReference(
    const AssemblyObject* solvingAssembly,
    App::DocumentObject* joint,
    const char* pName,
    const std::string& nestingPrefix
);

AssemblyExport std::vector<std::string> getSubAsList(const App::PropertyXLinkSub* prop);
AssemblyExport std::vector<std::string> getSubAsList(
    const App::DocumentObject* joint,
    const char* propName
);
AssemblyExport void syncPlacements(App::DocumentObject* src, App::DocumentObject* to);
AssemblyExport double getJointCurrentValue(App::DocumentObject* joint, bool isAngle);

}  // namespace Assembly
