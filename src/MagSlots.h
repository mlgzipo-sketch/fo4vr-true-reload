// ManualReloadF: magazines of other mods on base forms of their own, so ROCK keeps a hold per magazine
//
// Every magazine of another mod used to be one base form (MRF_MagGeneric): ROCK keeps its hold per base form, so all of
// them shared one hold. Now each is made from one of 64 slot forms (MRF_MagSlot00..63, kinds 34..97 of MRF_Mags):
//   - slots 0..11: one per kind of magazine, told by its size (pistol, rifle, drum, box; small, medium, large), so a
//     rifle magazine of one gun is held like a rifle magazine of another;
//   - slots 12..23: the same kinds for magazines worn by a carrier (their root is their middle);
//   - slots 24..63: one per magazine whose hold was saved (stick click) when its kind's hold did not suit it.
// A saved hold is given to ROCK as the first hold of that slot's base form. All of it is kept in
// Data/F4SE/Plugins/ManualReloadF_slots.txt.
#pragma once

#include <RE/Fallout.h>
#include <cstdint>
#include <string>

namespace mrf
{
    inline constexpr int kSlotKindBase = 34;   // MRF_MagSlot00 in MRF_Mags
    inline constexpr int kSlotCount = 64;
    inline constexpr int kClassSlots = 24;   // 0..11 kinds, 12..23 the same kinds worn by a carrier

    // The kind a real magazine of another mod is made from (its slot form gets the model `model` wears now); the
    // plain generic kind when the plugin has no slots (an old esp).
    int magSlotKind(const std::string& magModel, const char* formModel, bool carrier) noexcept;

    // The stick click with a magazine of another mod held by ROCK: its hold is kept for its kind (the first one of
    // that kind, or the one that set it) or for it alone. `said` = a line for the HUD.
    bool saveSlotHold(const std::string& magModel, std::uint32_t heldRef, int hand, std::string& said) noexcept;

    // Once ROCK is there: the holds saved before are given to it again.
    void giveSlotHolds(int hand) noexcept;

    // Each frame a magazine of another mod is held by ROCK: once settled, where ROCK put it against its saved hold tells
    // ROCK's own grab frame; the holds are given again in it when they are off.
    void watchSlotHold(int hand, std::uint32_t heldRef) noexcept;
}
