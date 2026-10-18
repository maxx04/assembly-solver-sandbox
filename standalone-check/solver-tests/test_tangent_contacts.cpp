// Standalone tests for OndselSolver's Tangent joints besides point on curve:
// point on surface, curve on surface, curve on curve (ASMTTangentJoint).
//
// A grounded part carries geometry I (surface or curve), a free part carries J (point or curve).
// After solve (runPreDrag) and after drag steps (runDragStep) the checks measure independently of
// the joint: the point lies on the surface; the curve touches the surface or the other curve
// (smallest distance zero) without crossing it. Build and run: ./run-solver-tests.sh

#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "ASMTAssembly.h"
#include "ASMTCylindricalJoint.h"
#include "ASMTFixedJoint.h"
#include "ASMTMarker.h"
#include "ASMTPart.h"
#include "ASMTPrincipalMassMarker.h"
#include "ASMTTangentJoint.h"
#include "CREATE.h"
#include "NurbsCurve.h"
#include "NurbsSurface.h"

using namespace MbD;
using Vec3 = NurbsCurve::Vec3;

namespace {

int failures = 0;

std::string num(double x)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3g", x);
    return buf;
}

void check(bool ok, const std::string& what)
{
    std::printf("  %s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        failures++;
    }
}

struct Rot
{
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static Rot aboutX(double a)
    {
        Rot r;
        r.m[1][1] = std::cos(a);
        r.m[1][2] = -std::sin(a);
        r.m[2][1] = std::sin(a);
        r.m[2][2] = std::cos(a);
        return r;
    }
    static Rot aboutZ(double a)
    {
        Rot r;
        r.m[0][0] = std::cos(a);
        r.m[0][1] = -std::sin(a);
        r.m[1][0] = std::sin(a);
        r.m[1][1] = std::cos(a);
        return r;
    }
    Vec3 apply(const Vec3& v) const
    {
        return {
            m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2],
        };
    }
};

template<typename T>
void place(T& item, const Vec3& pos, const Rot& rot)
{
    item->setPosition3D(pos[0], pos[1], pos[2]);
    item->setRotationMatrix(rot.m[0][0], rot.m[0][1], rot.m[0][2], rot.m[1][0], rot.m[1][1],
                            rot.m[1][2], rot.m[2][0], rot.m[2][1], rot.m[2][2]);
}

double dist(const Vec3& a, const Vec3& b)
{
    double dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

std::shared_ptr<ASMTPart> makePart(const std::string& name, const Vec3& pos, const Rot& rot)
{
    auto part = CREATE<ASMTPart>::With();
    part->setName(name);
    auto mass = CREATE<ASMTPrincipalMassMarker>::With();
    mass->setMass(1.0);
    mass->setDensity(1.0);
    mass->setMomentOfInertias(1.0, 1.0, 1.0);
    part->setPrincipalMassMarker(mass);
    place(part, pos, rot);
    return part;
}

std::shared_ptr<ASMTMarker> makeMarker(const std::string& name, const Vec3& pos, const Rot& rot)
{
    auto marker = CREATE<ASMTMarker>::With();
    marker->setName(name);
    place(marker, pos, rot);
    return marker;
}

// ---------------------------------------------------------------- geometry

std::vector<double> flat(const std::vector<Vec3>& poles)
{
    std::vector<double> out;
    for (auto& p : poles) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

ASMTTangentJoint::Curve toCurve(const NurbsCurve& c)
{
    ASMTTangentJoint::Curve out;
    out.degree = c.degree;
    out.knots = c.knots;
    out.weights = c.weights;
    out.poles = flat(c.poles);
    out.closed = c.closed;
    return out;
}

ASMTTangentJoint::Surface toSurface(const NurbsSurface& s)
{
    ASMTTangentJoint::Surface out;
    out.degreeU = s.degreeU;
    out.degreeV = s.degreeV;
    out.nPolesU = s.nPolesU;
    out.nPolesV = s.nPolesV;
    out.knotsU = s.knotsU;
    out.knotsV = s.knotsV;
    out.weights = s.weights;
    out.poles = flat(s.poles);
    out.closedU = s.closedU;
    out.closedV = s.closedV;
    return out;
}

// Flat square [-60, 60]^2 in the xy plane (bilinear).
std::shared_ptr<NurbsSurface> flatSquare()
{
    std::vector<Vec3> poles = {{-60, -60, 0}, {-60, 60, 0}, {60, -60, 0}, {60, 60, 0}};
    return std::make_shared<NurbsSurface>(1, 1, std::vector<double>{0, 0, 1, 1},
                                          std::vector<double>{0, 0, 1, 1}, 2, 2,
                                          std::vector<double>(), poles, false, false);
}

// Bicubic bump over [-60, 60]^2: higher in the middle.
std::shared_ptr<NurbsSurface> bump()
{
    std::vector<Vec3> poles;
    double z[4][4] = {{0, 0, 0, 0}, {0, 12, 8, 0}, {0, 8, 12, 0}, {0, 0, 0, 0}};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            poles.push_back({-60.0 + 40.0 * i, -60.0 + 40.0 * j, z[i][j]});
        }
    }
    std::vector<double> knots = {0, 0, 0, 0, 1, 1, 1, 1};
    return std::make_shared<NurbsSurface>(3, 3, knots, knots, 4, 4, std::vector<double>(), poles,
                                          false, false);
}

// Exact full circle of radius r in the xy plane, closed rational quadratic.
std::shared_ptr<NurbsCurve> circle(double r)
{
    double w = std::sqrt(0.5);
    std::vector<Vec3> poles = {{r, 0, 0}, {r, r, 0}, {0, r, 0}, {-r, r, 0}, {-r, 0, 0},
                               {-r, -r, 0}, {0, -r, 0}, {r, -r, 0}, {r, 0, 0}};
    std::vector<double> weights = {1, w, 1, w, 1, w, 1, w, 1};
    std::vector<double> knots = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
    return std::make_shared<NurbsCurve>(2, knots, weights, poles, true);
}

// Open planar wave in the xy plane.
std::shared_ptr<NurbsCurve> wave()
{
    std::vector<Vec3> poles = {{-60, 0, 0}, {-30, 12, 0}, {0, -12, 0}, {30, 12, 0}, {60, 0, 0}};
    std::vector<double> knots = {0, 0, 0, 0, 0.5, 1, 1, 1, 1};
    return std::make_shared<NurbsCurve>(3, knots, std::vector<double>(), poles, false);
}

// ---------------------------------------------------------------- model

struct Model
{
    std::shared_ptr<ASMTAssembly> assembly;
    std::shared_ptr<ASMTPart> ground, mover;
    Vec3 markerJPos = {0, 0, 0};
    Rot markerJRot;

    // Rotation of the mover from its quaternion (same call order as FreeCAD).
    Rot moverRot() const
    {
        double q0, q1, q2, q3;
        mover->getQuarternions(q3, q0, q1, q2);
        double w = q3, a = q0, b = q1, c = q2;
        Rot r;
        double m[3][3] = {
            {1 - 2 * (b * b + c * c), 2 * (a * b - c * w), 2 * (a * c + b * w)},
            {2 * (a * b + c * w), 1 - 2 * (a * a + c * c), 2 * (b * c - a * w)},
            {2 * (a * c - b * w), 2 * (b * c + a * w), 1 - 2 * (a * a + b * b)},
        };
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                r.m[i][j] = m[i][j];
            }
        }
        return r;
    }

    Vec3 moverPos() const
    {
        double x, y, z;
        mover->getPosition3D(x, y, z);
        return {x, y, z};
    }

    // Point given in marker J coordinates, global.
    Vec3 globalJ(const Vec3& local) const
    {
        auto inPart = add(markerJPos, markerJRot.apply(local));
        return add(moverPos(), moverRot().apply(inPart));
    }

    void solve()
    {
        assembly->runPreDrag();
    }

    void dragTo(const Vec3& pos)
    {
        mover->updateMbDFromPosition3D(pos[0], pos[1], pos[2]);
        auto parts = std::make_shared<std::vector<std::shared_ptr<ASMTPart>>>();
        parts->push_back(mover);
        assembly->runDragStep(parts);
    }
};

// Ground at the origin, geometry I on a marker at the ground origin (identity), mover with marker J.
Model buildModel(const std::shared_ptr<ASMTTangentJoint>& joint, const Vec3& moverPos,
                 const Rot& moverRot, const Vec3& markerJPos, const Rot& markerJRot)
{
    Model model;
    model.markerJPos = markerJPos;
    model.markerJRot = markerJRot;
    model.assembly = CREATE<ASMTAssembly>::With();
    model.assembly->setName("OndselAssembly");
    model.ground = makePart("Ground", {0, 0, 0}, Rot());
    model.assembly->addPart(model.ground);
    model.mover = makePart("Mover", moverPos, moverRot);
    model.assembly->addPart(model.mover);

    auto fixAsm = makeMarker("marker-Ground", {0, 0, 0}, Rot());
    model.assembly->addMarker(fixAsm);
    auto fixPart = makeMarker("FixingMarker", {0, 0, 0}, Rot());
    model.ground->addMarker(fixPart);
    auto fixed = CREATE<ASMTFixedJoint>::With();
    fixed->setName("GroundedJoint");
    fixed->setMarkerI("/OndselAssembly/marker-Ground");
    fixed->setMarkerJ("/OndselAssembly/Ground/FixingMarker");
    model.assembly->addJoint(fixed);

    model.ground->addMarker(makeMarker("MarkerI", {0, 0, 0}, Rot()));
    model.mover->addMarker(makeMarker("MarkerJ", markerJPos, markerJRot));
    joint->setName("Tangent");
    joint->setMarkerI("/OndselAssembly/Ground/MarkerI");
    joint->setMarkerJ("/OndselAssembly/Mover/MarkerJ");
    model.assembly->addJoint(joint);
    return model;
}

bool solveOk(Model& model)
{
    try {
        model.solve();
        return true;
    }
    catch (const std::exception& e) {
        check(false, std::string("solve threw: ") + e.what());
        return false;
    }
}

// ---------------------------------------------------------------- independent measurements

// Signed distance of p to the surface (ground at origin, identity marker): dense sampling and
// local refinement by a pattern search, sign from the surface normal (+ = side of +z normal).
double signedDistanceToSurface(const NurbsSurface& s, const Vec3& p)
{
    const int n = 40;
    double bu = 0, bv = 0, best = 1e300;
    for (int i = 0; i <= n; i++) {
        for (int j = 0; j <= n; j++) {
            double u = s.firstU() + (s.lastU() - s.firstU()) * i / n;
            double v = s.firstV() + (s.lastV() - s.firstV()) * j / n;
            double d = dist(s.value(u, v), p);
            if (d < best) {
                best = d;
                bu = u;
                bv = v;
            }
        }
    }
    double step = (s.lastU() - s.firstU()) / n;
    while (step > 1e-13) {
        bool moved = false;
        for (int k = 0; k < 4; k++) {
            double u = bu + (k == 0 ? step : k == 1 ? -step : 0);
            double v = bv + (k == 2 ? step : k == 3 ? -step : 0);
            u = s.normalizeU(u);
            v = s.normalizeV(v);
            double d = dist(s.value(u, v), p);
            if (d < best) {
                best = d;
                bu = u;
                bv = v;
                moved = true;
            }
        }
        if (!moved) {
            step *= 0.5;
        }
    }
    auto f = s.value(bu, bv);
    auto nrm = s.normal(bu, bv);
    double side = (p[0] - f[0]) * nrm[0] + (p[1] - f[1]) * nrm[1] + (p[2] - f[2]) * nrm[2];
    return side >= 0 ? best : -best;
}

// Smallest signed distance over a curve given in marker J coordinates: sampling, then a
// golden-section refinement around the best sample.
// side: +1 measures along the surface normal, -1 against it.
double minSignedDistanceCurveToSurface(const Model& model, const NurbsCurve& c, const NurbsSurface& s,
                                       double side)
{
    auto f = [&](double t) { return side * signedDistanceToSurface(s, model.globalJ(c.value(t))); };
    double a = c.firstParameter(), b = c.lastParameter();
    const int n = 72;
    double bestT = a, best = 1e300;
    for (int i = 0; i <= n; i++) {
        double t = a + (b - a) * i / n;
        double d = f(t);
        if (d < best) {
            best = d;
            bestT = t;
        }
    }
    double lo = bestT - (b - a) / n, hi = bestT + (b - a) / n;
    const double g = (std::sqrt(5.0) - 1) / 2;
    for (int i = 0; i < 60; i++) {
        double t1 = hi - g * (hi - lo), t2 = lo + g * (hi - lo);
        if (f(c.normalizeParameter(t1)) < f(c.normalizeParameter(t2))) {
            hi = t2;
        }
        else {
            lo = t1;
        }
    }
    return std::min(best, f(c.normalizeParameter(0.5 * (lo + hi))));
}

// Distance from point p to a curve in global coordinates (ground identity), dense + golden.
double distanceToCurve(const NurbsCurve& c, const Vec3& p)
{
    double a = c.firstParameter(), b = c.lastParameter();
    const int n = 20000;
    double best = a, bestD = 1e300;
    for (int i = 0; i <= n; i++) {
        double u = a + (b - a) * i / n;
        double d = dist(c.value(u), p);
        if (d < bestD) {
            bestD = d;
            best = u;
        }
    }
    double lo = std::max(a, best - (b - a) / n), hi = std::min(b, best + (b - a) / n);
    const double g = (std::sqrt(5.0) - 1) / 2;
    for (int i = 0; i < 200; i++) {
        double u1 = hi - g * (hi - lo), u2 = lo + g * (hi - lo);
        if (dist(c.value(u1), p) < dist(c.value(u2), p)) {
            hi = u2;
        }
        else {
            lo = u1;
        }
    }
    return dist(c.value(0.5 * (lo + hi)), p);
}

// ---------------------------------------------------------------- tests

void testSurfaceEvaluation()
{
    std::printf("Surface evaluation\n");
    auto s = bump();
    double u = 0.37, v = 0.61, h = 1e-6;
    Vec3 p, su, sv, suu, suv, svv;
    s->evaluate(u, v, p, su, sv, suu, suv, svv);
    double e1 = 0, e2 = 0;
    for (int i = 0; i < 3; i++) {
        e1 = std::max(e1, std::fabs((s->value(u + h, v)[i] - s->value(u - h, v)[i]) / (2 * h) - su[i]));
        e1 = std::max(e1, std::fabs((s->value(u, v + h)[i] - s->value(u, v - h)[i]) / (2 * h) - sv[i]));
        double fuv = (s->value(u + h, v + h)[i] - s->value(u + h, v - h)[i] - s->value(u - h, v + h)[i]
                      + s->value(u - h, v - h)[i]) / (4 * h * h);
        e2 = std::max(e2, std::fabs(fuv - suv[i]));
    }
    check(e1 < 1e-4, "first derivatives match finite differences (" + num(e1) + ")");
    check(e2 < 1e-1, "mixed derivative matches finite differences (" + num(e2) + ")");
    Vec3 q = {7, -13, 30};
    double pu = std::nan(""), pv = std::nan("");
    s->project(q, pu, pv);
    s->evaluate(pu, pv, p, su, sv, suu, suv, svv);
    double ou = (p[0] - q[0]) * su[0] + (p[1] - q[1]) * su[1] + (p[2] - q[2]) * su[2];
    double ov = (p[0] - q[0]) * sv[0] + (p[1] - q[1]) * sv[1] + (p[2] - q[2]) * sv[2];
    check(std::fabs(ou) < 1e-8 && std::fabs(ov) < 1e-8, "foot point is orthogonal");
}

void testPointOnSurface(const std::string& name, std::shared_ptr<NurbsSurface> surface)
{
    std::printf("Point on surface: %s\n", name.c_str());
    auto joint = CREATE<ASMTTangentJoint>::With();
    joint->setPointOnSurface(toSurface(*surface));
    Vec3 start = {-12, 9, 25};
    auto model = buildModel(joint, start, Rot::aboutZ(0.3), {0, 0, -4}, Rot());
    if (!solveOk(model)) {
        return;
    }
    double d = std::fabs(signedDistanceToSurface(*surface, model.globalJ({0, 0, 0})));
    check(d < 1e-6, "point on surface after solve (distance " + num(d) + ")");
    double maxD = 0;
    for (int k = 1; k <= 8; k++) {
        try {
            model.dragTo(add(model.moverPos(), {3, -2, 0}));
        }
        catch (const std::exception& e) {
            check(false, std::string("drag threw: ") + e.what());
            return;
        }
        maxD = std::max(maxD, std::fabs(signedDistanceToSurface(*surface, model.globalJ({0, 0, 0}))));
    }
    check(maxD < 1e-6, "point on surface after every drag step (max " + num(maxD) + ")");
}

void testCurveOnSurface(const std::string& name, std::shared_ptr<NurbsSurface> surface,
                        const Vec3& start, const Rot& moverRot = Rot(),
                        const Vec3& markerJPos = {0, 0, 0}, const Rot& markerJRot = Rot::aboutX(M_PI / 2))
{
    std::printf("Curve on surface: %s\n", name.c_str());
    auto wheel = circle(5.0);
    auto joint = CREATE<ASMTTangentJoint>::With();
    joint->setCurveOnSurface(toSurface(*surface), toCurve(*wheel));
    // Wheel in the xz plane of the mover (marker rotated about x).
    auto model = buildModel(joint, start, moverRot, markerJPos, markerJRot);
    if (!solveOk(model)) {
        return;
    }
    double dMin = minSignedDistanceCurveToSurface(model, *wheel, *surface, start[2] > 0 ? 1.0 : -1.0);
    check(std::fabs(dMin) < 1e-5, "wheel touches the surface (smallest distance " + num(dMin) + ")");
    check(dMin > -1e-5, "wheel does not penetrate the surface");
    double startSide = start[2] > 0 ? 1.0 : -1.0;
    check(startSide * signedDistanceToSurface(*surface, model.globalJ({0, 0, 0})) > 0,
          "wheel stays on the side it started on");
    double worst = 0;
    for (int k = 1; k <= 8; k++) {
        try {
            model.dragTo(add(model.moverPos(), {4, 1, 0}));
        }
        catch (const std::exception& e) {
            check(false, std::string("drag threw: ") + e.what());
            return;
        }
        worst = std::max(
            worst, std::fabs(minSignedDistanceCurveToSurface(model, *wheel, *surface, start[2] > 0 ? 1.0 : -1.0)));
    }
    check(worst < 1e-5, "wheel stays touching while dragged (max " + num(worst) + ")");
}

// The wheel also hangs on a vertical cylindrical guide (second joint): no automatic set-down,
// the contact equation has to converge from the distance on its own.
void testGuidedWheel()
{
    std::printf("Curve on surface: wheel on a vertical guide above a bump\n");
    auto surface = bump();
    auto wheel = circle(5.0);
    auto joint = CREATE<ASMTTangentJoint>::With();
    joint->setCurveOnSurface(toSurface(*surface), toCurve(*wheel));
    Vec3 start = {-12, 8, 30};
    auto model = buildModel(joint, start, Rot(), {0, 0, 0}, Rot::aboutX(M_PI / 2));
    // Guide: z axis through the start point, wheel may slide along and turn about it.
    model.ground->addMarker(makeMarker("GuideI", {start[0], start[1], 0}, Rot()));
    model.mover->addMarker(makeMarker("GuideJ", {0, 0, 0}, Rot()));
    auto guide = CREATE<ASMTCylindricalJoint>::With();
    guide->setName("Guide");
    guide->setMarkerI("/OndselAssembly/Ground/GuideI");
    guide->setMarkerJ("/OndselAssembly/Mover/GuideJ");
    model.assembly->addJoint(guide);
    if (!solveOk(model)) {
        return;
    }
    double dMin = minSignedDistanceCurveToSurface(model, *wheel, *surface, 1.0);
    check(std::fabs(dMin) < 1e-5, "wheel touches the surface (smallest distance " + num(dMin) + ")");
    auto c = model.globalJ({0, 0, 0});
    check(std::hypot(c[0] - start[0], c[1] - start[1]) < 1e-6, "wheel stays on the guide axis");
    check(c[2] > 0.0, "wheel stays above the surface");
}

void testCurveOnCurveCircles()
{
    std::printf("Curve on curve: circle rolling around a circle\n");
    auto big = circle(20.0);
    auto small = circle(5.0);
    auto joint = CREATE<ASMTTangentJoint>::With();
    joint->setCurveOnCurve(toCurve(*big), toCurve(*small));
    auto model = buildModel(joint, {31, 2, 0}, Rot::aboutZ(0.4), {0, 0, 0}, Rot());
    if (!solveOk(model)) {
        return;
    }
    auto c = model.globalJ({0, 0, 0});
    double centerDist = std::hypot(c[0], c[1]);
    check(std::fabs(centerDist - 25.0) < 1e-6, "outer tangency, centre distance " + num(centerDist));
    check(std::fabs(c[2]) < 1e-6, "small circle stays in the plane");
    double worst = 0;
    for (int k = 1; k <= 12; k++) {
        double a = 0.15 * k;
        try {
            model.dragTo({25.0 * std::cos(a) + 1.0, 25.0 * std::sin(a), 0});
        }
        catch (const std::exception& e) {
            check(false, std::string("drag threw: ") + e.what());
            return;
        }
        auto ck = model.globalJ({0, 0, 0});
        worst = std::max(worst, std::fabs(std::hypot(ck[0], ck[1]) - 25.0));
    }
    check(worst < 1e-6, "stays tangent while dragged around (max error " + num(worst) + ")");
}

void testCurveOnCurveWave()
{
    std::printf("Curve on curve: circle on a wave\n");
    auto track = wave();
    auto small = circle(3.0);
    auto joint = CREATE<ASMTTangentJoint>::With();
    joint->setCurveOnCurve(toCurve(*track), toCurve(*small));
    auto model = buildModel(joint, {-5, 20, 0}, Rot(), {0, 0, 0}, Rot());
    if (!solveOk(model)) {
        return;
    }
    auto c = model.globalJ({0, 0, 0});
    double d = distanceToCurve(*track, c);
    check(std::fabs(d - 3.0) < 1e-5, "circle centre is one radius from the wave (" + num(d) + ")");
    check(c[1] > 3.0, "circle stays above the wave");
}

void roundTrip(const std::string& what, const std::shared_ptr<ASMTTangentJoint>& joint)
{
    // Only the joint itself: OndselSolver's reader for whole files fails on assembly-level
    // markers (independent of this joint, see test_point_on_curve.cpp).
    std::printf("ASMT: %s, store and parse back\n", what.c_str());
    joint->setName("Tangent");
    joint->setMarkerI("/OndselAssembly/Ground/MarkerI");
    joint->setMarkerJ("/OndselAssembly/Mover/MarkerJ");
    std::string file = "tangent_roundtrip.asmt";
    {
        std::ofstream os(file);
        joint->storeOnLevel(os, 3);
    }
    std::vector<std::string> lines;
    {
        std::ifstream is(file);
        std::string line;
        while (std::getline(is, line)) {
            lines.push_back(line);
        }
    }
    std::remove(file.c_str());
    check(!lines.empty() && lines[0] == "\t\t\tTangentJoint", "stored under the name the reader dispatches on");
    if (lines.empty()) {
        return;
    }
    lines.erase(lines.begin());
    auto parsed = CREATE<ASMTTangentJoint>::With();
    try {
        parsed->parseASMT(lines);
    }
    catch (const std::exception& e) {
        check(false, std::string("parse threw: ") + e.what());
        return;
    }
    auto sameCurve = [](const ASMTTangentJoint::Curve& a, const ASMTTangentJoint::Curve& b) {
        return a.degree == b.degree && a.knots == b.knots && a.weights == b.weights
            && a.poles == b.poles && a.closed == b.closed;
    };
    const auto& s1 = parsed->surfaceI;
    const auto& s2 = joint->surfaceI;
    bool sameSurface = s1.degreeU == s2.degreeU && s1.degreeV == s2.degreeV && s1.nPolesU == s2.nPolesU
        && s1.nPolesV == s2.nPolesV && s1.knotsU == s2.knotsU && s1.knotsV == s2.knotsV
        && s1.weights == s2.weights && s1.poles == s2.poles && s1.closedU == s2.closedU
        && s1.closedV == s2.closedV;
    bool same = parsed->kind == joint->kind && sameSurface && sameCurve(parsed->curveI, joint->curveI)
        && sameCurve(parsed->curveJ, joint->curveJ) && parsed->markerI == joint->markerI
        && parsed->markerJ == joint->markerJ;
    check(same, "kind, geometry and markers survive bit-exact");
}

void testAsmtRoundTrip()
{
    auto j1 = CREATE<ASMTTangentJoint>::With();
    j1->setPointOnSurface(toSurface(*bump()));
    roundTrip("point on surface", j1);
    auto j2 = CREATE<ASMTTangentJoint>::With();
    j2->setCurveOnSurface(toSurface(*bump()), toCurve(*circle(5.0)));
    roundTrip("curve on surface", j2);
    auto j3 = CREATE<ASMTTangentJoint>::With();
    j3->setCurveOnCurve(toCurve(*wave()), toCurve(*circle(3.0)));
    roundTrip("curve on curve", j3);
}

}  // namespace

int main()
{
    testSurfaceEvaluation();
    testPointOnSurface("flat square", flatSquare());
    testPointOnSurface("bicubic bump", bump());
    testCurveOnSurface("wheel above a flat square", flatSquare(), {3, -4, 20});
    testCurveOnSurface("wheel below a flat square", flatSquare(), {3, -4, -20});
    testCurveOnSurface("wheel above a bump", bump(), {-20, -15, 30});
    testCurveOnSurface("tilted wheel on an offset marker above a bump", bump(), {-15, 10, 30},
                       Rot::aboutX(70 * M_PI / 180), {1, -2, 0.5}, Rot::aboutX(30 * M_PI / 180));
    testGuidedWheel();
    testCurveOnCurveCircles();
    testCurveOnCurveWave();
    testAsmtRoundTrip();
    std::printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
