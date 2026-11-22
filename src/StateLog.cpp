#include "StateLog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <deque>

#include "Runtime.h"
#include "Breech.h"
#include "Frame.h"
#include "Angles.h"
#include "Editor.h"
#include "GripHook.h"
#include "Gun.h"
#include "Hands.h"
#include "Log.h"
#include "Magazine.h"
#include "NiMath.h"
#include "Nodes.h"
#include "Objects.h"
#include "Palm.h"
#include "Paths.h"
#include "Playback.h"
#include "Rail.h"
#include "Recorder.h"
#include "RockLink.h"
#include "SafeMemory.h"
#include "ScriptBridge.h"
#include "Settings.h"
#include "State.h"
#include "Zones.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    std::string nodeState(RE::NiAVObject* node) noexcept
    {
        if (!looksLikeObject(node)) {
            return "missing";
        }
        int hiddenUp = 0;
        for (auto* at = node->parent; looksLikeObject(at) && hiddenUp < 12; at = at->parent) {
            if (at->flags.flags & 1ull) {
                char text[96];
                std::snprintf(text, sizeof(text), "HIDDEN by parent '%s'", nodeName(at).c_str());
                return text;
            }
            ++hiddenUp;
        }
        return (node->flags.flags & 1ull) ? "HIDDEN" : "shown";
    }

    // One line per thing: the magazine, the slide, the free hand, the real magazines around.
    // The stick click with a magazine in the free hand: the way it is held now is how it comes from the belt from now on
    // (zones file "hold": where the magazine stands against the controller). Returns false with no magazine in the hand.

    bool saveHold(const Gun& gun) noexcept
    {
        auto& st = g_state;
        const auto& free = g_hands[freeIndex()];
        if (!gun.weapon || !free.valid || !free.grip) {
            return false;
        }
        bool found = false;
        RE::NiTransform node{};
        if (st.place == kHand) {
            node = compose(free.wand, st.magInWand);
            found = true;
        }
        for (const auto& [id, loose] : g_loose) {
            if (found) {
                break;
            }
            auto* ref = lookupRef(id);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            if (looksLikeObject(mag3D) && loose.kind == st.kind && distance(mag3D->worldBound.center, free.palm) < g_settings.handRadius + 4.0f) {
                node = compose(mag3D->world, inverse(st.looseInNode));
                found = true;
            }
        }
        if (!found) {
            return false;
        }
        const auto hold = holdAgainst(free.wand, node);
        if (!squareRotation(hold)) {
            note("HOLD: not saved, the magazine's turn could not be read (a node squashed flat)");
            return false;
        }
        st.holdInWand = hold;
        st.holdInWandKnown = true;
        // this magazine's own line (hold@<model>), and the gun's line for its other magazines that have none yet
        const auto variant = holdKeyOf(st.magModel);
        bool ok = true;
        if (!variant.empty()) {
            ok = writeHoldLine(gun.weapon->GetFormID(), variant.c_str(), st.holdInWand);
        }
        if (!st.holdVariant || variant.empty()) {
            ok = writeHoldLine(gun.weapon->GetFormID(), "hold", st.holdInWand) && ok;
        }
        return ok;
    }

    void writeState(const char* why, const Gun& gun, RE::NiAVObject* root, RE::NiAVObject* magazine, RE::NiAVObject* bolt) noexcept
    {
        const auto& st = g_state;
        const auto& free = g_hands[freeIndex()];
        note("[%s] magazine: %s, %.1f out of %.1f%s%s, %u rounds; gun ammoCount %u, chamber %s", why, placeName(st.place), st.s, st.length,
            st.autoHold ? ", held out by the button" : "", st.resting ? ", resting half in" : "", st.rounds, gun.data ? gun.data->ammoCount : 0u,
            st.chamber ? "loaded" : "empty");
        if (looksLikeObject(magazine) && looksLikeObject(root)) {
            const auto at = inGun(root, magazine->world.translate);
            const auto want = st.magRest.translate + st.axis * st.s;
            note("[%s]   magazine node %s, at (%.1f %.1f %.1f) in the gun, %.1f from where it should be", why, nodeState(magazine).c_str(), at.x, at.y,
                at.z, distance(at, want));
        } else {
            note("[%s]   magazine node missing", why);
        }
        if (looksLikeObject(bolt) && looksLikeObject(root)) {
            const auto at = inGun(root, bolt->world.translate);
            note("[%s] slide: pulled %.2f of %.1f%s%s%s%s, node %s at (%.1f %.1f %.1f)", why, st.rackPull, g_settings.rackTravel,
                st.racking ? ", TAKEN by the grip" : "", st.pushing ? ", pushed by the open palm" : "", st.slideLocked ? ", locked open" : "",
                st.boltAction ? ", bolt action" : "", nodeState(bolt).c_str(), at.x, at.y, at.z);
        }
        const bool onMag = looksLikeObject(root) && palmOnMagazine(root, free.palm);
        note("[%s] free hand: %s, grip %s (%s), palm on the magazine %d, wants %d, ROCK holds %08X (on magazine %d, on slide %d, on gun %d)", why,
            free.valid ? "tracked" : "NOT TRACKED", free.grip ? "closed" : "open", griphook::owned() ? "hidden from ROCK" : "seen by ROCK", onMag ? 1 : 0,
            st.armReason, rocklink::heldBy(freeIndex()), g_rock.onMagazine ? 1 : 0, g_rock.onBolt ? 1 : 0, g_rock.onGun ? 1 : 0);
        if (st.handToken || st.handRef) {
            note("[%s] real magazine for the hand: token %d (spare %d, let go %d), ref %08X, ROCK took it %d", why, st.handToken, st.handSpare ? 1 : 0,
                st.handTokenDropped ? 1 : 0, st.handRef, st.grabDone ? 1 : 0);
        }
        for (const auto& [id, loose] : g_loose) {
            auto* ref = lookupRef(id);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            note("[%s] real magazine %08X: %u rounds, %s, 3D %s, %.0f from the free hand", why, id, loose.rounds,
                !ref ? "GONE" : (ref->IsDisabled() ? "DISABLED" : "enabled"), looksLikeObject(mag3D) ? nodeState(mag3D).c_str() : "NONE",
                looksLikeObject(mag3D) && free.valid ? distance(mag3D->world.translate, free.palm) : -1.0f);
        }
    }

    // A short fingerprint of the state: when it changes the whole state is written down.

    std::string stateKey() noexcept
    {
        const auto& st = g_state;
        char text[160];
        std::snprintf(text, sizeof(text), "%d|%d|%d|%u|%d|%d|%d|%d|%d|%d|%d|%u|%d", st.place, st.resting ? 1 : 0, st.autoHold ? 1 : 0, st.rounds,
            st.chamber ? 1 : 0, st.racking ? 1 : 0, st.slideLocked ? 1 : 0, st.rackPull > 0.5f ? 1 : 0, st.rackPull > g_settings.rackTravel - 0.5f ? 1 : 0,
            g_rock.onMagazine ? 1 : 0, g_rock.onBolt ? 1 : 0, st.handRef, st.grabDone ? 1 : 0);
        return text;
    }
}
