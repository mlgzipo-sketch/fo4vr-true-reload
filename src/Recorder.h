// ManualReloadF: Writes the game's own reload animation down (bRecordAnim / bAutoRecord): node poses per frame, then hinge and slide fits
#pragma once

#include <RE/Fallout.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "State.h"
#include "Gun.h"

namespace mrf
{
    // ------------------------------------------------------------ the game's reload, written down

    // While bRecordAnim is on the plugin leaves the gun alone; each time the game plays its own reload,
    // every node of the gun is taken against the gun's root each frame, and what moved is worked out:
    // a turn about a hinge (axis, pivot, angle) or a slide along a line (direction, travel), with when it
    // happens, and nodes that show up or vanish (shells, a speedloader).
    struct AnimRecorder
    {
        bool                                   active{ false };
        Clock::time_point                      start{};
        Clock::time_point                      lastSeen{};
        std::uint32_t                          weapon{ 0 };
        std::vector<std::string>               names;
        std::vector<RE::NiAVObject*>           nodes;
        std::vector<float>                     times;
        std::vector<std::vector<RE::NiTransform>> frames;   // [frame][node], against the gun root
        std::vector<std::vector<std::uint8_t>> culled;
        int                                    takes{ 0 };
    };

    extern AnimRecorder g_rec;

    extern std::unordered_map<std::uint32_t, bool> g_recorded;   // weapon -> a recording of its reload is on disk
    extern bool g_playStale;                              // a new recording: read it again

    bool hasRecording(std::uint32_t weapon) noexcept;

    struct M3 { float m[3][3]; };   // columns = the node's axes in the root's frame

    M3 colsOf(const RE::NiTransform& t) noexcept;

    M3 mulAB(const M3& a, const M3& b) noexcept;

    M3 transposed(const M3& a) noexcept;

    RE::NiPoint3 mulV(const M3& a, const RE::NiPoint3& v) noexcept;

    float angleOf(const M3& d) noexcept;

    float det3(const M3& a) noexcept;

    bool solve3(const M3& a, const RE::NiPoint3& b, RE::NiPoint3& x) noexcept;

    void finishRecording() noexcept;

    void recordReload(RE::PlayerCharacter* player, const Gun& gun, Clock::time_point now) noexcept;
}
