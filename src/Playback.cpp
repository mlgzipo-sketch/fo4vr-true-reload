#include "Playback.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "Runtime.h"
#include "Angles.h"
#include "GripHook.h"
#include "Gun.h"
#include "Hands.h"
#include "Log.h"
#include "Magazine.h"
#include "NiMath.h"
#include "Nodes.h"
#include "Objects.h"
#include "Palm.h"
#include "Paths.h"
#include "Rail.h"
#include "Recorder.h"
#include "RockLink.h"
#include "SafeMemory.h"
#include "ScriptBridge.h"
#include "Settings.h"
#include "State.h"
#include "Zones.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    PlayedAnim g_play;

    bool loadPlayed(std::uint32_t weapon) noexcept
    {
        auto& p = g_play;
        if (g_playStale) {
            g_playStale = false;
            p.weapon = 0;
        }
        if (p.weapon == weapon) {
            return p.loaded;
        }
        p = PlayedAnim{};
        p.weapon = weapon;
        char path[160];
        std::snprintf(path, sizeof(path), "Data/F4SE/Plugins/ManualReloadF_anim/%08X.txt", weapon);
        std::ifstream file(path);
        if (!file) {
            return false;
        }
        struct HingeLine { float angle; RE::NiPoint3 axis, pivot; float from; };
        std::unordered_map<std::string, HingeLine> hinges;
        std::unordered_map<std::string, int> index;
        std::string line;
        float lastTime = -1.0f;
        while (std::getline(file, line)) {
            std::vector<std::string> words;
            {
                std::size_t at = 0;
                while (at < line.size()) {
                    while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) ++at;
                    const auto start = at;
                    while (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != '\r') ++at;
                    if (at > start) words.emplace_back(line.substr(start, at - start));
                }
            }
            const auto num = [&](std::size_t i) { return i < words.size() ? std::strtof(words[i].c_str(), nullptr) : 0.0f; };
            const auto after = [&](const char* key) -> std::size_t {
                for (std::size_t i = 0; i < words.size(); ++i) if (words[i] == key) return i + 1;
                return words.size();
            };
            if (words.size() > 12 && words[0] == "hinge") {
                HingeLine h{};
                h.angle = num(after("angle"));
                const auto ax = after("axis");
                h.axis = { num(ax), num(ax + 1), num(ax + 2) };
                const auto pv = after("pivot");
                h.pivot = { num(pv), num(pv + 1), num(pv + 2) };
                h.from = num(after("from"));
                hinges[words[1]] = h;
                continue;
            }
            if (words.size() < 15 || words[0] != "f") {
                continue;
            }
            const float t = num(1);
            const std::string name = words[2];
            float v[12]{};
            for (int k = 0; k < 12; ++k) v[k] = num(3 + k);
            if (t != lastTime) {
                lastTime = t;
                p.times.push_back(t);
                p.frames.emplace_back(p.names.size(), identityTransform());
            }
            auto found = index.find(name);
            if (found == index.end()) {
                if (p.frames.size() > 1) {
                    continue;   // every node is in the first frame
                }
                found = index.emplace(name, static_cast<int>(p.names.size())).first;
                p.names.emplace_back(name);
                p.frames.back().push_back(identityTransform());
            }
            RE::NiTransform tr = identityTransform();
            setRows(tr, { v[3], v[4], v[5] }, { v[6], v[7], v[8] }, { v[9], v[10], v[11] });
            tr.translate = { v[0], v[1], v[2] };
            p.frames.back()[found->second] = tr;
        }
        p.parsed = p.frames.size() >= 10;
        p.index = index;
        if (const auto boltAt = index.find("WeaponBolt"); p.parsed && boltAt != index.end()) {
            const int n = boltAt->second;
            const auto restCols = colsOf(p.frames[0][n]);
            std::vector<float> away(p.frames.size(), 0.0f);
            std::size_t peak = 0;
            for (std::size_t f = 1; f < p.frames.size(); ++f) {
                const auto d = p.frames[f][n].translate - p.frames[0][n].translate;
                away[f] = std::sqrt(dot(d, d)) + 3.0f * angleOf(mulAB(colsOf(p.frames[f][n]), transposed(restCols)));
                if (away[f] > away[peak]) peak = f;
            }
            if (away[peak] > 0.5f) {
                std::size_t from = peak;
                while (from > 0 && away[from - 1] > away[peak] * 0.02f) --from;
                from = from > 0 ? from - 1 : 0;
                p.bolt = n;
                p.boltFrom = from;
                p.boltTo = peak;
                for (std::size_t f = from; f <= peak; ++f) {
                    p.boltTurn = std::max(p.boltTurn, angleOf(mulAB(colsOf(p.frames[f][n]), transposed(restCols))));
                }
                p.boltKnown = true;
                note("PLAY: bolt / slide path of %08X: frames %zu..%zu (%.2f..%.2f s), turns %.1f deg on the way%s", weapon, from, peak, p.times[from],
                    p.times[peak], p.boltTurn * 57.29578f, p.boltTurn > 0.35f ? ": a bolt action, worked by hand after every shot" : "");
            }
        }
        const auto main = hinges.find("WeaponMagazine");
        const auto mainIndex = index.find("WeaponMagazine");
        if (p.frames.size() < 10 || main == hinges.end() || mainIndex == index.end()) {
            detail("PLAY: the recording of %08X has no hinge on WeaponMagazine (%zu frames)", weapon, p.frames.size());
            return false;
        }
        p.openAngle = main->second.angle * 0.01745329f;
        p.axis = normalized(main->second.axis);
        p.pivot = main->second.pivot;
        const auto restCols = colsOf(p.frames[0][mainIndex->second]);
        p.openTime = p.times.back();
        for (std::size_t f = 1; f < p.frames.size(); ++f) {
            if (angleOf(mulAB(colsOf(p.frames[f][mainIndex->second]), transposed(restCols))) >= p.openAngle * 0.97f) {
                p.openTime = p.times[f];
                break;
            }
        }
        // posed: everything that is on the way by the time it is open, except parts that swing on their own axis
        // below the opening part (a speedloader, shells thrown out)
        for (std::size_t n = 0; n < p.names.size(); ++n) {
            const auto& name = p.names[n];
            const auto h = hinges.find(name);
            bool moving = true;
            if (h != hinges.end()) {
                const bool sameHinge = std::fabs(dot(normalized(h->second.axis), p.axis)) > 0.97f && std::fabs(h->second.angle - main->second.angle) < 3.0f;
                const bool early = h->second.from <= p.openTime + 0.01f;
                const bool own = name.rfind("WeaponMagazineChild", 0) == 0 || name.find("Shell") != std::string::npos ||
                                 name.find("SpeedLoader") != std::string::npos || name.find("Bullet") != std::string::npos ||
                                 name.find("Casing") != std::string::npos;
                moving = early && (sameHinge || !own);
            } else {
                moving = false;   // slides are left to the game (triggers, sights)
            }
            if (moving) {
                p.posed.push_back(static_cast<int>(n));
            }
        }
        p.loaded = true;
        std::string list;
        for (const auto n : p.posed) {
            list += p.names[n] + " ";
        }
        note("PLAY: recording of %08X, %zu frames; opens %.1f deg about (%.2f, %.2f, %.2f) at (%.2f, %.2f, %.2f) by %.2f s; posed: %s", weapon, p.frames.size(),
            main->second.angle, p.axis.x, p.axis.y, p.axis.z, p.pivot.x, p.pivot.y, p.pivot.z, p.openTime, list.c_str());
        return true;
    }

    RE::NiTransform playedAt(const PlayedAnim& p, int node, float time) noexcept
    {
        std::size_t hi = 1;
        while (hi < p.times.size() - 1 && p.times[hi] < time) {
            ++hi;
        }
        const std::size_t lo = hi - 1;
        const float span = p.times[hi] - p.times[lo];
        const float k = span > 1e-5f ? std::clamp((time - p.times[lo]) / span, 0.0f, 1.0f) : 0.0f;
        const auto& a = p.frames[lo][node];
        const auto& b = p.frames[hi][node];
        RE::NiTransform out = identityTransform();
        out.translate = a.translate + (b.translate - a.translate) * k;
        const auto x = normalized(axisOf(a, 0) + (axisOf(b, 0) - axisOf(a, 0)) * k);
        auto y = axisOf(a, 1) + (axisOf(b, 1) - axisOf(a, 1)) * k;
        y = normalized(y - x * dot(y, x));
        setRows(out, x, y, cross(x, y));
        return out;
    }

    // Poses the opening part as the recording has it at `amount` (0 closed .. 1 open) of its opening move.

    bool posePlayed(RE::NiAVObject* root, float amount) noexcept
    {
        auto& p = g_play;
        if (!p.loaded) {
            return false;
        }
        if (p.rootSeen != root || p.nodes.size() != p.posed.size()) {
            p.rootSeen = root;
            p.nodes.clear();
            for (const auto n : p.posed) {
                p.nodes.push_back(findNamed(root, p.names[n].c_str()));
            }
        }
        const float time = std::clamp(amount, 0.0f, 1.0f) * p.openTime;
        std::unordered_map<RE::NiAVObject*, RE::NiTransform> placed;
        std::vector<RE::NiAVObject*> tops;
        for (std::size_t i = 0; i < p.posed.size(); ++i) {
            auto* node = p.nodes[i];
            if (!looksLikeObject(node) || !looksLikeObject(node->parent)) {
                continue;
            }
            auto recorded = playedAt(p, p.posed[i], time);
            recorded.scale = root->world.scale > 0.0f ? node->world.scale / root->world.scale : 1.0f;
            const auto world = compose(root->world, recorded);
            const auto up = placed.find(node->parent);
            const bool parentPosed = up != placed.end();
            node->local = relative(parentPosed ? up->second : node->parent->world, world);
            placed[node] = world;
            if (!parentPosed) {
                tops.push_back(node);
            }
        }
        for (auto* node : tops) {
            refreshSubtree(node, RE::NiPoint3{});
        }
        return true;
    }

    // The angle of a point about the hinge (gun root frame), for turning the opening part by hand.

    float hingeAngle(const RE::NiAVObject* root, const RE::NiPoint3& world) noexcept
    {
        const auto& p = g_play;
        const auto at = relative(root->world, pointTransform(world)).translate - p.pivot;
        const auto flat = at - p.axis * dot(at, p.axis);
        auto u = std::fabs(p.axis.z) < 0.9f ? RE::NiPoint3{ 0.0f, 0.0f, 1.0f } : RE::NiPoint3{ 1.0f, 0.0f, 0.0f };
        u = normalized(u - p.axis * dot(u, p.axis));
        const auto v = cross(p.axis, u);
        return std::atan2(dot(flat, v), dot(flat, u));
    }

    // The bolt as the recording has it at `s` (0 rest .. 1 furthest back), in the gun root's frame.

    RE::NiTransform boltAt(float s) noexcept
    {
        const auto& p = g_play;
        const float time = p.times[p.boltFrom] + std::clamp(s, 0.0f, 1.0f) * (p.times[p.boltTo] - p.times[p.boltFrom]);
        if (s <= 0.0f) {
            return p.frames[0][p.bolt];
        }
        return playedAt(p, p.bolt, time);
    }

    BoltArc g_arc;

    void buildArc() noexcept
    {
        auto& a = g_arc;
        const auto& p = g_play;
        if (a.weapon == p.weapon && a.frames == p.frames.size() && !a.s.empty()) {
            return;
        }
        a = BoltArc{};
        a.weapon = p.weapon;
        a.frames = p.frames.size();
        constexpr int kSamples = 240;
        const RE::NiPoint3 probes[3]{ { 0.0f, 0.0f, 0.0f }, { 3.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 3.0f } };
        std::vector<std::array<RE::NiPoint3, 3>> at(kSamples + 1);
        for (int i = 0; i <= kSamples; ++i) {
            const auto t = boltAt(static_cast<float>(i) / kSamples);
            for (int k = 0; k < 3; ++k) {
                at[i][k] = compose(t, pointTransform(probes[k])).translate;
            }
        }
        // where the slide is furthest from rest (by its origin)
        int last = kSamples;
        if (p.boltTurn <= 0.35f) {
            float farthest = 0.0f;
            for (int i = 0; i <= kSamples; ++i) {
                farthest = std::max(farthest, distance(at[i][0], at[0][0]));
            }
            for (int i = 0; i <= kSamples; ++i) {
                if (distance(at[i][0], at[0][0]) >= farthest * 0.98f) {
                    last = std::max(i, 1);
                    break;
                }
            }
        }
        std::vector<float> length(last + 1, 0.0f);
        for (int i = 1; i <= last; ++i) {
            float step = 0.0f;
            for (int k = 0; k < 3; ++k) {
                step = std::max(step, distance(at[i][k], at[i - 1][k]));
            }
            length[i] = length[i - 1] + step;
        }
        const float total = std::max(length[last], 0.001f);
        for (int i = 0; i <= last; ++i) {
            a.s.push_back(static_cast<float>(i) / kSamples);
            a.u.push_back(length[i] / total);
        }
        note("CORE2: the slide's way is %.1f long; it is furthest back at %.0f%% of the recorded time", total, 100.0f * last / kSamples);
    }

    RE::NiTransform boltArc(float u) noexcept
    {
        buildArc();
        const auto& a = g_arc;
        u = std::clamp(u, 0.0f, 1.0f);
        if (a.u.size() < 2) {
            return boltAt(u);
        }
        const auto it = std::lower_bound(a.u.begin(), a.u.end(), u);
        const std::size_t j = std::clamp<std::size_t>(static_cast<std::size_t>(it - a.u.begin()), 1, a.u.size() - 1);
        const float u0 = a.u[j - 1], u1 = a.u[j];
        const float f = u1 > u0 ? (u - u0) / (u1 - u0) : 0.0f;
        return boltAt(a.s[j - 1] + (a.s[j] - a.s[j - 1]) * f);
    }

    RE::NiTransform boltWay(float s) noexcept { return g_settings.core2 ? boltArc(s) : boltAt(s); }

    // The zones file's way for the slide / handle: how far it stands off its rest at u (0..1 of the way).

    bool hasRoute() noexcept { return g_settings.core2 && g_state.route.size() >= 2; }

    RE::NiPoint3 routeShift(float u) noexcept
    {
        const auto& st = g_state;
        if (st.route.size() < 2) {
            return {};
        }
        u = std::clamp(u, 0.0f, 1.0f);
        std::size_t j = 1;
        while (j + 1 < st.route.size() && st.routeAt[j] < u) {
            ++j;
        }
        const float a = st.routeAt[j - 1], b = st.routeAt[j];
        const float f = b > a ? std::clamp((u - a) / (b - a), 0.0f, 1.0f) : 1.0f;
        return st.route[j - 1] + (st.route[j] - st.route[j - 1]) * f - st.route[0];
    }

    // Where the slide's way has the part at u: turned by `angle`, moved by `lin`.

    void routeMotion(float u, float& angle, RE::NiPoint3& lin) noexcept
    {
        const auto& st = g_state;
        const auto& f = st.slideFit;
        u = std::clamp(u, 0.0f, 1.0f);
        if (f.arc && f.arcAt > 0.0f && u <= f.arcAt) {
            angle = f.angle * u / f.arcAt;
            lin = {};
            return;
        }
        angle = f.arc ? f.angle : 0.0f;
        lin = routeShift(u) - (f.arc ? st.route[f.k] - st.route[0] : RE::NiPoint3{});
    }

    RE::NiPoint3 routeApply(float u, const RE::NiPoint3& p) noexcept
    {
        float angle;
        RE::NiPoint3 lin;
        routeMotion(u, angle, lin);
        const auto& f = g_state.slideFit;
        return f.arc ? rotateAbout(p - f.pivot, f.axis, angle) + f.pivot + lin : p + lin;
    }

    RE::NiPoint3 routeInverse(float u, const RE::NiPoint3& p) noexcept
    {
        float angle;
        RE::NiPoint3 lin;
        routeMotion(u, angle, lin);
        const auto& f = g_state.slideFit;
        return f.arc ? rotateAbout(p - lin - f.pivot, f.axis, -angle) + f.pivot : p - lin;
    }

    bool boltPathFor(const Gun& gun) noexcept
    {
        if (!gun.weapon) {
            return false;
        }
        loadPlayed(gun.weapon->GetFormID());
        return g_play.parsed && g_play.boltKnown && g_play.weapon == gun.weapon->GetFormID();
    }

    // The slide: grip it with the free hand, pull it back along the gun, let it go forward.
    // All the way back throws out the chambered round; going forward takes the next one from the magazine.

    void rackByHand(const Gun& gun, RE::NiAVObject* root, RE::NiAVObject* bolt, float dt) noexcept
    {
        auto& st = g_state;
        if (!looksLikeObject(root) || !looksLikeObject(bolt)) {
            st.racking = false;
            return;
        }
        const auto& spare = g_hands[freeIndex()];
        const auto& firing = g_hands[g_settings.gunHand];
        const float scale = gunScale(root);
        const bool route = hasRoute();   // the zones file's way wins over the recorded one
        const bool path = !route && g_settings.recordedBolt && boltPathFor(gun) && looksLikeObject(bolt->parent);
        const float travel = g_settings.rackTravel;
        if (path && !st.boltAction && g_play.boltTurn > 0.35f) {
            st.boltAction = true;
        }
        // the controller, not the drawn hand: ROCK moves the drawn hand onto whatever it holds
        const auto handLocal = relative(root->world, pointTransform(spare.wand.translate)).translate;
        const bool handBusy = st.place == kRailHand || st.place == kHand || (griphook::owned() && st.armReason != kArmSlide) ||
                              (g_settings.core2 && (st.handRef != 0 || st.handToken != 0));
        const bool rockTook = g_rock.onBolt && !st.rockWasOnBolt && spare.valid && spare.grip && !handBusy && st.place != kRailAuto &&
                              Clock::now() - st.seatedAt > std::chrono::milliseconds(700);
        st.rockWasOnBolt = g_rock.onBolt;
        const float lockAt = g_settings.rackTravel * std::clamp(g_settings.slideLockAt, 0.0f, 1.0f);
        if (g_settings.core2) {
            // core2: no grab point. The palm in the slide's box moves it along its recorded way by the hand's own motion
            // along that way (motion across it is ignored): gripped it stays taken until the grip opens, open-handed
            // it is pushed only while the palm is on it. Let go, a slide springs home, a bolt action stays put.
            const float s0 = std::clamp(st.rackPull / travel, 0.0f, 1.0f);
            auto wayPoint = [&](float s) {
                s = std::clamp(s, 0.0f, 1.0f);
                if (route) {
                    return routeApply(s, st.handInBolt);
                }
                if (path) {
                    return compose(boltWay(s), pointTransform(st.handInBolt)).translate;
                }
                return st.handInBolt + RE::NiPoint3{ 0.0f, -s * travel, 0.0f };
            };
            auto shiftNow = [&]() {
                if (route) {
                    return routeShift(s0);
                }
                if (path) {
                    return boltWay(s0).translate - boltWay(0.0f).translate;
                }
                return RE::NiPoint3{ 0.0f, -st.rackPull, 0.0f };
            };
            const auto palmLocal = inGun(root, spare.palm);
            bool onSlide = false;
            if (spare.valid) {
                onSlide = st.slideBoxKnown ? inBox(st.slideLo, st.slideHi, route ? routeInverse(s0, palmLocal) : palmLocal - shiftNow(), g_settings.contactMargin)
                                           : distance(spare.palm, slideGrabPoint(root, bolt)) < g_settings.slideGrabRadius;
            }
            const bool magFirst = st.place == kRailAuto && palmOnMagazine(root, spare.palm);   // a magazine sticking out comes first
            const bool justSeated = Clock::now() - st.seatedAt < std::chrono::milliseconds(700);
            const bool mayTouch = spare.valid && !handBusy && !magFirst && !justSeated;
            auto anchor = [&]() {
                // the hand's point on the part, kept as the part moves: the way the hand goes is this point's way
                if (route) {
                    st.handInBolt = routeInverse(s0, handLocal);
                } else if (path) {
                    st.handInBolt = relative(boltWay(s0), pointTransform(handLocal)).translate;
                } else {
                    st.handInBolt = handLocal + RE::NiPoint3{ 0.0f, s0 * travel, 0.0f };
                }
            };
            if (!st.racking && mayTouch && spare.grip && ((!spare.previousGrip && onSlide) || rockTook)) {
                st.racking = true;
                st.pushing = false;
                anchor();
                buzz(freeIndex(), 500);
                detail("CORE2: the slide is taken (palm in its box), %.2f of its way", s0);
            }
            if (st.racking && !(spare.valid && spare.grip)) {
                st.racking = false;
            }
            const bool push = !st.racking && mayTouch && !spare.grip && onSlide;
            if (push && !st.pushing) {
                anchor();
                detail("CORE2: the open palm is on the slide, %.2f of its way", s0);
            }
            st.pushing = push;
            if ((st.racking || st.pushing) && st.prevHandKnown) {
                const float e = 0.03f;
                const float a = std::clamp(s0 - e, 0.0f, 1.0f);
                const float b = std::clamp(s0 + e, 0.0f, 1.0f);
                const auto tangent = (wayPoint(b) - wayPoint(a)) * (1.0f / std::max(b - a, 0.001f));
                const float tt = dot(tangent, tangent);
                if (tt > 1e-4f) {
                    const auto moved = handLocal - st.prevHandLocal;
                    float ds = dot(moved, tangent) / tt;
                    ds = std::clamp(ds, -0.4f, 0.4f);
                    if (st.pushing) {
                        ds *= std::clamp(g_settings.pushFactor, 0.0f, 1.0f);   // an open palm slips on it
                        if (dot(moved, moved) > 36.0f) {
                            ds = 0.0f;   // a jump of the tracking, not a push
                        }
                    }
                    const float lo = st.slideLocked ? lockAt / travel : 0.0f;
                    st.rackPull = std::clamp(s0 + ds, lo, 1.0f) * travel;
                }
            } else if (st.slideLocked) {
                st.rackPull = approach(st.rackPull, lockAt, 80.0f * dt);
            } else if (!st.boltAction) {
                st.rackPull = approach(st.rackPull, 0.0f, 25.0f * travel * dt);
            }
            if (st.slideLocked && st.rackPull >= travel * 0.95f) {
                st.slideLocked = false;   // pulled off the slide stop: let go and it runs home
                st.rackCycled = true;
                playSound(2);
                logNote("RACK: slide pulled off the stop");
            }
        } else {
            if (!st.racking && spare.valid && spare.grip && ((!spare.previousGrip && !handBusy) || rockTook)) {
                const float toSlide = st.place != kRailAuto ? distance(spare.palm, slideGrabPoint(root, bolt)) : 1e9f;
                const float toMag = st.place == kRailAuto && st.railKnown ? distance(spare.palm, grabPoint(root)) : 1e9f;
                if (rockTook || (toSlide < g_settings.slideGrabRadius && toSlide <= toMag)) {
                    st.racking = true;
                    st.rackGrabY = handLocal.y + st.rackPull;
                    st.rackGrabHands = distance(spare.wand.translate, firing.wand.translate);
                    st.rackGrabPull = st.rackPull;
                    if (path) {
                        st.handInBolt = relative(boltAt(st.rackPull / travel), pointTransform(handLocal)).translate;
                    }
                    buzz(freeIndex(), 500);
                    detail("Slide taken by the free hand (%.1f from its middle)", toSlide);
                }
            }
            if (st.racking && !(spare.valid && spare.grip)) {
                st.racking = false;   // let go: the spring sends it home
            }
            if (st.racking && path) {
                // the hand's point on the bolt follows the recorded way back (a bolt action: up, then back)
                const float cur = st.rackPull / travel;
                // the pull along the bolt's way back counts fully, the rest (lifting the handle) only partly:
                // pulling back lifts the handle by itself instead of holding the bolt where it is
                const auto travelled = boltAt(1.0f).translate - boltAt(0.0f).translate;
                const auto wayBack = dot(travelled, travelled) > 0.09f ? normalized(travelled) : RE::NiPoint3{};
                float best = cur, bestDistance = 1e9f;
                for (int k = -35; k <= 35; ++k) {
                    const float s = std::clamp(cur + k * 0.01f, 0.0f, 1.0f);
                    const auto at = compose(boltAt(s), pointTransform(st.handInBolt)).translate;
                    const auto off = at - handLocal;
                    const float along = dot(off, wayBack);
                    const auto side = off - wayBack * along;
                    const float d = along * along + 0.1f * dot(side, side);
                    if (d < bestDistance) {
                        bestDistance = d;
                        best = s;
                    }
                }
                st.rackPull = std::clamp(best * travel, st.slideLocked ? lockAt : 0.0f, travel);
                if (st.slideLocked && st.rackPull >= travel * 0.95f) {
                    st.slideLocked = false;
                    st.rackCycled = true;
                    playSound(2);
                    logNote("RACK: slide pulled off the stop");
                }
            } else if (st.racking) {
                // along the gun, and by the hands closing in (when the gun turns after the hand, the first stays put)
                const float byGun = st.rackGrabY - handLocal.y;
                const float byHands = st.rackGrabPull + (st.rackGrabHands - distance(spare.wand.translate, firing.wand.translate)) / scale;
                st.rackPull = std::clamp(std::max(byGun, byHands), st.slideLocked ? lockAt : 0.0f, g_settings.rackTravel);
                if (st.slideLocked && st.rackPull >= g_settings.rackTravel * 0.95f) {
                    st.slideLocked = false;   // pulled off the slide stop: let go and it runs home
                    st.rackCycled = true;
                    playSound(2);
                    logNote("RACK: slide pulled off the stop");
                }
            } else if (st.slideLocked) {
                st.rackPull = approach(st.rackPull, lockAt, 80.0f * dt);   // held open after the last shot
            } else if (!st.boltAction) {
                st.rackPull = approach(st.rackPull, 0.0f, 25.0f * g_settings.rackTravel * dt);
            }
        }
        if (!st.rackCycled && !st.slideLocked && st.rackPull >= g_settings.rackTravel * 0.85f) {
            st.rackCycled = true;
            if (st.chamber || st.spentInChamber) {
                // out of the ejection port, to the right and up
                const auto port = looksLikeObject(bolt) ? bolt->world.translate : root->world.translate;
                ejectCasings(gun, root, port, axisOf(root->world, 0) + axisOf(root->world, 2) * 0.6f, 1,
                    st.chamber ? "an unfired round, racked out" : "the fired case, racked out");
                st.spentInChamber = false;
            }
            if (st.chamber) {
                st.chamber = false;   // the round flies out (back to the ammo you carry)
            }
            syncAmmo(gun);
            playSound(2);
            buzz(freeIndex(), 1500);
            logNote("RACK: slide all the way back");
        }
        if (st.rackCycled && st.rackPull <= g_settings.rackTravel * 0.25f) {
            st.rackCycled = false;
            if (st.place == kSeated && st.rounds > 0) {
                st.rounds -= 1;
                st.chamber = true;
            }
            syncAmmo(gun);
            playSound(3);
            buzz(freeIndex(), 2000);
            buzz(g_settings.gunHand, 1500);
            note("RACK: slide home, chamber %s, magazine %u", st.chamber ? "loaded" : "empty", st.rounds);
        }
        if (route) {
            if ((st.rackPull > 0.0f || st.racking) && looksLikeObject(bolt->parent)) {
                // the bolt stands at rest here (set before): turn it and move it along the way
                const auto rest = relative(root->world, bolt->world);
                float angle;
                RE::NiPoint3 lin;
                routeMotion(st.rackPull / travel, angle, lin);
                const auto& f = st.slideFit;
                const auto posed = f.arc ? turnedPose(rest, f.axis, f.pivot, angle, lin) : turnedPose(rest, RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, RE::NiPoint3{}, 0.0f, lin);
                bolt->local = relative(bolt->parent->world, compose(root->world, posed));
                refreshSubtree(bolt, RE::NiPoint3{});
            }
        } else if (path) {
            if (st.rackPull > 0.0f || st.racking) {
                auto recorded = boltWay(st.rackPull / travel);
                recorded.scale = bolt->world.scale / scale;
                bolt->local = relative(bolt->parent->world, compose(root->world, recorded));
                refreshSubtree(bolt, RE::NiPoint3{});
            }
        } else if (st.rackPull > 0.0f) {
            refreshSubtree(bolt, axisOf(root->world, 1) * (-st.rackPull * scale));
        }
    }
}
