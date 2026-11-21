#include "Recorder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
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

    AnimRecorder g_rec;

    std::unordered_map<std::uint32_t, bool> g_recorded;   // weapon -> a recording of its reload is on disk
    bool g_playStale{ false };                              // a new recording: read it again

    bool hasRecording(std::uint32_t weapon) noexcept
    {
        const auto known = g_recorded.find(weapon);
        if (known != g_recorded.end()) {
            return known->second;
        }
        char path[160];
        std::snprintf(path, sizeof(path), "Data/F4SE/Plugins/ManualReloadF_anim/%08X.txt", weapon);
        // a take under a second (cut short) does not count: the gun is recorded again
        bool there = false;
        if (std::ifstream file(path); file) {
            std::string first;
            std::getline(file, first);
            const auto over = first.find(" over ");
            there = over != std::string::npos && std::strtof(first.c_str() + over + 6, nullptr) >= 1.0f;
        }
        g_recorded[weapon] = there;
        return there;
    }

    M3 colsOf(const RE::NiTransform& t) noexcept
    {
        M3 r{};
        for (int c = 0; c < 3; ++c) {
            const auto a = axisOf(t, c);
            r.m[0][c] = a.x; r.m[1][c] = a.y; r.m[2][c] = a.z;
        }
        return r;
    }

    M3 mulAB(const M3& a, const M3& b) noexcept
    {
        M3 r{};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
        return r;
    }

    M3 transposed(const M3& a) noexcept
    {
        M3 r{};
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
        return r;
    }

    RE::NiPoint3 mulV(const M3& a, const RE::NiPoint3& v) noexcept
    {
        return { a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z,
            a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z };
    }

    float angleOf(const M3& d) noexcept
    {
        const float c = (d.m[0][0] + d.m[1][1] + d.m[2][2] - 1.0f) * 0.5f;
        return std::acos(std::clamp(c, -1.0f, 1.0f));
    }

    float det3(const M3& a) noexcept
    {
        return a.m[0][0] * (a.m[1][1] * a.m[2][2] - a.m[1][2] * a.m[2][1]) - a.m[0][1] * (a.m[1][0] * a.m[2][2] - a.m[1][2] * a.m[2][0]) +
               a.m[0][2] * (a.m[1][0] * a.m[2][1] - a.m[1][1] * a.m[2][0]);
    }

    bool solve3(const M3& a, const RE::NiPoint3& b, RE::NiPoint3& x) noexcept
    {
        const float d = det3(a);
        if (std::fabs(d) < 1e-6f) return false;
        float out[3]{};
        const float bv[3]{ b.x, b.y, b.z };
        for (int c = 0; c < 3; ++c) {
            M3 m = a;
            for (int r = 0; r < 3; ++r) m.m[r][c] = bv[r];
            out[c] = det3(m) / d;
        }
        x = { out[0], out[1], out[2] };
        return true;
    }

    void finishRecording() noexcept
    {
        auto& r = g_rec;
        r.active = false;
        const auto frameCount = r.frames.size();
        if (frameCount < 10) {
            note("RECORD: too short (%zu frames), dropped", frameCount);
            return;
        }
        ++r.takes;
        char folder[] = "Data/F4SE/Plugins/ManualReloadF_anim";
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        char path[160];
        std::snprintf(path, sizeof(path), "%s/%08X.txt", folder, r.weapon);
        std::ofstream out(path, std::ios::trunc);
        char line[512];
        std::snprintf(line, sizeof(line), "; ManualReloadF: the game's reload of weapon %08X, %zu frames over %.2f s, %zu nodes\n", r.weapon, frameCount,
            r.times.back(), r.names.size());
        out << line;
        note("RECORD: weapon %08X, %zu frames over %.2f s -> %s", r.weapon, frameCount, r.times.back(), path);
        std::vector<std::size_t> moving;
        for (std::size_t n = 0; n < r.names.size(); ++n) {
            const auto& rest = r.frames[0][n];
            const auto restCols = colsOf(rest);
            float bestAngle = 0.0f, bestMove = 0.0f;
            std::size_t angleAt = 0, moveAt = 0;
            for (std::size_t f = 1; f < frameCount; ++f) {
                const auto& now = r.frames[f][n];
                const float a = angleOf(mulAB(colsOf(now), transposed(restCols)));
                const auto d = now.translate - rest.translate;
                const float m = std::sqrt(dot(d, d));
                if (a > bestAngle) { bestAngle = a; angleAt = f; }
                if (m > bestMove) { bestMove = m; moveAt = f; }
            }
            bool showsUp = false, vanishes = false;
            for (std::size_t f = 1; f < frameCount; ++f) {
                if (r.culled[f][n] != r.culled[f - 1][n]) {
                    (r.culled[f][n] ? vanishes : showsUp) = true;
                    std::snprintf(line, sizeof(line), "event %.3f %s %s\n", r.times[f], r.culled[f][n] ? "hide" : "show", r.names[n].c_str());
                    out << line;
                }
            }
            if (bestAngle < 0.05f && bestMove < 0.3f) {
                continue;
            }
            // the frames it is away from rest (over a fifth of its peak)
            float from = -1.0f, to = -1.0f;
            for (std::size_t f = 1; f < frameCount; ++f) {
                const auto& now = r.frames[f][n];
                const float a = angleOf(mulAB(colsOf(now), transposed(restCols)));
                const auto d = now.translate - rest.translate;
                const float m = std::sqrt(dot(d, d));
                if ((bestAngle >= 0.05f && a > bestAngle * 0.2f) || (bestAngle < 0.05f && m > bestMove * 0.2f)) {
                    if (from < 0.0f) from = r.times[f];
                    to = r.times[f];
                }
            }
            moving.push_back(n);
            if (bestAngle >= 0.05f) {
                const auto& peak = r.frames[angleAt][n];
                const M3 d = mulAB(colsOf(peak), transposed(restCols));
                const float s = std::sin(bestAngle);
                RE::NiPoint3 axis{ d.m[2][1] - d.m[1][2], d.m[0][2] - d.m[2][0], d.m[1][0] - d.m[0][1] };
                axis = axis * (1.0f / (2.0f * (std::fabs(s) > 1e-4f ? s : 1e-4f)));
                const float len = std::sqrt(dot(axis, axis));
                if (len > 1e-4f) axis = axis * (1.0f / len);
                // the pivot: the point the turn leaves where it was (I - D) p = t1 - D t0, pinned along the axis
                M3 a{};
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) a.m[i][j] = (i == j ? 1.0f : 0.0f) - d.m[i][j];
                const float ax[3]{ axis.x, axis.y, axis.z };
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) a.m[i][j] += ax[i] * ax[j];
                const auto rhs = peak.translate - mulV(d, rest.translate) + axis * dot(axis, rest.translate);
                RE::NiPoint3 pivot = rest.translate;
                const bool pivotOk = solve3(a, rhs, pivot);
                // how far the pivot really stays put (a pure hinge leaves it still)
                const auto moved = mulV(d, pivot - rest.translate) + peak.translate - pivot;
                std::snprintf(line, sizeof(line),
                    "hinge %s angle %.1f axis %.3f %.3f %.3f pivot %.3f %.3f %.3f slip %.2f%s from %.3f to %.3f peak %.3f move %.2f\n", r.names[n].c_str(),
                    bestAngle * 57.29578f, axis.x, axis.y, axis.z, pivot.x, pivot.y, pivot.z, std::sqrt(dot(moved, moved)), pivotOk ? "" : " (no pivot)", from, to,
                    r.times[angleAt], bestMove);
            } else {
                const auto d = r.frames[moveAt][n].translate - r.frames[0][n].translate;
                const auto dir = d * (1.0f / bestMove);
                std::snprintf(line, sizeof(line), "slide %s travel %.2f dir %.3f %.3f %.3f from %.3f to %.3f peak %.3f\n", r.names[n].c_str(), bestMove, dir.x,
                    dir.y, dir.z, from, to, r.times[moveAt]);
            }
            out << line;
            line[std::strlen(line) - 1] = '\0';
            logNote(line);
            (void)showsUp;
            (void)vanishes;
        }
        // the moving nodes frame by frame, for working them out offline: time, then per node x y z and the 3 axes
        out << "; frames: t | name x y z ax ay az bx by bz cx cy cz (axes of the node in the gun root's frame)\n";
        for (std::size_t f = 0; f < frameCount; ++f) {
            for (const auto n : moving) {
                const auto& t = r.frames[f][n];
                const auto c = colsOf(t);
                std::snprintf(line, sizeof(line), "f %.3f %s %.3f %.3f %.3f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %d\n", r.times[f], r.names[n].c_str(),
                    t.translate.x, t.translate.y, t.translate.z, c.m[0][0], c.m[1][0], c.m[2][0], c.m[0][1], c.m[1][1], c.m[2][1], c.m[0][2], c.m[1][2],
                    c.m[2][2], r.culled[f][n] ? 1 : 0);
                out << line;
            }
        }
        g_recorded[r.weapon] = true;
        g_playStale = true;
        hud("Reload recorded: from now on this gun is reloaded by hand");
    }

    void recordReload(RE::PlayerCharacter* player, const Gun& gun, Clock::time_point now) noexcept
    {
        auto& r = g_rec;
        auto* root = findNamed(player->firstPerson3D.get(), "Weapon");
        if (!looksLikeObject(root) || !gun.weapon) {
            if (r.active) finishRecording();
            return;
        }
        const bool reloading = static_cast<std::uint32_t>(player->gunState) == static_cast<std::uint32_t>(g_settings.reloadGunState);
        if (reloading) {
            r.lastSeen = now;
        }
        if (!r.active) {
            if (!reloading) {
                return;
            }
            r = AnimRecorder{ .takes = r.takes };
            r.active = true;
            r.start = now;
            r.lastSeen = now;
            r.weapon = gun.weapon->GetFormID();
            forEachNode(root, [&](RE::NiAVObject* object, int depth) {
                if (depth > 30 || r.nodes.size() >= 400) return false;
                auto name = nodeName(object);
                if (!name.empty()) {
                    for (auto& c : name) if (c == ' ') c = '_';
                    r.names.push_back(name);
                    r.nodes.push_back(object);
                }
                return true;
            });
            note("RECORD: the game's reload of %08X started, %zu nodes", r.weapon, r.nodes.size());
        }
        const float t = std::chrono::duration<float>(now - r.start).count();
        std::vector<RE::NiTransform> frame(r.nodes.size());
        std::vector<std::uint8_t> hidden(r.nodes.size());
        for (std::size_t n = 0; n < r.nodes.size(); ++n) {
            auto* object = r.nodes[n];
            if (looksLikeObject(object)) {
                frame[n] = relative(root->world, object->world);
                hidden[n] = (object->flags.flags & 1ull) ? 1 : 0;
            } else {
                frame[n] = r.frames.empty() ? identityTransform() : r.frames.back()[n];
                hidden[n] = 1;
            }
        }
        r.times.push_back(t);
        r.frames.push_back(std::move(frame));
        r.culled.push_back(std::move(hidden));
        // keeps going 2.5 s after the state ends (the game's animation runs on after it), at most 8 s
        if ((!reloading && now - r.lastSeen > std::chrono::milliseconds(2500)) || t > 8.0f) {
            finishRecording();
        }
    }
}
