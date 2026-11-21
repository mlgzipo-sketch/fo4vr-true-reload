// ManualReloadF: The recorded reload played back by hand: the moving parts' paths, hinges and the bolt's way
#pragma once

#include <RE/Fallout.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "State.h"
#include "Gun.h"

namespace mrf
{
    // ------------------------------------------------------------ the game's reload, played back by hand

    // A recording (ManualReloadF_anim/<weapon>.txt) read back: the moving nodes frame by frame against the gun root,
    // and the hinge of the part that opens (WeaponMagazine: a cylinder, the barrels). The opening part and whatever
    // moves with it are posed from the recording at a point of its opening move chosen by the plugin or the hand.
    struct PlayedAnim
    {
        bool                                      loaded{ false };
        std::uint32_t                             weapon{ 0 };
        std::vector<std::string>                  names;
        std::vector<float>                        times;
        std::vector<std::vector<RE::NiTransform>> frames;   // [frame][node]
        float                                     openTime{ 0.0f };
        float                                     openAngle{ 0.0f };   // radians
        RE::NiPoint3                              axis{};              // hinge, gun root frame
        RE::NiPoint3                              pivot{};
        std::vector<int>                          posed;               // the nodes posed (indices in names)
        std::vector<RE::NiAVObject*>              nodes;               // resolved on the drawn gun
        RE::NiAVObject*                           rootSeen{ nullptr };
        bool                                      parsed{ false };     // frames are there (whatever opens)
        std::unordered_map<std::string, int>      index;
        // the bolt / slide (WeaponBolt): its way back, from rest to where it goes furthest
        bool                                      boltKnown{ false };
        int                                       bolt{ -1 };
        std::size_t                               boltFrom{ 0 };
        std::size_t                               boltTo{ 0 };
        float                                     boltTurn{ 0.0f };    // radians it turns on the way (a bolt action lifts its handle)
    };

    extern PlayedAnim g_play;

    bool loadPlayed(std::uint32_t weapon) noexcept;

    RE::NiTransform playedAt(const PlayedAnim& p, int node, float time) noexcept;

    // Poses the opening part as the recording has it at `amount` (0 closed .. 1 open) of its opening move.
    bool posePlayed(RE::NiAVObject* root, float amount) noexcept;

    // The angle of a point about the hinge (gun root frame), for turning the opening part by hand.
    float hingeAngle(const RE::NiAVObject* root, const RE::NiPoint3& world) noexcept;

    // The bolt as the recording has it at `s` (0 rest .. 1 furthest back), in the gun root's frame.
    RE::NiTransform boltAt(float s) noexcept;

    // core2: the recorded way measured by how far the bolt really goes, not by time. The game's animation pulls the
    // slide back in a third of its take and holds it there: by time, most of the hand's way would move nothing, and the
    // rest would throw the slide. Here 0..1 is the distance along the way (turns count by a point 3 units off the
    // axis), ending where the slide is furthest back (a bolt action keeps its whole way).
    struct BoltArc
    {
        std::uint32_t weapon{ 0 };
        std::size_t   frames{ 0 };
        std::vector<float> s;      // time fraction of each sample
        std::vector<float> u;      // distance fraction of each sample
    };

    extern BoltArc g_arc;

    void buildArc() noexcept;

    RE::NiTransform boltArc(float u) noexcept;

    RE::NiTransform boltWay(float s) noexcept;

    // The zones file's way for the slide / handle: how far it stands off its rest at u (0..1 of the way).
    bool hasRoute() noexcept;

    RE::NiPoint3 routeShift(float u) noexcept;

    // Where the slide's way has the part at u: turned by `angle`, moved by `lin`.
    void routeMotion(float u, float& angle, RE::NiPoint3& lin) noexcept;

    RE::NiPoint3 routeApply(float u, const RE::NiPoint3& p) noexcept;

    RE::NiPoint3 routeInverse(float u, const RE::NiPoint3& p) noexcept;

    bool boltPathFor(const Gun& gun) noexcept;

    // The slide: grip it with the free hand, pull it back along the gun, let it go forward.
    // All the way back throws out the chambered round; going forward takes the next one from the magazine.
    void rackByHand(const Gun& gun, RE::NiAVObject* root, RE::NiAVObject* bolt, float dt) noexcept;
}
