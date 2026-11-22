#include "Frame.h"

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
#include "Angles.h"
#include "Breech.h"
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
#include "StateLog.h"
#include "Zones.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    // ------------------------------------------------------------ every frame
    // core2: a real magazine on its way into the free hand, or held there by ROCK. ROCK is asked again while the
    // grip is held and it has not taken it; let go, it falls (at the belt or over the shoulder it is put away).

    void realMagazineInHand(RE::PlayerCharacter* player) noexcept
    {
        auto& st = g_state;
        const auto& free = g_hands[freeIndex()];
        const auto now = Clock::now();
        if (st.handToken && st.handSpare && !(free.valid && free.grip) && !st.handTokenDropped) {
            st.handTokenDropped = true;   // let go before the spare came
            st.handStash = inBelt(player, free.palm) || overShoulder(player, free.palm);
        }
        if (!st.handRef) {
            return;
        }
        const auto held = rocklink::heldBy(freeIndex());
        if (held == st.handRef) {
            if (!st.grabDone) {
                st.grabDone = true;
                note("CORE2: ROCK holds magazine %08X in the palm (%.2f s, %d asks)", st.handRef,
                    std::chrono::duration<float>(now - st.handRefAt).count(), st.grabTries);
            }
            return;
        }
        if (st.grabDone) {
            // ROCK let it go: put away at the belt / over the shoulder, else it is just a magazine lying there
            if (inBelt(player, free.palm) || overShoulder(player, free.palm)) {
                const auto it = g_loose.find(st.handRef);
                const auto rounds = it != g_loose.end() ? it->second.rounds : 0u;
                deleteMagazine(st.handRef);
                char text[96]{};
                std::snprintf(text, sizeof(text), "Magazine stowed (%u rounds returned to inventory)", rounds);
                hud(text);
                playSound(4);
            }
            note("CORE2: magazine %08X is let go", st.handRef);
            st.handRef = 0;
            return;
        }
        // until ROCK has it, it stays where it came in the hand (it would drop for a moment and jump back up)
        if (st.handRefInWandValid && free.valid && free.grip) {
            auto* ref = lookupRef(st.handRef);
            if (looksLikeObject(ref ? ref->Get3D() : nullptr)) {
                const auto pose = toLinkPose(compose(free.wand, st.handRefInWand));
                rocklink::driveLoose(st.handRef, pose.rotate, pose.translate, 0.05f, "a magazine waiting for the hand");
            }
        }
        int failure = 0;
        const int state = rocklink::forceGrabState(&failure);
        // the grip read as open for a frame or two while the hand is handed over is not a let go
        static Clock::time_point gripLostAt{};
        if (free.valid && free.grip) {
            gripLostAt = Clock::time_point{};
        } else if (gripLostAt == Clock::time_point{}) {
            gripLostAt = now;
        }
        const bool letGo = gripLostAt != Clock::time_point{} && now - gripLostAt > std::chrono::milliseconds(200);
        if (letGo || now - st.handRefAt > std::chrono::milliseconds(2000)) {
            note("CORE2: ROCK did not take magazine %08X (grab state %d, failure %d, held %08X): it falls", st.handRef, state, failure, held);
            // let go before ROCK took it: it was dropped on purpose (a spent magazine thrown away), nothing is wrong
            // with this gun's magazines. Only a grip held all the way through and never answered says ROCK can't.
            // no gun is switched to drawn magazines for good any more: one miss (the hand still on the gun, a
            // grab asked mid-swap) made every later magazine of that gun hang drawn in the air. ROCK is asked anew next time.
            rocklink::cancelGrab();   // a request still queued would keep the magazine claimed: it would hang in the air, not to be taken
            st.handRef = 0;
            return;
        }
        // queued (1): ROCK is on it, asking again only answers "hand busy"; asked again after it was rejected or cancelled
        if (st.grabTries < 5 && now - st.grabTriedAt > std::chrono::milliseconds(250) && state != 1 && state != 2) {
            st.grabTriedAt = now;
            ++st.grabTries;
            detail("CORE2: asking ROCK again for magazine %08X (state %d, failure %d, held %08X)", st.handRef, state, failure, held);
            if (held != 0) {
                // ROCK still has the hand on the gun's magazine part (pulled out of the well): it lets go first
                const bool let = rocklink::releaseHeld(freeIndex(), held);
                detail("CORE2: ROCK is asked to let go of %08X first: %d", held, let ? 1 : 0);
            }
            auto* ref = lookupRef(st.handRef);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            const auto c = looksLikeObject(mag3D) ? palmGripPoint(mag3D, free.palm) : free.palm;
            const float middle[3]{ c.x, c.y, c.z };
            rocklink::forceGrab(freeIndex(), st.handRef, middle);
        }
    }

    // The gun is gone from the hand while its magazine was in play: the magazine goes back to the pouch.

    void putAway(const char* why) noexcept
    {
        auto& st = g_state;
        if (st.place == kRailAuto || st.place == kRailHand || st.place == kHand) {
            note("The magazine in play is put away (%s): %u rounds stay with the ammo you carry", why, st.rounds);
            st.rounds = 0;
            st.place = kGone;
        }
        releaseHand();
        griphook::setArmed(false);
        st.armReason = kArmNone;
    }

    std::uint64_t buttonMask(int id) noexcept
    {
        return vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(id));
    }

    bool menuOpen() noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (frik && frik->isWristPipboyOpen && frik->isWristPipboyOpen()) {
            return true;
        }
        if (frik && frik->isConfigOpen && frik->isConfigOpen()) {
            return true;
        }
        const auto* ui = RE::UI::GetSingleton();
        return ui && ui->menuMode != 0;
    }
}
