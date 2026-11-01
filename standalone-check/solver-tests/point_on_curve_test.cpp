// Solver-only test for the point-on-curve joint prototype (OndselSolver PointOnCurveJoint, foot point
// method). No FreeCAD involved: a fixed base carries a helix, the origin of a free part must end up
// on it from several start positions; the base may be shifted and turned.
//
// Build and run: standalone-check/solver-tests/run_point_on_curve_test.sh
// Prints "RESULT: PASS" or "RESULT: FAIL".
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include <OndselSolver/ASMTAssembly.h>
#include <OndselSolver/ASMTFixedJoint.h>
#include <OndselSolver/ASMTMarker.h>
#include <OndselSolver/ASMTPart.h>
#include <OndselSolver/ASMTPointOnCurveJoint.h>
#include <OndselSolver/ASMTPrincipalMassMarker.h>
#include <OndselSolver/CREATE.h>

using namespace MbD;
using Vec = std::array<double, 3>;

namespace {
constexpr double R = 20.0;   // helix radius
constexpr double H = 5.0;    // rise per radian

Vec helix(double s) { return {R * std::cos(s), R * std::sin(s), H * s}; }
Vec helixD(double s) { return {-R * std::sin(s), R * std::cos(s), H}; }
Vec helixDD(double s) { return {-R * std::cos(s), -R * std::sin(s), 0.0}; }
double dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec sub(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }

// Foot point on the helix: Newton on f(s) = (c(s) - p).c'(s), start from the angle of p.
void helixFoot(const Vec& p, Vec& c, Vec& t)
{
    double s = std::atan2(p[1], p[0]);
    s += 2.0 * M_PI * std::round((p[2] / H - s) / (2.0 * M_PI));
    for (int i = 0; i < 50; i++) {
        Vec d = sub(helix(s), p);
        double f = dot(d, helixD(s));
        double df = dot(helixD(s), helixD(s)) + dot(d, helixDD(s));
        double step = f / df;
        s -= step;
        if (std::abs(step) < 1e-14) {
            break;
        }
    }
    c = helix(s);
    Vec d = helixD(s);
    double len = std::sqrt(dot(d, d));
    t = {d[0] / len, d[1] / len, d[2] / len};
}

struct Rot { double m[3][3]; };
Rot rotZ(double a) { return {{{std::cos(a), -std::sin(a), 0}, {std::sin(a), std::cos(a), 0}, {0, 0, 1}}}; }
Rot rotX(double a) { return {{{1, 0, 0}, {0, std::cos(a), -std::sin(a)}, {0, std::sin(a), std::cos(a)}}}; }
Rot mul(const Rot& a, const Rot& b)
{
    Rot r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}
Vec rotApply(const Rot& a, const Vec& v)
{
    return {dot({a.m[0][0], a.m[0][1], a.m[0][2]}, v), dot({a.m[1][0], a.m[1][1], a.m[1][2]}, v),
            dot({a.m[2][0], a.m[2][1], a.m[2][2]}, v)};
}
Vec rotApplyT(const Rot& a, const Vec& v)
{
    return {a.m[0][0] * v[0] + a.m[1][0] * v[1] + a.m[2][0] * v[2],
            a.m[0][1] * v[0] + a.m[1][1] * v[1] + a.m[2][1] * v[2],
            a.m[0][2] * v[0] + a.m[1][2] * v[1] + a.m[2][2] * v[2]};
}

std::shared_ptr<ASMTMarker> marker(const std::string& name, const Vec& pos, const Rot& r)
{
    auto m = CREATE<ASMTMarker>::With();
    m->setName(name);
    m->setPosition3D(pos[0], pos[1], pos[2]);
    m->setRotationMatrix(r.m[0][0], r.m[0][1], r.m[0][2], r.m[1][0], r.m[1][1], r.m[1][2], r.m[2][0], r.m[2][1], r.m[2][2]);
    return m;
}

std::shared_ptr<ASMTPart> part(const std::string& name, const Vec& pos, const Rot& r)
{
    auto p = CREATE<ASMTPart>::With();
    p->setName(name);
    auto mass = CREATE<ASMTPrincipalMassMarker>::With();
    mass->setMass(1.0);
    mass->setDensity(1.0);
    mass->setMomentOfInertias(1.0, 1.0, 1.0);
    p->setPrincipalMassMarker(mass);
    p->setPosition3D(pos[0], pos[1], pos[2]);
    p->setRotationMatrix(r.m[0][0], r.m[0][1], r.m[0][2], r.m[1][0], r.m[1][1], r.m[1][2], r.m[2][0], r.m[2][1], r.m[2][2]);
    return p;
}

// Returns the distance of the moved point from the helix (in base coordinates), or -1 on failure.
double runCase(const Vec& basePos, const Rot& baseRot, const Vec& start, double& moved)
{
    const Rot ident = rotZ(0.0);
    auto assembly = CREATE<ASMTAssembly>::With();
    assembly->setName("OndselAssembly");
    assembly->addMarker(marker("ground", basePos, baseRot));

    auto base = part("Base", basePos, baseRot);
    base->addMarker(marker("Fix", {0, 0, 0}, ident));
    base->addMarker(marker("CurveFrame", {0, 0, 0}, ident));
    assembly->addPart(base);
    auto mover = part("Mover", start, ident);
    mover->addMarker(marker("Point", {0, 0, 0}, ident));
    assembly->addPart(mover);

    auto fix = CREATE<ASMTFixedJoint>::With();
    fix->setName("Ground");
    fix->setMarkerI("/OndselAssembly/ground");
    fix->setMarkerJ("/OndselAssembly/Base/Fix");
    assembly->addJoint(fix);
    auto onCurve = ASMTPointOnCurveJoint::With();
    onCurve->setName("OnCurve");
    onCurve->setMarkerI("/OndselAssembly/Base/CurveFrame");
    onCurve->setMarkerJ("/OndselAssembly/Mover/Point");
    onCurve->curve = helixFoot;
    assembly->addJoint(onCurve);

    try {
        assembly->runPreDrag();
    }
    catch (const std::exception& e) {
        std::printf("  Ausnahme: %s\n", e.what());
        return -1.0;
    }
    Vec p{};
    mover->getPosition3D(p[0], p[1], p[2]);
    Vec local = rotApplyT(baseRot, sub(p, basePos));
    Vec c{}, t{};
    helixFoot(local, c, t);
    Vec d = sub(local, c);
    Vec m = sub(p, start);
    moved = std::sqrt(dot(m, m));
    return std::sqrt(dot(d, d));
}
}  // namespace

int main()
{
    struct Case { const char* name; Vec basePos; Rot baseRot; Vec start; };
    const Rot ident = rotZ(0.0);
    const Rot turned = mul(rotZ(0.7), rotX(0.4));
    const Case cases[] = {
        {"nah an der Kurve", {0, 0, 0}, ident, {20.5, 0.3, 0.2}},
        {"seitlich weg", {0, 0, 0}, ident, {28.0, 9.0, 4.0}},
        {"zweite Windung", {0, 0, 0}, ident, {18.0, -6.0, 35.0}},
        {"innen nahe Achse", {0, 0, 0}, ident, {6.0, 2.0, 10.0}},
        {"Basis verschoben+gedreht", {100, -40, 15}, turned, {0, 0, 0}},
    };
    bool ok = true;
    for (const auto& c : cases) {
        Vec start = c.start;
        if (c.basePos[0] != 0.0) {  // Start relativ zur Basis: neben dem Kurvenpunkt bei s = 1
            Vec local = helix(1.0);
            local = {local[0] + 3.0, local[1] - 2.0, local[2] + 1.5};
            Vec w = rotApply(c.baseRot, local);
            start = {w[0] + c.basePos[0], w[1] + c.basePos[1], w[2] + c.basePos[2]};
        }
        double moved = 0.0;
        double dist = runCase(c.basePos, c.baseRot, start, moved);
        bool good = dist >= 0.0 && dist < 1e-6;
        ok = ok && good;
        std::printf("%-26s Abstand zur Kurve %.2e mm, bewegt %.3f mm -> %s\n", c.name, dist, moved, good ? "OK" : "FEHLER");
    }
    std::printf(ok ? "RESULT: PASS\n" : "RESULT: FAIL\n");
    return ok ? 0 : 1;
}
