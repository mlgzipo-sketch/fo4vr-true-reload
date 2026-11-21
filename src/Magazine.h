// ManualReloadF: The belt and shoulder zones, and the magazine's life: eject, drop, take, seat, the belt pose
#pragma once

#include <RE/Fallout.h>
#include <cstdint>
#include <string>
#include "State.h"
#include "Gun.h"
#include "Hands.h"

namespace mrf
{
    // ------------------------------------------------------------ zones around the body
    bool inBelt(RE::PlayerCharacter* player, const RE::NiPoint3& palm, float* heightOut = nullptr, float* sideOut = nullptr) noexcept;

    // Behind the body and above the shoulders: where loot is put away.
    bool overShoulder(RE::PlayerCharacter* player, const RE::NiPoint3& palm) noexcept;

    // ------------------------------------------------------------ the magazine's life
    void startBlend(const RE::NiTransform& from, float seconds) noexcept;

    RE::NiTransform blended(const RE::NiTransform& target, Clock::time_point now) noexcept;

    // Caught: the magazine keeps the turn it had in the hand and lines up with the well only as it is pushed in
    // (fully lined up a little before it is seated), so nothing jumps at the mouth and the hand is not turned.
    void startAlign(const RE::NiTransform& rail, const RE::NiTransform& caught, float s) noexcept;

    RE::NiTransform aligned(const RE::NiTransform& rail) noexcept;

    void releaseHand() noexcept;

    void startEject(const Gun& gun, bool hold, Clock::time_point now) noexcept;

    void seat(const Gun& gun) noexcept;

    // The magazine leaves our hands and becomes a real object; it is drawn falling until that object is there.
    void startDrop(const RE::NiTransform& node, const RE::NiPoint3& velocity, const char* why) noexcept;

    void stash(const char* where) noexcept;

    // The free hand closes on a magazine that sticks out: the palm goes to the magazine, not the magazine to the palm.
    void takeFromRail(RE::NiAVObject* root, const Hand& hand) noexcept;

    // The well's way out as the magazine itself carries it (its own frame at rest), turned with a loose magazine:
    // a cell or a drum that leaves sideways is lined up when this points along the well, not its own Z.
    RE::NiPoint3 wayOutOf(const RE::NiTransform& node) noexcept;

    // How a magazine from the belt sits in the hand: upright in the fist, rounds forward.
    RE::NiTransform holdPose(const Hand& hand, float scale) noexcept;

    // Where the palm holds a magazine, on the magazine as it sits in the gun (gun frame): "point maghold" from the zones
    // file, else the middle of its outer half (a cell: its middle).
    RE::NiPoint3 defaultHoldPoint() noexcept;

    // A magazine from the belt: turned exactly as it goes into the gun as the gun is held now, held at its hold point,
    // so it reaches the well lined up and nothing turns the hand there. Without a gun or a rail: the old fist pose.
    RE::NiTransform beltPose(const RE::NiAVObject* root, const Hand& hand, float scale) noexcept;

    std::uint32_t freshMagazineRounds(const Gun& gun, std::uint32_t inventory) noexcept;

    // The rounds seen on top of a magazine: as many as it holds (up to the ones the model has).
    bool isGunPartOnMag(std::string name) noexcept;

    // The cryolator hangs its drum under WeaponMagazine: without this the drum leaves with the cell.
    // The drum is kept where it sits on the gun; when the magazine node would be hidden, only its
    // other branches are hidden.
    void keepGunPartsOnMag(RE::NiAVObject* root, RE::NiAVObject* magazine, bool seated) noexcept;

    void showRounds(RE::NiAVObject* magazine, std::uint32_t rounds) noexcept;

    // Real magazines: forgotten when they are gone, cleaned up when old, and taken over by this plugin
    // when the free hand brings one to a gun that has none.
    void looseMagazines(const Gun* gun, RE::NiAVObject* root) noexcept;
}
