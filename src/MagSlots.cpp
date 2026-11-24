#include "MagSlots.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "Gun.h"
#include "Hands.h"
#include "NiMath.h"
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
            rocklink::Pose hold{};   // the magazine's root against the hand (as it was saved: where it should be)
            std::string    model;    // what its form wears now
            bool           sent{ false };            // ..what ROCK was given for it this session (in ROCK's own frame)
            RE::NiTransform given{};
            std::chrono::steady_clock::time_point sentAt{};
        };

        std::array<Slot, kSlotCount> g_slots;
        std::unordered_map<std::string, int> g_kindOf;   // magazine -> its kind slot (told once, by its size)
        bool g_loaded{ false };
        bool g_given{ false };
        int g_lastSlot{ -1 };   // the slot the last magazine was made from

        // ROCK's grab frame (its proxy, not the hand) against the hand, measured from a grab that used a hold we gave:
        // ROCK takes a first hold in that frame, so a hold saved against the hand is turned into it
        bool g_proxyKnown{ false };
        RE::NiTransform g_proxyInHand{};

        RE::NiTransform toNi(const rocklink::Pose& p) noexcept
        {
            RE::NiTransform t = identityTransform();
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    t.rotate.entry[r][c] = p.rotate[r * 3 + c];
                }
            }
            t.translate = RE::NiPoint3{ p.translate[0], p.translate[1], p.translate[2] };
            t.scale = 1.0f;
            return t;
        }

        rocklink::Pose toPose(const RE::NiTransform& t) noexcept
        {
            rocklink::Pose p{};
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    p.rotate[r * 3 + c] = t.rotate.entry[r][c];
                }
            }
            p.translate[0] = t.translate.x;
            p.translate[1] = t.translate.y;
            p.translate[2] = t.translate.z;
            return p;
        }

        // what ROCK is given for a hold saved against the hand
        RE::NiTransform forRock(const rocklink::Pose& hold) noexcept
        {
            const auto want = toNi(hold);
            return g_proxyKnown ? compose(inverse(g_proxyInHand), want) : want;
        }

        float angleBetween(const RE::NiTransform& a, const RE::NiTransform& b) noexcept
        {
            float trace = 0.0f;
            for (int i = 0; i < 3; ++i) {
                trace += dot(axisOf(a, i), axisOf(b, i));
            }
            return std::acos(std::clamp((trace - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.2958f;
        }

        bool giveSlot(int slot, int hand) noexcept;

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
                if (line.rfind("proxy ", 0) == 0) {
                    std::istringstream p(line.substr(6));
                    rocklink::Pose pose{};
                    bool ok = true;
                    for (float& v : pose.rotate) ok = ok && static_cast<bool>(p >> v);
                    for (float& v : pose.translate) ok = ok && static_cast<bool>(p >> v);
                    if (ok) {
                        g_proxyInHand = toNi(pose);
                        g_proxyKnown = true;
                        note("SLOTS: ROCK's grab frame against the hand is known: (%.2f %.2f %.2f)", pose.translate[0], pose.translate[1], pose.translate[2]);
                    }
                    continue;
                }
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
            if (g_proxyKnown) {
                const auto p = toPose(g_proxyInHand);
                out << "proxy";
                char num[24];
                for (const float v : p.rotate) {
                    std::snprintf(num, sizeof(num), " %.5f", v);
                    out << num;
                }
                for (const float v : p.translate) {
                    std::snprintf(num, sizeof(num), " %.4f", v);
                    out << num;
                }
                out << '\n';
            }
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
        g_lastSlot = slot;
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
        const bool given = giveSlot(slot, hand);
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
            if (giveSlot(i, hand)) {
                ++given;
            }
        }
        note("SLOTS: %d saved magazine holds given to ROCK", given);
    }
}

namespace mrf
{
    namespace
    {
        bool giveSlot(int slot, int hand) noexcept
        {
            auto& s = g_slots[slot];
            auto* misc = slotForm(slot);
            if (!misc || !s.hasHold) {
                return false;
            }
            const auto rock = forRock(s.hold);
            if (!rocklink::setDefaultHold(misc->GetFormID(), hand, toPose(rock))) {
                return false;
            }
            s.sent = true;
            s.given = rock;
            s.sentAt = std::chrono::steady_clock::now();
            return true;
        }
    }

    void watchSlotHold(int hand, std::uint32_t heldRef) noexcept
    {
        static std::uint32_t seenRef = 0;
        static std::chrono::steady_clock::time_point seenAt{};
        static bool measured = false;
        const auto now = std::chrono::steady_clock::now();
        if (heldRef != seenRef) {
            seenRef = heldRef;
            seenAt = now;
            measured = false;
        }
        if (measured || !heldRef || now - seenAt < std::chrono::milliseconds(450)) {
            return;
        }
        measured = true;
        // the slot the held magazine was made from, by its base form
        auto* first = slotForm(0);
        auto* objRef = lookupRef(heldRef);
        const auto* base = objRef ? objRef->GetObjectReference() : nullptr;
        const int slot = first && base ? static_cast<int>(base->GetFormID()) - static_cast<int>(first->GetFormID()) : -1;
        if (slot < 0 || slot >= kSlotCount) {
            return;
        }
        g_lastSlot = slot;
        auto& s = g_slots[slot];
        if (!s.hasHold || !s.sent || s.sentAt > seenAt) {
            return;   // this grab did not use a hold we gave
        }
        rocklink::Pose held{};
        if (!rocklink::heldInHand(hand, heldRef, held)) {
            return;
        }
        const auto actual = toNi(held);
        const auto want = toNi(s.hold);
        const float off = std::sqrt(dot(actual.translate - want.translate, actual.translate - want.translate));
        const float turn = angleBetween(actual, want);
        // ROCK's frame against the hand: actual = proxy * given
        g_proxyInHand = compose(actual, inverse(s.given));
        g_proxyKnown = true;
        note("SLOTS: slot %d held %.2f off and %.1f deg turned from its saved hold; ROCK's grab frame against the hand (%.2f %.2f %.2f)", g_lastSlot,
            off, turn, g_proxyInHand.translate.x, g_proxyInHand.translate.y, g_proxyInHand.translate.z);
        if (off > 0.4f || turn > 3.0f) {
            // the next grab comes where it was saved: every slot is given again in ROCK's frame as now measured
            int given = 0;
            for (int i = 0; i < kSlotCount; ++i) {
                if (g_slots[i].hasHold && giveSlot(i, hand)) {
                    ++given;
                }
            }
            save();
            note("SLOTS: %d holds given to ROCK again in its own grab frame", given);
        }
    }
}
