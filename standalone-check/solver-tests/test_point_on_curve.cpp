// Standalone tests for OndselSolver's PointOnCurveJoint (Tangent joint "vertex on spline").
//
// Builds small ASMT assemblies directly (the same API FreeCAD uses): a grounded part carrying a
// curve and a free part carrying the point. Checks after solve (runPreDrag) and after drag steps
// (runDragStep) that the point lies on the curve, and that it stops at the ends of an open
// curve. Build and run: ./run-solver-tests.sh
//
// The distance check is independent of the joint: the curve is sampled densely in global
// coordinates and the closest sample is refined by a golden-section search.

#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ASMTAssembly.h"
#include "ASMTFixedJoint.h"
#include "ASMTMarker.h"
#include "ASMTPart.h"
#include "ASMTPointOnCurveJoint.h"
#include "ASMTPrincipalMassMarker.h"
#include "CREATE.h"
#include "NurbsCurve.h"

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
    // Row-major rotation matrix.
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    static Rot aboutZ(double a)
    {
        Rot r;
        r.m[0][0] = std::cos(a);
        r.m[0][1] = -std::sin(a);
        r.m[1][0] = std::sin(a);
        r.m[1][1] = std::cos(a);
        return r;
    }
    static Rot aboutX(double a)
    {
        Rot r;
        r.m[1][1] = std::cos(a);
        r.m[1][2] = -std::sin(a);
        r.m[2][1] = std::sin(a);
        r.m[2][2] = std::cos(a);
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
    item->setRotationMatrix(
        rot.m[0][0], rot.m[0][1], rot.m[0][2],
        rot.m[1][0], rot.m[1][1], rot.m[1][2],
        rot.m[2][0], rot.m[2][1], rot.m[2][2]
    );
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

// Global curve point: curve is in marker coordinates, marker on the grounded part.
struct CurveSetup
{
    std::shared_ptr<NurbsCurve> curve;
    Vec3 groundPos = {0, 0, 0};
    Rot groundRot;
    Vec3 markerPos = {0, 0, 0};
    Rot markerRot;
    Vec3 global(double u) const
    {
        auto c = curve->value(u);
        auto inPart = add(markerPos, markerRot.apply(c));
        return add(groundPos, groundRot.apply(inPart));
    }
    // Independent distance from p to the curve.
    double distanceTo(const Vec3& p, double* uOut = nullptr) const
    {
        double a = curve->firstParameter(), b = curve->lastParameter();
        const int n = 20000;
        double best = a, bestD = 1e300;
        for (int i = 0; i <= n; i++) {
            double u = a + (b - a) * i / n;
            double d = dist(global(u), p);
            if (d < bestD) {
                bestD = d;
                best = u;
            }
        }
        double lo = std::max(a, best - (b - a) / n), hi = std::min(b, best + (b - a) / n);
        const double g = (std::sqrt(5.0) - 1) / 2;
        for (int i = 0; i < 200; i++) {
            double u1 = hi - g * (hi - lo), u2 = lo + g * (hi - lo);
            if (dist(global(u1), p) < dist(global(u2), p)) {
                hi = u2;
            }
            else {
                lo = u1;
            }
        }
        if (uOut) {
            *uOut = 0.5 * (lo + hi);
        }
        return dist(global(0.5 * (lo + hi)), p);
    }
};

struct Model
{
    std::shared_ptr<ASMTAssembly> assembly;
    std::shared_ptr<ASMTPart> ground, slider;
    Vec3 pointLocal;  // point marker on the slider (no rotation)

    Vec3 pointGlobal() const
    {
        double x, y, z, q0, q1, q2, q3;
        slider->getPosition3D(x, y, z);
        slider->getQuarternions(q3, q0, q1, q2);  // same call order as FreeCAD
        // Rotation from unit quaternion (w = q3, vector = q0..q2).
        double w = q3, a = q0, b = q1, c = q2;
        double r[3][3] = {
            {1 - 2 * (b * b + c * c), 2 * (a * b - c * w), 2 * (a * c + b * w)},
            {2 * (a * b + c * w), 1 - 2 * (a * a + c * c), 2 * (b * c - a * w)},
            {2 * (a * c - b * w), 2 * (b * c + a * w), 1 - 2 * (a * a + b * b)},
        };
        const auto& p = pointLocal;
        return {
            x + r[0][0] * p[0] + r[0][1] * p[1] + r[0][2] * p[2],
            y + r[1][0] * p[0] + r[1][1] * p[1] + r[1][2] * p[2],
            z + r[2][0] * p[0] + r[2][1] * p[1] + r[2][2] * p[2],
        };
    }

    void solve()
    {
        assembly->runPreDrag();
    }

    void dragTo(const Vec3& sliderPos)
    {
        slider->updateMbDFromPosition3D(sliderPos[0], sliderPos[1], sliderPos[2]);
        auto parts = std::make_shared<std::vector<std::shared_ptr<ASMTPart>>>();
        parts->push_back(slider);
        assembly->runDragStep(parts);
    }
};

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

Model buildModel(const CurveSetup& setup, const Vec3& sliderPos, const Vec3& pointLocal)
{
    Model model;
    model.pointLocal = pointLocal;
    model.assembly = CREATE<ASMTAssembly>::With();
    model.assembly->setName("OndselAssembly");

    model.ground = makePart("Ground", setup.groundPos, setup.groundRot);
    model.assembly->addPart(model.ground);
    model.slider = makePart("Slider", sliderPos, Rot());
    model.assembly->addPart(model.slider);

    // Grounding as FreeCAD does it: fixed joint between an assembly marker and the part.
    auto fixAsm = makeMarker("marker-Ground", setup.groundPos, setup.groundRot);
    model.assembly->addMarker(fixAsm);
    auto fixPart = makeMarker("FixingMarker", {0, 0, 0}, Rot());
    model.ground->addMarker(fixPart);
    auto fixed = CREATE<ASMTFixedJoint>::With();
    fixed->setName("GroundedJoint");
    fixed->setMarkerI("/OndselAssembly/marker-Ground");
    fixed->setMarkerJ("/OndselAssembly/Ground/FixingMarker");
    model.assembly->addJoint(fixed);

    auto curveMarker = makeMarker("CurveMarker", setup.markerPos, setup.markerRot);
    model.ground->addMarker(curveMarker);
    auto pointMarker = makeMarker("PointMarker", pointLocal, Rot());
    model.slider->addMarker(pointMarker);

    auto joint = CREATE<ASMTPointOnCurveJoint>::With();
    joint->setName("Tangent");
    joint->setMarkerI("/OndselAssembly/Ground/CurveMarker");
    joint->setMarkerJ("/OndselAssembly/Slider/PointMarker");
    const auto& c = *setup.curve;
    std::vector<double> poles;
    for (auto& p : c.poles) {
        poles.insert(poles.end(), p.begin(), p.end());
    }
    joint->setCurve(c.degree, c.knots, c.weights, poles, c.closed);
    model.assembly->addJoint(joint);
    return model;
}

// Open cubic B-spline, a wave in the xy plane of the marker.
std::shared_ptr<NurbsCurve> waveSpline()
{
    std::vector<Vec3> poles = {{0, 0, 0}, {10, 15, 0}, {20, -15, 0}, {30, 10, 0}, {40, 0, 0}};
    std::vector<double> knots = {0, 0, 0, 0, 0.5, 1, 1, 1, 1};
    return std::make_shared<NurbsCurve>(3, knots, std::vector<double>(), poles, false);
}

// Non-planar open spline.
std::shared_ptr<NurbsCurve> helixSpline()
{
    std::vector<Vec3> poles = {{0, 0, 0}, {10, 10, 5}, {0, 20, 10}, {-10, 10, 15}, {0, 0, 20}, {10, 10, 25}};
    std::vector<double> knots = {0, 0, 0, 0, 1, 2, 3, 3, 3, 3};
    return std::make_shared<NurbsCurve>(3, knots, std::vector<double>(), poles, false);
}

// Exact full circle of radius r as a closed rational quadratic B-spline (9 poles).
std::shared_ptr<NurbsCurve> circle(double r)
{
    double w = std::sqrt(0.5);
    std::vector<Vec3> poles = {{r, 0, 0}, {r, r, 0}, {0, r, 0}, {-r, r, 0}, {-r, 0, 0},
                               {-r, -r, 0}, {0, -r, 0}, {r, -r, 0}, {r, 0, 0}};
    std::vector<double> weights = {1, w, 1, w, 1, w, 1, w, 1};
    std::vector<double> knots = {0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
    return std::make_shared<NurbsCurve>(2, knots, weights, poles, true);
}

void testCurveEvaluation()
{
    std::printf("Curve evaluation\n");
    auto c = circle(7.0);
    double maxErr = 0;
    for (int i = 0; i <= 100; i++) {
        double u = 4.0 * i / 100;
        auto p = c->value(u);
        maxErr = std::max(maxErr, std::fabs(std::hypot(p[0], p[1]) - 7.0));
    }
    check(maxErr < 1e-12, "rational circle has constant radius (err " + num(maxErr) + ")");

    // Derivative against finite differences.
    auto s = helixSpline();
    Vec3 c0, d1, d2;
    double u = 1.37, h = 1e-6;
    s->evaluate(u, c0, d1, d2);
    auto cp = s->value(u + h), cm = s->value(u - h);
    double errD1 = 0, errD2 = 0;
    for (int i = 0; i < 3; i++) {
        errD1 = std::max(errD1, std::fabs((cp[i] - cm[i]) / (2 * h) - d1[i]));
        errD2 = std::max(errD2, std::fabs((cp[i] - 2 * c0[i] + cm[i]) / (h * h) - d2[i]));
    }
    check(errD1 < 1e-5, "first derivative matches finite differences");
    check(errD2 < 1e-2, "second derivative matches finite differences");

    Vec3 p = {12, 3, 4};
    double uFoot = s->project(p);
    Vec3 cf, d1f, d2f;
    s->evaluate(uFoot, cf, d1f, d2f);
    double orth = (cf[0] - p[0]) * d1f[0] + (cf[1] - p[1]) * d1f[1] + (cf[2] - p[2]) * d1f[2];
    check(std::fabs(orth) < 1e-9, "foot point is orthogonal");
}

void testSolve(const std::string& name, const CurveSetup& setup, const Vec3& sliderPos, const Vec3& pointLocal)
{
    std::printf("%s\n", name.c_str());
    auto model = buildModel(setup, sliderPos, pointLocal);
    try {
        model.solve();
    }
    catch (const std::exception& e) {
        check(false, std::string("solve threw: ") + e.what());
        return;
    }
    double d = setup.distanceTo(model.pointGlobal());
    check(d < 1e-6, "point on curve after solve (distance " + num(d) + ")");
}

void testDrag(const std::string& name, const CurveSetup& setup, const Vec3& start,
              const std::vector<Vec3>& targets, double* endDistance = nullptr, bool atEnd = false)
{
    std::printf("%s\n", name.c_str());
    auto model = buildModel(setup, start, {0, 0, 0});
    try {
        model.solve();
        double maxD = 0;
        for (auto& t : targets) {
            model.dragTo(t);
            maxD = std::max(maxD, setup.distanceTo(model.pointGlobal()));
        }
        check(maxD < 1e-6, "point on curve after every drag step (max distance " + num(maxD) + ")");
        if (atEnd) {
            double dEnd = dist(model.pointGlobal(), setup.global(setup.curve->lastParameter()));
            if (endDistance) {
                *endDistance = dEnd;
            }
            check(dEnd < 1e-5, "point stops at the curve end (distance to end " + num(dEnd) + ")");
        }
        else {
            // Followed the mouse: the final point is near the projection of the target.
            double uT;
            double dT = setup.distanceTo(targets.back(), &uT);
            (void)dT;
            double follow = dist(model.pointGlobal(), setup.global(uT));
            check(follow < 1.0, "point follows the drag target along the curve (" + num(follow) + ")");
        }
    }
    catch (const std::exception& e) {
        check(false, std::string("threw: ") + e.what());
    }
}

void testAsmtRoundTrip(const CurveSetup& setup)
{
    // Only the joint itself: OndselSolver's reader for whole files currently fails on
    // assembly-level markers (independent of this joint), so the joint's own store/parse pair
    // is checked directly.
    std::printf("ASMT: store joint, parse it back\n");
    auto model = buildModel(setup, {12, 9, 4}, {0, 0, 0});
    std::shared_ptr<ASMTPointOnCurveJoint> joint;
    for (auto& j : *model.assembly->joints) {
        if (auto pc = std::dynamic_pointer_cast<ASMTPointOnCurveJoint>(j)) {
            joint = pc;
        }
    }
    std::string file = "point_on_curve_roundtrip.asmt";
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
    check(!lines.empty() && lines[0] == "\t\t\tPointOnCurveJoint", "stored under the name the reader dispatches on");
    lines.erase(lines.begin());
    auto readBack = CREATE<ASMTPointOnCurveJoint>::With();
    try {
        readBack->parseASMT(lines);
    }
    catch (const std::exception& e) {
        check(false, std::string("parse threw: ") + e.what());
        return;
    }
    bool same = readBack->degree == joint->degree && readBack->closed == joint->closed
        && readBack->knots == joint->knots && readBack->weights == joint->weights
        && readBack->poles == joint->poles && readBack->markerI == joint->markerI
        && readBack->markerJ == joint->markerJ;
    check(same, "curve data and markers survive bit-exact");
}

}  // namespace

int main()
{
    testCurveEvaluation();

    CurveSetup wave;
    wave.curve = waveSpline();
    testSolve("Solve: planar spline, point starts off the curve", wave, {12, 9, 4}, {0, 0, 0});
    testSolve("Solve: point marker offset on the slider", wave, {5, -3, 2}, {1, 2, 3});

    CurveSetup moved = wave;
    moved.groundPos = {100, -20, 5};
    moved.groundRot = Rot::aboutZ(0.7);
    moved.markerPos = {3, 4, 5};
    moved.markerRot = Rot::aboutX(1.1);
    testSolve("Solve: grounded part and curve marker moved/rotated", moved, {110, -5, 10}, {0, 0, 0});

    CurveSetup helix;
    helix.curve = helixSpline();
    testSolve("Solve: non-planar spline", helix, {3, 8, 9}, {0, 0, 0});

    std::vector<Vec3> along;
    for (int i = 1; i <= 20; i++) {
        double x = 5 + 30.0 * i / 20;
        along.push_back({x, 2.0, 0});
    }
    testDrag("Drag: along the planar spline", wave, {5, 2, 0}, along);

    std::vector<Vec3> beyond;
    for (int i = 1; i <= 30; i++) {
        beyond.push_back({30 + 1.0 * i, 0, 0});
    }
    testDrag("Drag: past the open end", wave, {30, 0, 0}, beyond, nullptr, true);

    CurveSetup ring;
    ring.curve = circle(10.0);
    std::vector<Vec3> around;
    for (int i = 1; i <= 40; i++) {
        double a = 2 * M_PI * i / 40 + 0.1;
        around.push_back({12 * std::cos(a), 12 * std::sin(a), 0});
    }
    testDrag("Drag: once around a closed circle (across the seam)", ring, {11, 1, 0}, around);

    testAsmtRoundTrip(wave);

    std::printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
