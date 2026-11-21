// ManualReloadF: Small helpers of the per-frame update: the real magazine in hand, putting the gun away, buttons, menus
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
    // ------------------------------------------------------------ every frame
    // core2: a real magazine on its way into the free hand, or held there by ROCK. ROCK is asked again while the
    // grip is held and it has not taken it; let go, it falls (at the belt or over the shoulder it is put away).
    void realMagazineInHand(RE::PlayerCharacter* player) noexcept;

    // The gun is gone from the hand while its magazine was in play: the magazine goes back to the pouch.
    void putAway(const char* why) noexcept;

    std::uint64_t buttonMask(int id) noexcept;

    bool menuOpen() noexcept;
}
