// Unit tests for the pure geometry of the plugin (no game needed): transforms (NiMath) and the ways drawn in
// Blender (Paths). Built as ManualReloadF_tests.exe and run by CI after every build; a failed check fails the build.
#include "NiMath.h"
#include "Paths.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    int g_failed = 0;
    int g_checks = 0;

    void check(bool ok, const char* what, int line)
    {
        ++g_checks;
        if (!ok) {
            ++g_failed;
            std::printf("FAILED (line %d): %s\n", line, what);
        }
    }

#define CHECK(x) check((x), #x, __LINE__)

    constexpr float kPi = 3.14159265f;

    bool same(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
    bool same(const RE::NiPoint3& a, const RE::NiPoint3& b, float eps = 1e-3f) { return mrf::distance(a, b) <= eps; }

    bool same(const RE::NiTransform& a, const RE::NiTransform& b, float eps = 1e-3f)
    {
        for (int i = 0; i < 3; ++i) {
            if (!same(mrf::axisOf(a, i), mrf::axisOf(b, i), eps)) {
                return false;
            }
        }
        return same(a.translate, b.translate, eps) && same(a.scale, b.scale, eps);
    }

    // A pose turned by `angle` about the world axis `axis`, standing at `at`.
    RE::NiTransform pose(const RE::NiPoint3& axis, float angle, const RE::NiPoint3& at, float scale = 1.0f)
    {
        auto t = mrf::identityTransform();
        const auto n = mrf::normalized(axis);
        mrf::setRows(t, mrf::rotateAbout({ 1, 0, 0 }, n, angle), mrf::rotateAbout({ 0, 1, 0 }, n, angle), mrf::rotateAbout({ 0, 0, 1 }, n, angle));
        t.translate = at;
        t.scale = scale;
        return t;
    }

    void vectors()
    {
        CHECK(same(mrf::dot({ 1, 2, 3 }, { 4, 5, 6 }), 32.0f));
        CHECK(same(mrf::cross({ 1, 0, 0 }, { 0, 1, 0 }), { 0, 0, 1 }));
        CHECK(same(mrf::normalized({ 0, 3, 4 }), { 0, 0.6f, 0.8f }));
        CHECK(same(mrf::distance({ 1, 1, 1 }, { 4, 5, 1 }), 5.0f));
        CHECK(same(mrf::approach(0.0f, 1.0f, 0.25f), 0.25f));
        CHECK(same(mrf::approach(0.9f, 1.0f, 0.25f), 1.0f));
        CHECK(same(mrf::approach(1.0f, 0.0f, 0.25f), 0.75f));
    }

    void transforms()
    {
        const auto id = mrf::identityTransform();
        const auto a = pose({ 0.3f, -0.5f, 0.8f }, 0.7f, { 10, -4, 2 }, 1.0f);
        const auto b = pose({ 1, 0.2f, 0 }, -1.3f, { -3, 7, 0.5f }, 1.0f);

        CHECK(same(mrf::compose(id, a), a));
        CHECK(same(mrf::compose(a, id), a));
        CHECK(same(mrf::compose(a, mrf::inverse(a)), id));
        // relative undoes compose: the local pose comes back from the world pose
        CHECK(same(mrf::relative(a, mrf::compose(a, b)), b));
        // a scaled parent
        const auto s = pose({ 0, 0, 1 }, 0.4f, { 1, 2, 3 }, 0.896f);
        CHECK(same(mrf::relative(s, mrf::compose(s, b)), b));
        // rows stay unit and square
        for (int i = 0; i < 3; ++i) {
            CHECK(same(mrf::dot(mrf::axisOf(a, i), mrf::axisOf(a, i)), 1.0f));
            CHECK(same(mrf::dot(mrf::axisOf(a, i), mrf::axisOf(a, (i + 1) % 3)), 0.0f));
        }
        // a direction in a pose's own frame: its own X axis
        CHECK(same(mrf::directionToWorld(a, { 1, 0, 0 }), mrf::axisOf(a, 0)));
        // blend: the ends, and halfway between a pose and itself
        CHECK(same(mrf::blend(a, b, 0.0f), a));
        CHECK(same(mrf::blend(a, b, 1.0f), b));
        CHECK(same(mrf::blend(a, a, 0.5f), a));
    }

    void turns()
    {
        // a turn about Z by 90 degrees takes X to Y and keeps the axis
        CHECK(same(mrf::rotateAbout({ 1, 0, 0 }, { 0, 0, 1 }, kPi / 2), { 0, 1, 0 }));
        CHECK(same(mrf::rotateAbout({ 0, 0, 5 }, { 0, 0, 1 }, 1.0f), { 0, 0, 5 }));
        // there and back
        const RE::NiPoint3 v{ 2, -1, 3 }, axis = mrf::normalized({ 1, 1, 0 });
        CHECK(same(mrf::rotateAbout(mrf::rotateAbout(v, axis, 0.8f), axis, -0.8f), v));
        // a part turned about a pivot keeps its distance to the pivot
        const RE::NiPoint3 pivot{ 0, 10.794f, 1.801f };
        auto rest = mrf::identityTransform();
        rest.translate = { 0, 24, 3.5f };
        const auto opened = mrf::turnedPose(rest, { 1, 0, 0 }, pivot, 55.0f * kPi / 180.0f, {});
        CHECK(same(mrf::distance(opened.translate, pivot), mrf::distance(rest.translate, pivot)));
    }

    // points on a circle about `pivot` (axis X), from `from` to `to` radians, then a straight run
    std::vector<RE::NiPoint3> arcThenLine(const RE::NiPoint3& pivot, float radius, float from, float to, int arcPoints, int linePoints)
    {
        std::vector<RE::NiPoint3> pts;
        for (int i = 0; i < arcPoints; ++i) {
            const float a = from + (to - from) * static_cast<float>(i) / static_cast<float>(arcPoints - 1);
            pts.push_back(pivot + RE::NiPoint3{ 0, radius * std::cos(a), radius * std::sin(a) });
        }
        const auto last = pts.back();
        for (int i = 1; i <= linePoints; ++i) {
            pts.push_back(last + RE::NiPoint3{ 0, 0, -2.0f * static_cast<float>(i) });
        }
        return pts;
    }

    std::vector<float> shares(const std::vector<RE::NiPoint3>& pts)
    {
        std::vector<float> at(pts.size(), 0.0f);
        float total = 0.0f;
        for (std::size_t i = 1; i < pts.size(); ++i) {
            total += mrf::distance(pts[i], pts[i - 1]);
            at[i] = total;
        }
        for (auto& a : at) {
            a = total > 0.0f ? a / total : 0.0f;
        }
        return at;
    }

    void arcs()
    {
        // the barrels of a break action: a 55 degree turn about a hinge
        const RE::NiPoint3 pivot{ 0, 10.8f, 1.8f };
        const auto pts = arcThenLine(pivot, 13.3f, 0.13f, 0.13f - 55.0f * kPi / 180.0f, 6, 0);
        const auto fit = mrf::fitArc(pts, shares(pts));
        CHECK(fit.arc);
        CHECK(same(fit.angle, 55.0f * kPi / 180.0f, 0.01f));
        CHECK(same(std::fabs(fit.axis.x), 1.0f, 0.01f));
        CHECK(same(fit.pivot, pivot, 0.05f));
        CHECK(same(fit.arcAt, 1.0f));

        // a bolt handle: turns 40 degrees, then slides straight back
        const auto bolt = arcThenLine({ 0, 0, 0 }, 3.0f, 0.0f, 40.0f * kPi / 180.0f, 5, 4);
        const auto boltAt = shares(bolt);
        const auto boltFit = mrf::fitArc(bolt, boltAt);
        CHECK(boltFit.arc);
        CHECK(same(boltFit.angle, 40.0f * kPi / 180.0f, 0.01f));
        CHECK(boltFit.k == 4);
        CHECK(same(boltFit.arcAt, boltAt[4]));

        // a straight way is not a turn
        const std::vector<RE::NiPoint3> line{ { 0, 0, 0 }, { 0, 1, 0 }, { 0, 2, 0 }, { 0, 3, 0 } };
        CHECK(!mrf::fitArc(line, shares(line)).arc);
    }

    void ways()
    {
        const std::vector<RE::NiPoint3> pts{ { 0, 0, 0 }, { 0, 10, 0 }, { 0, 10, 10 } };
        const auto at = shares(pts);
        CHECK(same(mrf::alongWay(pts, at, 0.0f), { 0, 0, 0 }));
        CHECK(same(mrf::alongWay(pts, at, 0.5f), { 0, 10, 0 }));
        CHECK(same(mrf::alongWay(pts, at, 0.75f), { 0, 10, 5 }));
        CHECK(same(mrf::alongWay(pts, at, 1.0f), { 0, 10, 10 }));
        float away = -1.0f;
        CHECK(same(mrf::nearestOnWay(pts, at, { 2, 5, 0 }, &away), 0.25f));
        CHECK(same(away, 2.0f));
        CHECK(same(mrf::nearestOnWay(pts, at, { 0, -5, 0 }, nullptr), 0.0f));
    }
}

int main()
{
    vectors();
    transforms();
    turns();
    arcs();
    ways();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
