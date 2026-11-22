// ManualReloadF: the player's animation events (NotifyAnimationGraph): the game's own reload of a gun reloaded by hand is
// stopped before it starts, so its animation never plays (an empty gun used to play it by itself)
#pragma once

namespace RE
{
    class PlayerCharacter;
}

namespace mrf::animhook
{
    // once the player is there; patches the player's IAnimationGraphManagerHolder vtable (slot 1)
    void install(RE::PlayerCharacter* player) noexcept;
    bool installed() noexcept;
    // the gun in hand is reloaded by hand: the game's reload events are refused
    void setBlocking(bool value) noexcept;
    // how many reload starts were refused since the gun was taken
    unsigned blockedCount() noexcept;
    // a reload start was refused since the last call (the game asked to reload: the button, or the gun ran dry)
    bool takeRefused() noexcept;
}
