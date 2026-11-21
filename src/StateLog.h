// ManualReloadF: What the plugin thinks, written to the log on every change; saving a hand pose
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
    // ------------------------------------------------------------ what the plugin thinks, for the log

    inline const char* placeName(int place) noexcept
    {
        switch (place) {
        case kSeated: return "in the gun";
        case kRailAuto: return "sticking out by itself";
        case kRailHand: return "in the well, moved by the hand";
        case kHand: return "drawn in the hand (real one on its way)";
        case kGone: return "no magazine in the gun";
        default: return "?";
        }
    }

    std::string nodeState(RE::NiAVObject* node) noexcept;

    // One line per thing: the magazine, the slide, the free hand, the real magazines around.
    // The stick click with a magazine in the free hand: the way it is held now is how it comes from the belt from now on
    // (zones file "hold": where the magazine stands against the controller). Returns false with no magazine in the hand.
    bool saveHold(const Gun& gun) noexcept;

    void writeState(const char* why, const Gun& gun, RE::NiAVObject* root, RE::NiAVObject* magazine, RE::NiAVObject* bolt) noexcept;

    // A short fingerprint of the state: when it changes the whole state is written down.
    std::string stateKey() noexcept;
}
