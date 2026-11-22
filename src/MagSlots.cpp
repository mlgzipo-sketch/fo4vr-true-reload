#include "MagSlots.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "Gun.h"
#include "Log.h"
#include "RockLink.h"

namespace mrf
{
    namespace
    {
        constexpr RE::TESFormID kFirstSlotForm = 0x82F;   // MRF_MagSlot00
        constexpr const char* kSlotsPath = "Data/F4SE/Plugins/ManualReloadF_slots.txt";
        constexpr const char* kShapes[4]{ "pistol", "rifle", "drum", "box" };
        constexpr const char* kSizes[3]{ "small", "medium", "large" };

        struct Slot
        {
            std::string    key;      // own slot: the magazine it belongs to; kind slot: the magazine whose hold it has
            bool           hasHold{ false };
            rocklink::Pose hold{};   // the magazine's root against the hand
            std::string    model;    // what its form wears now
        };

        std::array<Slot, kSlotCount> g_slots;
        std::unordered_map<std::string, int> g_kindOf;   // magazine -> its kind slot (told once, by its size)
        bool g_loaded{ false };
        bool g_given{ false };

        // "Weapons\DOOM\MAC11\MAC11Mag.nif" -> "mac11mag"
        std::string magKey(const std::string& model)
        {
            const auto slash = model.find_last_of("\\/");
            auto name = model.substr(slash == std::string::npos ? 0 : slash + 1);
            if (const auto dot = name.rfind('.'); dot != std::string::npos) {
                name.resize(dot);
            }
            std::string key;
            for (const char c : name) {
                if (c != ' ' && c != '\t') {
                    key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
            }
            return key.empty() ? std::string("-") : key;
        }

        RE::TESObjectMISC* slotForm(int slot) noexcept
        {
            auto* data = RE::TESDataHandler::GetSingleton();
            auto* form = data ? data->LookupForm(kFirstSlotForm + static_cast<RE::TESFormID>(slot), "ManualReloadF.esp") : nullptr;
            return form ? form->As<RE::TESObjectMISC>() : nullptr;
        }

        void load() noexcept
        {
            if (g_loaded) {
                return;
            }
            g_loaded = true;
            std::ifstream in(kSlotsPath);
            int read = 0;
            for (std::string line; std::getline(in, line);) {
                std::istringstream words(line);
                std::string head, key;
                int slot = -1;
                if (!(words >> head >> slot >> key) || (head != "kind" && head != "own") || slot < 0 || slot >= kSlotCount) {
                    continue;
                }
                if ((head == "kind") != (slot < kClassSlots)) {
                    continue;
                }
                rocklink::Pose pose{};
                bool ok = true;
                for (float& v : pose.rotate) ok = ok && static_cast<bool>(words >> v);
                for (float& v : pose.translate) ok = ok && static_cast<bool>(words >> v);
                if (!ok) {
                    continue;
                }
                g_slots[slot].key = key;
                g_slots[slot].hold = pose;
                g_slots[slot].hasHold = true;
                ++read;
            }
            note("SLOTS: %d saved magazine holds read from %s", read, kSlotsPath);
        }

        bool save() noexcept
        {
            std::ofstream out(kSlotsPath, std::ios::trunc);
            if (!out) {
                return false;
            }
            out << "; True Reload: the holds of magazines of other mods (written by the stick click; ROCK takes each one on its own form)\n";
            out << "; kind N <magazine that set it> rotate(9) translate(3) = a kind of magazine; own N <magazine> ... = that magazine alone\n";
            for (int i = 0; i < kSlotCount; ++i) {
                const auto& s = g_slots[i];
                if (!s.hasHold) {
                    continue;
                }
                out << (i < kClassSlots ? "kind " : "own ") << i << ' ' << s.key;
                char num[24];
                for (const float v : s.hold.rotate) {
                    std::snprintf(num, sizeof(num), " %.5f", v);
                    out << num;
                }
                for (const float v : s.hold.translate) {
                    std::snprintf(num, sizeof(num), " %.4f", v);
                    out << num;
                }
                out << '\n';
            }
            return static_cast<bool>(out);
        }

        // the kind of magazine by its box in the gun (x across, y along the gun, z up)
        int kindSlot(const std::string& key) noexcept
        {
            if (const auto it = g_kindOf.find(key); it != g_kindOf.end()) {
                return it->second;
            }
            float lo[3]{}, hi[3]{};
            int shape = 1, size = 1;
            float d[3]{ 0.0f, 0.0f, 0.0f };
            const bool measured = rocklink::partBox(0, lo, hi);
            if (measured) {
                for (int i = 0; i < 3; ++i) d[i] = std::max(0.0f, hi[i] - lo[i]);
                const float biggest = std::max({ d[0], d[1], d[2] });
                size = biggest < 7.0f ? 0 : biggest < 12.0f ? 1 : 2;
                if (d[0] > 2.5f && d[0] > 0.5f * std::max(d[1], d[2])) {
                    shape = 2;   // as wide as it is long or tall: a drum
                } else if (d[2] < 0.75f * d[1]) {
                    shape = 3;   // longer along the gun than it is tall: a box
                } else if (d[1] < 2.6f && d[2] < 9.0f) {
                    shape = 0;   // thin and short: a pistol's
                } else {
                    shape = 1;   // a rifle's stick or curved magazine
                }
            }
            const int slot = shape * 3 + size;
            g_kindOf[key] = slot;
            note("SLOTS: magazine '%s' is a %s %s one (box %.1f x %.1f x %.1f%s): kind slot %d", key.c_str(), kSizes[size], kShapes[shape], d[0], d[1],
                d[2], measured ? "" : ", not measured by ROCK", slot);
            return slot;
        }

        int ownSlot(const std::string& key) noexcept
        {
            for (int i = kClassSlots; i < kSlotCount; ++i) {
                if (g_slots[i].hasHold && g_slots[i].key == key) {
                    return i;
                }
            }
            return -1;
        }
    }

    int magSlotKind(const std::string& magModel, const char* formModel) noexcept
    {
        load();
        const auto key = magKey(magModel);
        int slot = ownSlot(key);
        if (slot < 0) {
            slot = kindSlot(key);
        }
        auto* misc = slotForm(slot);
        if (!misc || !formModel || !*formModel) {
            note("SLOTS: %s: the magazine is made from MRF_MagGeneric", !misc ? "no MRF_MagSlot forms in ManualReloadF.esp (an old esp?)" : "no model");
            return kGenericKind;
        }
        auto& s = g_slots[slot];
        if (s.model != formModel) {
            s.model = formModel;
            static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*misc)).SetModel(s.model.c_str());
        }
        detail("SLOTS: magazine '%s' made from slot %d (%s)", key.c_str(), slot, slot < kClassSlots ? "its kind's" : "its own");
        return kSlotKindBase + slot;
    }

    bool saveSlotHold(const std::string& magModel, std::uint32_t heldRef, int hand, std::string& said) noexcept
    {
        load();
        rocklink::Pose pose{};
        if (!heldRef || !rocklink::heldInHand(hand, heldRef, pose)) {
            said = "The hold is saved for this gun; ROCK does not hold the magazine now, so it is not passed to ROCK";
            note("SLOTS: the hold of %08X could not be read from ROCK (not held by that hand?)", heldRef);
            return false;
        }
        const auto key = magKey(magModel);
        int slot = ownSlot(key);
        const char* how = "its own hold, updated";
        if (slot < 0) {
            const int kind = kindSlot(key);
            if (!g_slots[kind].hasHold || g_slots[kind].key == key) {
                slot = kind;
                how = "the hold of every magazine of its kind";
            } else {
                for (int i = kClassSlots; i < kSlotCount && slot < 0; ++i) {
                    if (!g_slots[i].hasHold) {
                        slot = i;
                    }
                }
                how = "a hold of its own (its kind keeps the hold of another magazine)";
                if (slot < 0) {
                    said = "No room for another magazine's own hold (52 are taken)";
                    note("SLOTS: all own slots are taken: '%s' keeps its kind's hold", key.c_str());
                    return false;
                }
            }
        }
        auto& s = g_slots[slot];
        s.key = key;
        s.hold = pose;
        s.hasHold = true;
        auto* misc = slotForm(slot);
        const bool given = misc && rocklink::setDefaultHold(misc->GetFormID(), hand, pose);
        const bool written = save();
        note("SLOTS: '%s' -> slot %d, %s: root in the hand (%.2f %.2f %.2f); ROCK %s, file %s", key.c_str(), slot, how, pose.translate[0],
            pose.translate[1], pose.translate[2], given ? "took it" : "DID NOT take it", written ? "written" : "NOT written");
        char text[160];
        std::snprintf(text, sizeof(text), "Magazine hold saved: %s", slot < kClassSlots ? "for every magazine of this kind" : "for this magazine");
        said = text;
        return given && written;
    }

    void giveSlotHolds(int hand) noexcept
    {
        if (g_given || !rocklink::connected()) {
            return;
        }
        g_given = true;
        load();
        int given = 0;
        for (int i = 0; i < kSlotCount; ++i) {
            if (!g_slots[i].hasHold) {
                continue;
            }
            auto* misc = slotForm(i);
            if (misc && rocklink::setDefaultHold(misc->GetFormID(), hand, g_slots[i].hold)) {
                ++given;
            }
        }
        note("SLOTS: %d saved magazine holds given to ROCK", given);
    }
}
