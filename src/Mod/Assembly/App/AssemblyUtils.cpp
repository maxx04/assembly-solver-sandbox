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

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Sphere.hxx>


#include <App/Application.h>
#include <App/Datums.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <App/PropertyStandard.h>
#include <App/Link.h>

#include <Base/Placement.h>
#include <Base/Tools.h>
#include <Base/Interpreter.h>

#include <Mod/Part/App/DatumFeature.h>
#include <Mod/Part/App/LinkArray.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/PartDesign/App/Body.h>

#include "AssemblyUtils.h"
#include "AssemblyObject.h"
#include "AssemblyLink.h"

#include "Groups.h"


namespace PartApp = Part;

// ======================================= Utils ======================================
namespace Assembly
{

bool isSuppressedLinkElement(const App::DocumentObject* obj)
{
    if (!obj || !obj->isDerivedFrom<App::LinkElement>()) {
        return false;
    }
    auto* ext = obj->getExtension<App::SuppressibleExtension>();
    return ext && ext->Suppressed.getValue();
}

void swapJCS(const App::DocumentObject* joint)
{
    if (!joint) {
        return;
    }

    auto pPlc1 = joint->getPropertyByName<App::PropertyPlacement>("Placement1");
    auto pPlc2 = joint->getPropertyByName<App::PropertyPlacement>("Placement2");
    if (pPlc1 && pPlc2) {
        const auto temp = pPlc1->getValue();
        pPlc1->setValue(pPlc2->getValue());
        pPlc2->setValue(temp);
    }
    auto pRef1 = joint->getPropertyByName<App::PropertyXLinkSub>("Reference1");
    auto pRef2 = joint->getPropertyByName<App::PropertyXLinkSub>("Reference2");
    if (pRef1 && pRef2) {
        auto temp = pRef1->getValue();
        auto subs1 = pRef1->getSubValues();
        auto subs2 = pRef2->getSubValues();
        pRef1->setValue(pRef2->getValue());
        pRef1->setSubValues(std::move(subs2));
        pRef2->setValue(temp);
        pRef2->setSubValues(std::move(subs1));
    }
}

bool isEdgeType(const App::DocumentObject* obj, const std::string& elName, const GeomAbs_CurveType type)
{
    auto* base = dynamic_cast<const PartApp::Feature*>(obj);
    if (!base) {
        return false;
    }

    const auto& TopShape = base->Shape.getShape();

    // Check for valid face types
    const auto edge = TopoDS::Edge(TopShape.getSubShape(elName.c_str()));
    BRepAdaptor_Curve sf(edge);

    return sf.GetType() == type;
}

bool isFaceType(const App::DocumentObject* obj, const std::string& elName, const GeomAbs_SurfaceType type)
{
    auto* base = dynamic_cast<const PartApp::Feature*>(obj);
    if (!base) {
        return false;
    }

    const auto TopShape = base->Shape.getShape();

    // Check for valid face types
    const auto face = TopoDS::Face(TopShape.getSubShape(elName.c_str()));
    BRepAdaptor_Surface sf(face);

    return sf.GetType() == type;
}

double getFaceRadius(const App::DocumentObject* obj, const std::string& elt)
{
    auto* base = dynamic_cast<const PartApp::Feature*>(obj);
    if (!base) {
        return 0.0;
    }

    const PartApp::TopoShape& TopShape = base->Shape.getShape();

    // Check for valid face types
    TopoDS_Face face = TopoDS::Face(TopShape.getSubShape(elt.c_str()));
    BRepAdaptor_Surface sf(face);

    const auto type = sf.GetType();
    return type == GeomAbs_Cylinder ? sf.Cylinder().Radius()
        : type == GeomAbs_Sphere    ? sf.Sphere().Radius()
                                    : 0.0;
}

double getEdgeRadius(const App::DocumentObject* obj, const std::string& elt)
{
    auto* base = dynamic_cast<const PartApp::Feature*>(obj);
    if (!base) {
        return 0.0;
    }

    const auto& TopShape = base->Shape.getShape();

    // Check for valid face types
    const auto edge = TopoDS::Edge(TopShape.getSubShape(elt.c_str()));
    BRepAdaptor_Curve sf(edge);

    return sf.GetType() == GeomAbs_Circle ? sf.Circle().Radius() : 0.0;
}

DistanceType getDistanceType(App::DocumentObject* joint)
{
    if (!joint) {
        return DistanceType::Other;
    }

    const auto type1 = getElementTypeFromProp(joint, "Reference1");
    const auto type2 = getElementTypeFromProp(joint, "Reference2");
    auto elt1 = getElementFromProp(joint, "Reference1");
    auto elt2 = getElementFromProp(joint, "Reference2");
    auto* obj1 = getLinkedObjFromRef(joint, "Reference1");
    auto* obj2 = getLinkedObjFromRef(joint, "Reference2");

    if (type1 == "Vertex" && type2 == "Vertex") {
        return DistanceType::PointPoint;
    }
    else if (type1 == "Edge" && type2 == "Edge") {
        if (isEdgeType(obj1, elt1, GeomAbs_Line) || isEdgeType(obj2, elt2, GeomAbs_Line)) {
            if (!isEdgeType(obj1, elt1, GeomAbs_Line)) {
                swapJCS(joint);  // make sure that line is first if not 2 lines.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isEdgeType(obj2, elt2, GeomAbs_Line)) {
                return DistanceType::LineLine;
            }
            else if (isEdgeType(obj2, elt2, GeomAbs_Circle)) {
                return DistanceType::LineCircle;
            }
            // TODO : other cases Ellipse, parabola, hyperbola...
        }

        else if (isEdgeType(obj1, elt1, GeomAbs_Circle) || isEdgeType(obj2, elt2, GeomAbs_Circle)) {
            if (!isEdgeType(obj1, elt1, GeomAbs_Circle)) {
                swapJCS(joint);  // make sure that circle is first if not 2 lines.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isEdgeType(obj2, elt2, GeomAbs_Circle)) {
                return DistanceType::CircleCircle;
            }
            // TODO : other cases Ellipse, parabola, hyperbola...
        }
    }
    else if (type1 == "Face" && type2 == "Face") {
        if (isFaceType(obj1, elt1, GeomAbs_Plane) || isFaceType(obj2, elt2, GeomAbs_Plane)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Plane)) {
                swapJCS(joint);  // make sure plane is first if its not 2 planes.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Plane)) {
                return DistanceType::PlanePlane;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Cylinder)) {
                return DistanceType::PlaneCylinder;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::PlaneSphere;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Cone)) {
                return DistanceType::PlaneCone;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Torus)) {
                return DistanceType::PlaneTorus;
            }
        }

        else if (isFaceType(obj1, elt1, GeomAbs_Cylinder) || isFaceType(obj2, elt2, GeomAbs_Cylinder)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Cylinder)) {
                swapJCS(joint);  // make sure cylinder is first if its not 2 cylinders.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Cylinder)) {
                return DistanceType::CylinderCylinder;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::CylinderSphere;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Cone)) {
                return DistanceType::CylinderCone;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Torus)) {
                return DistanceType::CylinderTorus;
            }
        }

        else if (isFaceType(obj1, elt1, GeomAbs_Cone) || isFaceType(obj2, elt2, GeomAbs_Cone)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Cone)) {
                swapJCS(joint);  // make sure cone is first if its not 2 cones.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Cone)) {
                return DistanceType::ConeCone;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Torus)) {
                return DistanceType::ConeTorus;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::ConeSphere;
            }
        }

        else if (isFaceType(obj1, elt1, GeomAbs_Torus) || isFaceType(obj2, elt2, GeomAbs_Torus)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Torus)) {
                swapJCS(joint);  // make sure torus is first if its not 2 torus.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Torus)) {
                return DistanceType::TorusTorus;
            }
            else if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::TorusSphere;
            }
        }

        else if (isFaceType(obj1, elt1, GeomAbs_Sphere) || isFaceType(obj2, elt2, GeomAbs_Sphere)) {
            if (!isFaceType(obj1, elt1, GeomAbs_Sphere)) {
                swapJCS(joint);  // make sure sphere is first if its not 2 spheres.
                std::swap(elt1, elt2);
                std::swap(obj1, obj2);
            }

            if (isFaceType(obj2, elt2, GeomAbs_Sphere)) {
                return DistanceType::SphereSphere;
            }
        }
    }
    else if ((type1 == "Vertex" && type2 == "Face") || (type1 == "Face" && type2 == "Vertex")) {
        if (type1 == "Vertex") {  // Make sure face is the first.
            swapJCS(joint);
            std::swap(elt1, elt2);
            std::swap(obj1, obj2);
        }
        if (isFaceType(obj1, elt1, GeomAbs_Plane)) {
            return DistanceType::PointPlane;
        }
        else if (isFaceType(obj1, elt1, GeomAbs_Cylinder)) {
            return DistanceType::PointCylinder;
        }
        else if (isFaceType(obj1, elt1, GeomAbs_Sphere)) {
            return DistanceType::PointSphere;
        }
        else if (isFaceType(obj1, elt1, GeomAbs_Cone)) {
            return DistanceType::PointCone;
        }
        else if (isFaceType(obj1, elt1, GeomAbs_Torus)) {
            return DistanceType::PointTorus;
        }
    }
    else if ((type1 == "Edge" && type2 == "Face") || (type1 == "Face" && type2 == "Edge")) {
        if (type1 == "Edge") {  // Make sure face is the first.
            swapJCS(joint);
            std::swap(elt1, elt2);
            std::swap(obj1, obj2);
        }
        if (isEdgeType(obj2, elt2, GeomAbs_Line)) {
            if (isFaceType(obj1, elt1, GeomAbs_Plane)) {
                return DistanceType::LinePlane;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Cylinder)) {
                return DistanceType::LineCylinder;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Sphere)) {
                return DistanceType::LineSphere;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Cone)) {
                return DistanceType::LineCone;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Torus)) {
                return DistanceType::LineTorus;
            }
        }
        else {
            // For other curves we consider them as planes for now. Can be refined later.
            if (isFaceType(obj1, elt1, GeomAbs_Plane)) {
                return DistanceType::CurvePlane;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Cylinder)) {
                return DistanceType::CurveCylinder;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Sphere)) {
                return DistanceType::CurveSphere;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Cone)) {
                return DistanceType::CurveCone;
            }
            else if (isFaceType(obj1, elt1, GeomAbs_Torus)) {
                return DistanceType::CurveTorus;
            }
        }
    }
    else if ((type1 == "Vertex" && type2 == "Edge") || (type1 == "Edge" && type2 == "Vertex")) {
        if (type1 == "Vertex") {  // Make sure edge is the first.
            swapJCS(joint);
            std::swap(elt1, elt2);
            std::swap(obj1, obj2);
        }
        if (isEdgeType(obj1, elt1, GeomAbs_Line)) {  // Point on line joint.
            return DistanceType::PointLine;
        }
        else {
            // For other curves we do a point in plane-of-the-curve.
            // Maybe it would be best tangent / distance to the conic? For arcs and
            // circles we could use ASMTRevSphJoint. But is it better than pointInPlane?
            return DistanceType::PointCurve;
        }
    }
    return DistanceType::Other;
}

JointGroup* getJointGroup(const App::Part* part)
{
    if (!part) {
        return nullptr;
    }

    const auto* doc = part->getDocument();

    const auto jointGroups = doc->getObjectsOfType(JointGroup::getClassTypeId());
    if (jointGroups.empty()) {
        return nullptr;
    }
    for (auto jointGroup : jointGroups) {
        if (part->hasObject(jointGroup)) {
            return freecad_cast<JointGroup*>(jointGroup);
        }
    }
    return nullptr;
}

void setJointActivated(const App::DocumentObject* joint, bool val)
{
    if (!joint) {
        return;
    }

    if (auto propSuppressed = joint->getPropertyByName<App::PropertyBool>("Suppressed")) {
        propSuppressed->setValue(!val);
    }
}

bool getJointActivated(const App::DocumentObject* joint)
{
    if (!joint) {
        return false;
    }

    if (const auto propActivated = joint->getPropertyByName<App::PropertyBool>("Suppressed")) {
        return !propActivated->getValue();
    }
    return false;
}

double getJointDistance(const App::DocumentObject* joint, const char* propertyName)
{
    if (!joint) {
        return 0.0;
    }

    const auto* prop = joint->getPropertyByName<App::PropertyFloat>(propertyName);
    if (!prop) {
        return 0.0;
    }

    return prop->getValue();
}

double getJointAngle(const App::DocumentObject* joint)
{
    return getJointDistance(joint, "Angle");
}

double getJointDistance(const App::DocumentObject* joint)
{
    return getJointDistance(joint, "Distance");
}

double getJointDistance2(const App::DocumentObject* joint)
{
    return getJointDistance(joint, "Distance2");
}

JointType getJointType(const App::DocumentObject* joint)
{
    if (!joint) {
        return JointType::Fixed;
    }

    const auto* prop = joint->getPropertyByName<App::PropertyEnumeration>("JointType");
    if (!prop) {
        return JointType::Fixed;
    }

    return static_cast<JointType>(prop->getValue());
}

std::vector<std::string> getSubAsList(const App::PropertyXLinkSub* prop)
{
    if (!prop) {
        return {};
    }

    const auto subs = prop->getSubValues();
    if (subs.empty()) {
        return {};
    }

    return Base::Tools::splitSubName(subs[0]);
}

std::vector<std::string> getSubAsList(const App::DocumentObject* obj, const char* pName)
{
    if (!obj) {
        return {};
    }
    return getSubAsList(obj->getPropertyByName<App::PropertyXLinkSub>(pName));
}

std::string getElementFromProp(const App::DocumentObject* obj, const char* pName)
{
    if (!obj) {
        return "";
    }

    const auto names = getSubAsList(obj, pName);
    if (names.empty()) {
        return "";
    }

    return names.back();
}

std::string getElementTypeFromProp(const App::DocumentObject* obj, const char* propName)
{
    // The prop is going to be something like 'Edge14' or 'Face7'. We need 'Edge' or 'Face'
    std::string elementType;
    for (const char ch : getElementFromProp(obj, propName)) {
        if (std::isalpha(ch)) {
            elementType += ch;
        }
    }
    return elementType;
}

App::DocumentObject* getObjFromProp(const App::DocumentObject* joint, const char* pName)
{
    if (!joint) {
        return {};
    }

    const auto* propObj = joint->getPropertyByName<App::PropertyLink>(pName);
    if (!propObj) {
        return {};
    }

    return propObj->getValue();
}

App::DocumentObject* getObjFromRef(App::DocumentObject* comp, const std::string& sub)
{
    if (!comp) {
        return nullptr;
    }

    const auto* doc = comp->getDocument();
    auto names = Base::Tools::splitSubName(sub);
    names.insert(names.begin(), comp->getNameInDocument());

    if (names.size() <= 2) {
        return comp;
    }

    // Lambda function to check if the typeId is a BodySubObject
    const auto isBodySubObject = [](App::DocumentObject* obj) -> bool {
        // PartDesign::Point + Line + Plane + CoordinateSystem
        // getViewProviderName instead of isDerivedFrom to avoid dependency on sketcher
        const auto isDerivedFromVpSketch
            = strcmp(obj->getViewProviderName(), "SketcherGui::ViewProviderSketch") == 0;
        return isDerivedFromVpSketch || obj->isDerivedFrom<PartApp::Datum>()
            || obj->isDerivedFrom<App::DatumElement>()
            || obj->isDerivedFrom<App::LocalCoordinateSystem>();
    };

    // Helper function to handle PartDesign::Body objects
    const auto handlePartDesignBody =
        [&](App::DocumentObject* obj,
            std::vector<std::string>::const_iterator it) -> App::DocumentObject* {
        auto nextIt = std::next(it);
        if (nextIt != names.end()) {
            for (auto* obji : obj->getOutList()) {
                if (*nextIt == obji->getNameInDocument() && isBodySubObject(obji)) {
                    // if obji is a LCS then perhaps we need to resolve one more level
                    if (auto* lcs = freecad_cast<App::LocalCoordinateSystem*>(obji)) {
                        nextIt = std::next(nextIt);
                        if (nextIt != names.end()) {
                            for (auto* objj : lcs->baseObjects()) {
                                if (*nextIt == objj->getNameInDocument()
                                    && objj->isDerivedFrom<App::DatumElement>()) {
                                    return objj;
                                }
                            }
                        }
                    }
                    return obji;
                }
            }
        }
        return obj;
    };


    for (auto it = names.begin(); it != names.end(); ++it) {
        App::DocumentObject* obj = doc->getObject(it->c_str());
        if (!obj) {
            return nullptr;
        }

        if (obj->isDerivedFrom<App::DocumentObjectGroup>()) {
            continue;
        }

        // The last but one name should be the selected
        if (std::next(it) == std::prev(names.end())) {
            return obj;
        }

        if (obj->isDerivedFrom<App::Part>() || obj->isLinkGroup()) {
            continue;
        }
        else if (obj->isDerivedFrom<PartDesign::Body>()) {
            return handlePartDesignBody(obj, it);
        }
        else if (obj->isDerivedFrom<PartApp::LinkArray>() || obj->isDerivedFrom<PartApp::Feature>()) {
            // Primitive, fastener, gear, etc.
            return obj;
        }
        else if (obj->isLink()) {
            App::DocumentObject* linked_obj = obj->getLinkedObject();
            if (linked_obj->isDerivedFrom<PartDesign::Body>()) {
                auto* retObj = handlePartDesignBody(linked_obj, it);
                return retObj == linked_obj ? obj : retObj;
            }
            else if (linked_obj->isDerivedFrom<PartApp::LinkArray>()) {
                return obj;
            }
            else if (linked_obj->isDerivedFrom<PartApp::Feature>()) {
                return obj;
            }
            else {
                doc = linked_obj->getDocument();
                continue;
            }
        }
    }

    return nullptr;
}

App::DocumentObject* getObjFromRef(const App::PropertyXLinkSub* prop)
{
    if (!prop) {
        return nullptr;
    }

    App::DocumentObject* obj = prop->getValue();
    if (!obj) {
        return nullptr;
    }

    const std::vector<std::string> subs = prop->getSubValues();
    if (subs.empty()) {
        return nullptr;
    }

    return getObjFromRef(obj, subs[0]);
}

App::DocumentObject* getObjFromJointRef(const App::DocumentObject* joint, const char* pName)
{
    if (!joint) {
        return nullptr;
    }

    const auto* prop = joint->getPropertyByName<App::PropertyXLinkSub>(pName);
    return getObjFromRef(prop);
}

App::DocumentObject* getLinkedObjFromRef(const App::DocumentObject* joint, const char* pObj)
{
    if (!joint) {
        return nullptr;
    }

    if (const auto* obj = getObjFromJointRef(joint, pObj)) {
        return obj->getLinkedObject(true);
    }
    return nullptr;
}

// FCPROJECT-PATCH (Bug C, "Instanz-Identitaet innerhalb einer duplizierten flexiblen
// Unterbaugruppe", 2026-09-16/17): siehe Deklaration in AssemblyUtils.h fuer die volle
// Begruendung. Generische Fassung - dies ist seit 2026-09-17 die einzige Implementierung, die
// AssemblyObject-Fassung darunter ist nur noch ein duenner Wrapper (candidates =
// solvingAssembly->getSubAssemblies() als oberste Ebene).
bool hasSiblingInstances(const std::vector<App::DocumentObject*>& candidates, AssemblyLink* asmLink)
{
    if (!asmLink) {
        return false;
    }

    auto* linkedAssembly = asmLink->getLinkedAssembly();
    if (!linkedAssembly) {
        return false;
    }

    int matches = 0;
    for (auto* candidate : candidates) {
        auto* sibling = freecad_cast<AssemblyLink*>(candidate);
        if (sibling && sibling->getLinkedAssembly() == linkedAssembly) {
            ++matches;
            if (matches >= 2) {
                return true;
            }
        }
    }
    return false;
}

bool hasSiblingInstances(const AssemblyObject* solvingAssembly, AssemblyLink* asmLink)
{
    if (!solvingAssembly) {
        return false;
    }
    auto subAssemblies = solvingAssembly->getSubAssemblies();
    std::vector<App::DocumentObject*> candidates(subAssemblies.begin(), subAssemblies.end());
    return hasSiblingInstances(candidates, asmLink);
}

App::DocumentObject* getMovingPartFromSel(
    const AssemblyObject* assemblyObject,
    App::DocumentObject* obj,
    const std::string& sub,
    bool verboseLog
)
{
    if (!obj) {
        return nullptr;
    }

    auto* doc = obj->getDocument();

    auto names = Base::Tools::splitSubName(sub);
    names.insert(names.begin(), obj->getNameInDocument());

    bool assemblyPassed = false;

    // FCPROJECT-PATCH (Bug C, 2026-09-16): siehe resolveJointReference() fuer die volle
    // Herleitung - identischer Mechanismus fuer den GUI-Auswahl-/Drag-Pfad. Anders als dort
    // braucht diese Funktion kein separates "realObj" (keine JCS-Offset-Versoehnung noetig -
    // ein Drag manipuliert direkt die Placement-Property des zurueckgegebenen Objekts, ein
    // Spiegel hat eine eigene, echte Placement-Property, die genuegt) - deshalb genuegt die
    // Ersetzung inline, ohne Signaturaenderung.
    Assembly::AssemblyLink* lastDuplicatedCrossing = nullptr;

    // FCPROJECT-PATCH (2026-09-20, "BG25 Slider-Drag Instanz 2 komplett unziehbar" - siehe
    // todo-bg25-slider-drag-two-instances): der Segment-fuer-Segment Namens-Walk unten sucht
    // jeden Namen per doc->getObject() im JEWEILS AKTUELLEN 'doc' - sobald ein Segment eine
    // FLEXIBLE, duplizierte AssemblyLink kreuzt, wird 'doc' auf das GETEILTE Vorlage-Dokument
    // umgeschaltet (siehe Zeile mit 'doc = linkedAssembly->getDocument()' unten). Ein
    // NACHFOLGENDES Namenssegment kann aber selbst wieder ein TOP-DOKUMENT-Spiegelobjekt sein
    // (z.B. eine verschachtelte RIGIDE Unterbaugruppe wie "Halterbaugruppe003" - lebt als
    // eigenstaendiges Objekt in DERSELBEN Instanz-Gruppe wie der zuvor gekreuzte Container,
    // NICHT im geteilten Dokument). Bei der ERSTEN eingefuegten Instanz kollidiert das
    // zufaellig nie (deren Spiegelnamen "Halterbaugruppe"/"Halterbaugruppe001" treffen
    // zufaellig dieselben Namen wie BG25s EIGENE interne Duplikation), bei jeder WEITEREN
    // Instanz (hier automatisch auf "002"/"003" umbenannt) existiert dieser Name im geteilten
    // Dokument nicht - doc->getObject() liefert nullptr, der gesamte Walk bricht ab (return
    // nullptr) - live reproduziert: Vorauswahl/Preselection funktioniert (nutzt einen anderen
    // Coin3D-Mechanismus), aber der eigentliche Zug-Start (dieser Walk) liefert 'part'=<null>.
    // Fix: bei einem gescheiterten doc->getObject() zusaetzlich in der Group des zuletzt
    // erfolgreich aufgeloesten Objekts nach demselben Namen suchen (das ist exakt die Gruppe,
    // in der ein TOP-DOKUMENT-Spiegel eines VERSCHACHTELTEN Containers liegt) - funktioniert
    // rekursiv fuer beliebige Verschachtelungstiefe, weil 'previousObj' bei jedem Schritt
    // aktualisiert wird, nicht nur beim ERSTEN Kreuzen einer duplizierten Instanz.
    App::DocumentObject* previousObj = nullptr;

    // FCPROJECT-PATCH (Teilschritt 3.1b): der Namens-Walk aus der urspruenglichen
    // Befund-3-Live-Diagnose 2026-09-03 ist inzwischen nachvollzogen - Logs bleiben fuer
    // kuenftige Diagnosen erhalten, laufen aber nur noch hinter verboseLog, da diese Funktion
    // bei jedem Selektions-/Zieh-Ereignis aufgerufen wird.
    if (verboseLog) {
        Base::Console().log(
            "FCPROJECT-DEBUG getMovingPartFromSel: assemblyObject='%s', selRoot='%s', sub='%s', "
            "names=[%s]\n",
            assemblyObject ? assemblyObject->getFullName().c_str() : "<null>",
            obj->getFullName().c_str(),
            sub.c_str(),
            [&names]() {
                std::string joined;
                for (const auto& n : names) {
                    if (!joined.empty()) {
                        joined += ", ";
                    }
                    joined += n;
                }
                return joined;
            }()
                .c_str()
        );
    }

    for (const auto& objName : names) {
        obj = doc->getObject(objName.c_str());
        if (!obj && previousObj) {
            // FCPROJECT-PATCH (2026-09-20, siehe Deklaration von 'previousObj' oben): Fallback -
            // dieser Name gehoert zu einem TOP-DOKUMENT-Spiegel, der als Kind des zuletzt
            // aufgeloesten Objekts lebt (nicht im aktuellen, evtl. bereits umgeschalteten 'doc').
            if (auto* prevAsGroup = freecad_cast<Assembly::AssemblyLink*>(previousObj)) {
                for (auto* candidate : prevAsGroup->Group.getValues()) {
                    if (candidate && objName == candidate->getNameInDocument()) {
                        obj = candidate;
                        doc = obj->getDocument();
                        break;
                    }
                }
            }
        }
        if (verboseLog) {
            Base::Console().log(
                "FCPROJECT-DEBUG   step name='%s' in doc='%s' -> obj='%s'\n",
                objName.c_str(),
                doc->getName(),
                obj ? obj->getFullName().c_str() : "<not found>"
            );
        }
        if (!obj) {
            continue;
        }
        previousObj = obj;

        if (obj->isLink()) {  // update the document if necessary for next object
            doc = obj->getLinkedObject()->getDocument();
            if (verboseLog) {
                Base::Console().log(
                    "FCPROJECT-DEBUG     isLink() -> doc switched to '%s'\n",
                    doc->getName()
                );
            }
        }

        if (obj == assemblyObject) {
            // We make sure we pass the assembly for cases like part.assembly.part.body
            assemblyPassed = true;
            continue;
        }
        if (!assemblyPassed) {
            continue;
        }

        if (obj->isDerivedFrom<App::DocumentObjectGroup>()) {
            continue;  // we ignore groups.
        }

        if (obj->isLinkGroup()) {
            continue;
        }

        // We ignore dynamic sub-assemblies.
        if (obj->isDerivedFrom<Assembly::AssemblyLink>()) {
            const auto* pRigid = obj->getPropertyByName<App::PropertyBool>("Rigid");
            if (pRigid && !pRigid->getValue()) {
                // FCPROJECT-PATCH (Fix-Ansatz D, "flexible Unterbaugruppe als Ganzes ziehbar,
                // wenn unverbunden", Nutzerauftrag 2026-09-16/17): eine KOMPLETT unverbundene
                // flexible AssemblyLink wird NICHT mehr transparent durchlaufen, sondern selbst
                // als gefundenes Objekt behandelt (wie eine starre) - sonst bekommt
                // canDragObjectIn3d() sie beim eigentlichen Maus-Drag (dieser Pfad hier, ueber
                // die Preselection, NICHT collectMovableObjects()) nie zu Gesicht, weil der
                // Sub-Pfad ohnehin schon in eines ihrer Kinder hineinzeigt. Eine TEILWEISE/voll
                // verbundene Instanz bleibt beim bisherigen, transparenten Verhalten.
                // isSubAssemblyFullyUnconnected() statt isPartConnected(asmLink) direkt
                // (2026-09-17) - ein Joint referenziert nie den Container selbst, sondern immer
                // ein Kind darin, siehe Deklaration in AssemblyObject.h.
                auto* asmLink = freecad_cast<Assembly::AssemblyLink*>(obj);
                if (asmLink
                    && const_cast<AssemblyObject*>(assemblyObject)
                           ->isSubAssemblyFullyUnconnected(asmLink)) {
                    if (verboseLog) {
                        Base::Console().log(
                            "FCPROJECT-DEBUG     flexible AssemblyLink '%s' unverbunden -> als "
                            "Ganzes behandelt (nicht durchlaufen)\n",
                            asmLink->getNameInDocument()
                        );
                    }
                    // Faellt durch zum "gefunden"-Fall unten, wie eine starre AssemblyLink.
                }
                else {
                    // FCPROJECT-PATCH (Befund 3, "Adressieren statt Kopieren", solver-root-cause-fix,
                    // 2026-09-03, live durch Nutzer-Maus-Drag aufgedeckt): derselbe Fehler wie in
                    // resolveJointReference() (siehe dortiger Kommentar) - Assembly::AssemblyLink
                    // erbt von App::Part, NICHT von App::Link, der obj->isLink()-Zweig weiter oben
                    // schaltet 'doc' fuer eine verschachtelte FLEXIBLE AssemblyLink deshalb NIE um.
                    // Ohne diesen Fix sucht der naechste Namens-Schritt des Walks weiterhin im
                    // AEUSSEREN Dokument statt im echten, verlinkten - liefert entweder nullptr
                    // (Klick/Zug bewegt gar nichts) oder trifft zufaellig eine gleichnamige lokale
                    // Spiegel-Kopie (Klick/Zug bewegt das FALSCHE Objekt, keine Joint-Einschraenkung
                    // greift - beobachtet als frei in jede Richtung ziehbare Box, 2 Ebenen tief
                    // verschachtelt).
                    if (asmLink) {
                        if (auto* linkedAssembly = asmLink->getLinkedAssembly()) {
                            doc = linkedAssembly->getDocument();
                            if (hasSiblingInstances(assemblyObject, asmLink)) {
                                lastDuplicatedCrossing = asmLink;
                            }
                            if (verboseLog) {
                                Base::Console().log(
                                    "FCPROJECT-DEBUG     flexible AssemblyLink -> doc switched to "
                                    "'%s'\n",
                                    doc->getName()
                                );
                            }
                        }
                        else if (verboseLog) {
                            Base::Console().log(
                                "FCPROJECT-DEBUG     flexible AssemblyLink but getLinkedAssembly()==null!\n"
                            );
                        }
                    }
                    continue;
                }
            }
        }

        // FCPROJECT-PATCH (Bug C, 2026-09-16): siehe resolveJointReference() fuer die
        // Begruendung - 'obj' liegt im geteilten, echten Dokument der zuletzt gekreuzten
        // duplizierten Instanz; ist es ein direktes Top-Level-Group-Kind dieser Instanz, liefert
        // objLinkMap den Spiegel DIESER Instanz statt des rohen, instanzblinden Zeigers.
        if (lastDuplicatedCrossing) {
            auto it = lastDuplicatedCrossing->objLinkMap.find(obj);
            if (it != lastDuplicatedCrossing->objLinkMap.end()) {
                obj = it->second;
            }
        }

        if (verboseLog) {
            Base::Console().log(
                "FCPROJECT-DEBUG   -> RETURN '%s'\n",
                obj->getFullName().c_str()
            );
        }
        return obj;
    }
    if (verboseLog) {
        Base::Console().log("FCPROJECT-DEBUG   -> RETURN nullptr\n");
    }
    return nullptr;
}

App::DocumentObject* getMovingPartFromRef(App::PropertyXLinkSub* prop)
{
    if (!prop) {
        return nullptr;
    }

    return prop->getValue();
}

App::DocumentObject* getMovingPartFromRef(App::DocumentObject* joint, const char* pName)
{
    if (!joint) {
        return nullptr;
    }

    auto* prop = joint->getPropertyByName<App::PropertyXLinkSub>(pName);
    return getMovingPartFromRef(prop);
}

// FCPROJECT-PATCH (Teilschritt 2 "adressieren statt kopieren", solver-root-cause-fix): siehe
// ausfuehrliche Erklaerung am Deklarationsort in AssemblyUtils.h. Rein diagnostisch/
// adressierungsbewusst, aendert fuer sich genommen nichts am Solver-Verhalten - genutzt von
// AssemblyObject::resolvePartForMbD().
ResolvedJointRef resolveJointReference(
    const AssemblyObject* solvingAssembly,
    App::DocumentObject* joint,
    const char* pName,
    const std::string& nestingPrefix
)
{
    if (!solvingAssembly || !joint || !pName) {
        return {};
    }

    const auto* prop = joint->getPropertyByName<App::PropertyXLinkSub>(pName);
    if (!prop) {
        return {};
    }

    App::DocumentObject* refObj = prop->getValue();
    if (!refObj) {
        return {};
    }

    const std::vector<std::string> subs = prop->getSubValues();
    const std::string localSub = subs.empty() ? std::string() : subs[0];

    // Full address, constructed so that it already starts "inside" solvingAssembly - see the
    // header comment for why no assemblyPassed-style gate is needed here, unlike
    // getMovingPartFromSel().
    std::string fullAddress = nestingPrefix + refObj->getNameInDocument();
    if (!localSub.empty()) {
        fullAddress += "." + localSub;
    }

    const auto names = Base::Tools::splitSubName(fullAddress);
    if (names.empty()) {
        return {};
    }

    App::Document* doc = solvingAssembly->getDocument();
    if (!doc) {
        return {};
    }

    // FCPROJECT-PATCH (Bug C, "Instanz-Identitaet innerhalb einer duplizierten flexiblen
    // Unterbaugruppe", 2026-09-16): merkt sich die zuletzt gekreuzte AssemblyLink-Instanz, aber
    // NUR wenn sie tatsaechlich dupliziert ist (hasSiblingInstances()) - siehe
    // docs/ARCHITECTURE.md §4.1 "Bug C" fuer die volle Herleitung. Wird am Ende genutzt, um das
    // gefundene, sonst instanzblinde reale Objekt per Vorwaertslookup in objLinkMap durch den
    // Spiegel DIESER konkreten Instanz zu ersetzen.
    Assembly::AssemblyLink* lastDuplicatedCrossing = nullptr;

    for (size_t i = 0; i < names.size(); ++i) {
        const std::string& objName = names[i];
        if (objName.empty()) {
            // Trailing empty segment from a trailing '.' (e.g. an empty element name) - nothing
            // left to resolve as an object.
            break;
        }

        App::DocumentObject* obj = doc->getObject(objName.c_str());
        if (!obj) {
            // Defensive: unlike getMovingPartFromSel() (which silently skips an unresolved
            // segment and keeps walking), resolveJointReference() must never guess - a segment
            // that cannot be found means the nestingPrefix/reference combination does not
            // actually address anything, and the caller falls back to getMovingPartFromRef().
            return {};
        }

        if (obj->isLink()) {
            // Cross-document boundary (e.g. a PDM-style App::Link to a part's own .FCStd file) -
            // subsequent segments must be looked up in the linked document.
            if (auto* linkedObj = obj->getLinkedObject()) {
                doc = linkedObj->getDocument();
            }
        }

        // FCPROJECT-PATCH (Root-Cause-Fix "Befund 3", solver-root-cause-fix, siehe
        // patches/bugreport-nested-flex-placement-divergence/Questions.md): Assembly::AssemblyLink
        // erbt von App::Part, NICHT von App::Link - der obj->isLink()-Zweig oben (kopiert aus dem
        // bestehenden Vorbild getMovingPartFromSel(), das denselben Fehler traegt) greift daher
        // fuer eine verschachtelte AssemblyLink NIE, und 'doc' bleibt faelschlich beim
        // AUFRUFENDEN Dokument stehen. Ohne diesen Zweig findet der naechste Pfadabschnitt (z.B.
        // "unterAssembly" oder das eigentliche Zielteil) ein GLEICHNAMIGES Objekt im FALSCHEN,
        // AEUSSEREN Dokument (die vom alten Kopier-Mechanismus dort gespiegelte Kopie) statt im
        // tatsaechlich verlinkten, tiefer liegenden Dokument - genau das war die Root Cause hinter
        // Befund 3 (mehrere Verschachtelungsebenen loesen unabhaengig und liefern dauerhaft
        // widerspruechliche Placement-Werte fuer dasselbe logische Teil).
        if (auto* assemblyLink = freecad_cast<Assembly::AssemblyLink*>(obj)) {
            if (auto* linkedAssembly = assemblyLink->getLinkedAssembly()) {
                doc = linkedAssembly->getDocument();
                if (hasSiblingInstances(solvingAssembly, assemblyLink)) {
                    // "letzte" gekreuzte duplizierte Instanz - passend zu objLinkMaps
                    // Ein-Hop-Reichweite (siehe Deklaration in AssemblyUtils.h).
                    lastDuplicatedCrossing = assemblyLink;
                }
            }
        }

        if (obj->isDerivedFrom<App::DocumentObjectGroup>()) {
            continue;  // we ignore groups, same as getMovingPartFromSel().
        }

        if (obj->isLinkGroup()) {
            continue;
        }

        // We ignore dynamic (flexible) sub-assemblies - transparently walk through them, exactly
        // like getMovingPartFromSel() does.
        if (obj->isDerivedFrom<Assembly::AssemblyLink>()) {
            const auto* pRigid = obj->getPropertyByName<App::PropertyBool>("Rigid");
            if (pRigid && !pRigid->getValue()) {
                continue;
            }
        }

        // Found the actual moving part - whatever names are left over becomes the resolved
        // sub-path (the piece of information getMovingPartFromSel() does not need to report, but
        // that a solver-identity fix needs).
        ResolvedJointRef result;
        result.obj = obj;
        for (size_t j = i + 1; j < names.size(); ++j) {
            if (!result.subPath.empty()) {
                result.subPath += ".";
            }
            result.subPath += names[j];
        }

        // FCPROJECT-PATCH (Bug C, 2026-09-16): 'obj' liegt im GETEILTEN, echten Dokument der
        // zuletzt gekreuzten duplizierten Instanz - per sich selbst instanzblind. Ist 'obj' ein
        // direktes Top-Level-Group-Kind dieser Instanz (objLinkMap kennt es), liefert der
        // Vorwaertslookup den Spiegel DIESER Instanz - ein echtes, persistentes Objekt statt des
        // rohen geteilten Zeigers. Findet sich kein Eintrag (Objekt liegt tiefer als ein Hop
        // innerhalb der verlinkten Baugruppe), bleibt 'result.obj' unveraendert - der Aufrufer
        // faellt dann auf die bestehende, instanzblinde canonicalizeForMbD()-Behandlung zurueck.
        if (lastDuplicatedCrossing) {
            auto it = lastDuplicatedCrossing->objLinkMap.find(obj);
            if (it != lastDuplicatedCrossing->objLinkMap.end()) {
                result.obj = it->second;
                result.resolvedViaInstanceMirror = true;
            }
        }
        return result;
    }

    return {};
}

void syncPlacements(App::DocumentObject* src, App::DocumentObject* to)
{
    auto* plcPropSource = dynamic_cast<App::PropertyPlacement*>(src->getPropertyByName("Placement"));
    auto* plcPropLink = dynamic_cast<App::PropertyPlacement*>(to->getPropertyByName("Placement"));

    if (plcPropSource && plcPropLink) {
        if (!plcPropSource->getValue().isSame(plcPropLink->getValue())) {
            plcPropLink->setValue(plcPropSource->getValue());
        }
    }
}
namespace
{
// Helper function to perform the recursive traversal. Kept in an anonymous
// namespace as it's an implementation detail of getAssemblyComponents.
void collectComponentsRecursively(
    const std::vector<App::DocumentObject*>& objects,
    std::vector<App::DocumentObject*>& results
)
{
    for (auto* obj : objects) {
        if (!obj || isSuppressedLinkElement(obj)) {
            continue;
        }

        if (auto* asmLink = freecad_cast<Assembly::AssemblyLink*>(obj)) {
            // If the sub-assembly is rigid, treat it as a single movable part.
            // If it's flexible, we need to check its individual components.
            if (asmLink->isRigid()) {
                results.push_back(asmLink);
            }
            else {
                collectComponentsRecursively(asmLink->Group.getValues(), results);
            }
            continue;
        }
        else if (obj->isLinkGroup()) {
            auto* linkGroup = static_cast<App::Link*>(obj);
            for (auto* elt : linkGroup->ElementList.getValues()) {
                if (!elt || isSuppressedLinkElement(elt)) {
                    continue;
                }
                results.push_back(elt);
            }
            continue;
        }
        else if (obj->isDerivedFrom<PartApp::LinkArray>()) {
            results.push_back(obj);
            continue;
        }
        else if (auto* group = freecad_cast<App::DocumentObjectGroup*>(obj)) {
            collectComponentsRecursively(group->Group.getValues(), results);
            continue;
        }
        else if (auto* link = freecad_cast<App::Link*>(obj)) {
            obj = link->getLinkedObject();
            if (!obj) {
                continue;
            }
            if (obj->isDerivedFrom<PartApp::LinkArray>()
                || (obj->isDerivedFrom<App::GeoFeature>()
                    && !obj->isDerivedFrom<App::LocalCoordinateSystem>())) {
                results.push_back(link);
            }
        }

        else if (
            obj->isDerivedFrom<App::GeoFeature>() && !obj->isDerivedFrom<App::LocalCoordinateSystem>()
        ) {
            results.push_back(obj);
        }
    }
}
}  // namespace

std::vector<App::DocumentObject*> getAssemblyComponents(const AssemblyObject* assembly)
{
    if (!assembly) {
        return {};
    }

    std::vector<App::DocumentObject*> components;
    collectComponentsRecursively(assembly->Group.getValues(), components);
    return components;
}

double getJointCurrentValue(App::DocumentObject* joint, bool isAngle)
{
    Base::Placement plc1 = App::GeoFeature::getPlacementFromProp(joint, "Placement1");
    Base::Placement plc2 = App::GeoFeature::getPlacementFromProp(joint, "Placement2");

    auto* ref1 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference1"));
    auto* ref2 = dynamic_cast<App::PropertyXLinkSub*>(joint->getPropertyByName("Reference2"));
    if (!ref1 || !ref2) {
        return 0.0;
    }
    Base::Placement obj_global_plc1 = App::GeoFeature::getGlobalPlacement(nullptr, ref1);
    Base::Placement obj_global_plc2 = App::GeoFeature::getGlobalPlacement(nullptr, ref2);

    plc1 = obj_global_plc1 * plc1;
    plc2 = obj_global_plc2 * plc2;

    Base::Placement plc3 = plc1.inverse() * plc2;

    if (isAngle) {
        Base::Vector3d x_axis = plc3.getRotation().multVec(Base::Vector3d(1, 0, 0));
        return std::atan2(x_axis.y, x_axis.x);
    }
    return (plc1.getPosition() - plc2.getPosition()).Length()
        * (plc3.getPosition().z < 0 ? -1.0 : 1.0);
}
}  // namespace Assembly
