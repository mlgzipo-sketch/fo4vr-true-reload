// ManualReloadF: Guns that open: revolvers, break actions, launchers, the alien blaster, the gamma gun; the gatling's crank and handle
#pragma once

#include <RE/Fallout.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "State.h"
#include "Gun.h"
#include "Hands.h"

namespace mrf
{
    // ------------------------------------------------------------ revolvers and break actions
    // Turns the rows (the node's own axes as its parent sees them) about one of the parent's axes.
    void turnRows(RE::NiMatrix3& rotation, int axis, float angle) noexcept;

    // The laser musket: the free hand takes the crank's handle and winds it round; each full turn charges one more shot.
    // The node the handle (zones "route lever") hangs on: of WeaponBolt and WeaponExtra1..3, the one with a model
    // nearest to where the handle is drawn.
    RE::NiAVObject* leverNodeOf(RE::NiAVObject* root) noexcept;

    // The handle on its way: turned about the drawn arc first, then moved along the rest of the way.
    void poseLever(RE::NiAVObject* root, RE::NiAVObject* node, float lever, const RE::NiTransform& restInRoot) noexcept;

    // A magazine gun with a handle (the gatling laser): pulled out, it throws the core out; put back home.
    void updateMagLever(const Gun& gun, RE::NiAVObject* root, Clock::time_point now) noexcept;

    void updateCrank(RE::PlayerCharacter* player, const Gun& gun, RE::NiAVObject* root, Clock::time_point now) noexcept;

    // writes "<key> x y z r00 .. r22" (a pose against the controller) into the gun's zones file, replacing the old one
    bool writeHoldLine(std::uint32_t weapon, const char* key, const RE::NiTransform& hold) noexcept;

    // a pose against the controller, its rotation made square (a scaled node leaves its scale in the rows)
    RE::NiTransform holdAgainst(const RE::NiTransform& wandIn, const RE::NiTransform& node) noexcept;

    // The reload button opens and closes the gun. Open, with the muzzle up, everything falls out of it. Rounds
    // come from the belt in the free hand (a full clip for a revolver, one shell at a time for a break action)
    // and go in at the breech. The free hand on the gun, or a jerk of the gun, closes it.
    void updateBreech(RE::PlayerCharacter* player, const Gun& gun, RE::NiAVObject* root, Clock::time_point now, float dt) noexcept;
}
