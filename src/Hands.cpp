#include "Hands.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Runtime.h"
#include "GripHook.h"
#include "Settings.h"
#include "Log.h"
#include "NiMath.h"
#include "SafeMemory.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    vr::IVRSystem* g_vr{ nullptr };

    vr::TrackedDeviceIndex_t controllerOf(int index) noexcept
    {
        if (!g_vr) {
            return vr::k_unTrackedDeviceIndexInvalid;
        }
        return g_vr->GetTrackedDeviceIndexForControllerRole(index == 0 ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
    }

    void buzz(int index, unsigned short microseconds) noexcept
    {
        const auto device = controllerOf(index);
        if (g_settings.haptics && device != vr::k_unTrackedDeviceIndexInvalid) {
            g_vr->TriggerHapticPulse(device, 0, microseconds);
        }
    }

    std::array<Hand, 2> g_hands;

    bool g_pinned{ false };
    bool g_palmFromController{ false };

    int freeIndex() noexcept { return 1 - g_settings.gunHand; }
    FRIKApiV2::Hand frikHand(int index) noexcept { return index == 0 ? FRIKApiV2::Hand::Right : FRIKApiV2::Hand::Left; }

    void readHand(int index) noexcept
    {
        auto& hand = g_hands[index];
        hand.previousGrip = hand.grip;
        hand.previousButtons = hand.buttons;
        hand.valid = false;
        hand.grip = false;
        hand.buttons = 0;
        hand.axes = {};
        const auto device = controllerOf(index);
        if (device != vr::k_unTrackedDeviceIndexInvalid) {
            vr::VRControllerState_t state{};
            if ((!g_settings.injectedInput && griphook::rawState(device, &state, sizeof(state))) || (g_vr && g_vr->GetControllerState(device, &state, sizeof(state)))) {
                hand.buttons = state.ulButtonPressed;
                for (int a = 0; a < 5; ++a) {
                    hand.axes[a] = RE::NiPoint3{ state.rAxis[a].x, state.rAxis[a].y, 0.0f };
                }
                hand.grip = (state.ulButtonPressed & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton))) != 0;
            }
        }
        if (index == freeIndex() && g_settings.triggerGrab) {
            // the reload is worked with the trigger: the real grip is left to ROCK (holding the gun, other things)
            const bool trigger = (hand.buttons & vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.triggerButton))) != 0;
            hand.grip = trigger && griphook::triggerLatched();
        }
        if (index == freeIndex()) {
            griphook::setDevice(device);
            griphook::setTriggerGrab(g_settings.triggerGrab, vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.triggerButton)));
            griphook::setGripMask(vr::ButtonMaskFromId(static_cast<vr::EVRButtonId>(g_settings.gripButton)));
            griphook::arbitrate(hand.grip);
        }
        const auto* frik = FRIKApiV2::inst;
        if (!frik || !frik->getTrackedHandTransform) {
            return;
        }
        const auto which = frikHand(index);
        RE::NiTransform wand;
        if (!frik->getTrackedHandTransform(which, FRIKApiV2::TrackedHandKind::Wand, &wand)) {
            return;
        }
        hand.wand = wand;
        hand.wand.scale = 1.0f;
        hand.wrist = hand.wand;
        hand.palm = wand.translate;
        FRIKApiV2::ArmChainTransforms chain{};
        chain.structSize = sizeof(chain);
        if (frik->getArmChain && frik->getIndexFingerTipPosition && frik->getArmChain(which, &chain) && (chain.validMask & (1u << 6))) {
            const auto tip = frik->getIndexFingerTipPosition(which);
            const auto wrist = chain.hand.translate;
            const float length = distance(tip, wrist);
            if (length < 40.0f && length > 0.1f) {
                hand.palm = (tip + wrist) * 0.5f;
                hand.wrist = chain.hand;
                hand.wrist.scale = 1.0f;
            }
        }
        // While we pin the drawn hand onto a part, the arm chain reports our own pin back, and a
        // grab that follows the palm then chases itself (the crank spinning, the gauss bolt
        // shaking).  So while pinned, the palm and the wrist come from the controller, offset as
        // they were the last time the hand was free.
        const bool pinnedHere = (g_pinned || g_palmFromController) && index == freeIndex();   // ..or ROCK holds it on a part
        if (pinnedHere && hand.inWandKnown) {
            hand.wrist = compose(hand.wand, hand.wristInWand);
            hand.palm = compose(hand.wand, pointTransform(hand.palmInWand)).translate;
        } else if (!pinnedHere) {
            hand.wristInWand = relative(hand.wand, hand.wrist);
            hand.palmInWand = relative(hand.wand, pointTransform(hand.palm)).translate;
            hand.inWandKnown = true;
        }
        hand.valid = true;
    }

    void hud(const char* text) noexcept
    {
        if (g_settings.messages && text) {
            RE::SendHUDMessage::ShowHUDMessage(text, nullptr, false, false);
        }
    }

    RE::TESObjectREFR* lookupRef(std::uint32_t id) noexcept
    {
        if (!id) {
            return nullptr;
        }
        auto* form = RE::TESForm::GetFormByID(id);
        return form ? form->As<RE::TESObjectREFR>() : nullptr;
    }

    bool g_posed{ false };
    bool g_posedPinch{ false };

    void pinHand(const RE::NiTransform& wrist) noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (!frik || !frik->setHandWorldTransform) {
            return;
        }
        auto target = wrist;
        target.scale = 1.0f;
        if (frik->setHandWorldTransform(kFrikTag, frikHand(freeIndex()), target, 120)) {
            g_pinned = true;
        }
    }

    // The hand on a turning part (a crank, a handle): the palm stays on the point of the part it took, the wrist
    // keeps the turn of the controller (so it is not wrung round, and does not swing out on a wider circle).

    RE::NiTransform handOnPart(const RE::NiTransform& wristNow, const RE::NiTransform& part, const RE::NiPoint3& gripInPart, const RE::NiPoint3& palmInWrist) noexcept
    {
        auto target = wristNow;
        target.scale = 1.0f;
        const auto grip = compose(part, pointTransform(gripInPart)).translate;
        const auto palmOffset = compose(target, pointTransform(palmInWrist)).translate - target.translate;
        target.translate = grip - palmOffset;
        return target;
    }

    namespace
    {
        using JoltClock = std::chrono::steady_clock;
        constexpr const char* kJoltTag = "ManualReloadF_Jolt";
        struct Jolt
        {
            bool               on{ false };
            JoltClock::time_point at{};
            RE::NiTransform    wristInWand{};   // the drawn hand on the controller as the jolt began (not read back while we move it)
            RE::NiPoint3       dirInWand{};     // the way it is knocked, in the controller's frame
            float              amplitude{ 0.0f };
        };
        Jolt g_jolt;
    }

    void startJolt(const RE::NiPoint3& worldDirection, float strength) noexcept
    {
        const auto& hand = g_hands[g_settings.gunHand];
        if (!g_settings.slideJolt || !hand.valid || !hand.inWandKnown || strength <= 0.0f) {
            return;
        }
        const float length = std::sqrt(dot(worldDirection, worldDirection));
        if (length < 1e-4f) {
            return;
        }
        const auto dir = worldDirection * (1.0f / length);
        if (!g_jolt.on) {
            g_jolt.wristInWand = hand.wristInWand;
        }
        g_jolt.dirInWand = relative(hand.wand, pointTransform(hand.wand.translate + dir)).translate;
        g_jolt.amplitude = g_settings.slideJoltDistance * std::clamp(strength, 0.0f, 2.0f);
        g_jolt.at = JoltClock::now();
        g_jolt.on = true;
    }

    void updateJolt() noexcept
    {
        if (!g_jolt.on) {
            return;
        }
        const auto* frik = FRIKApiV2::inst;
        const auto& hand = g_hands[g_settings.gunHand];
        const float t = std::chrono::duration<float>(JoltClock::now() - g_jolt.at).count();
        constexpr float rise = 0.025f;   // knocked out fast
        constexpr float back = 0.06f;    // and pulled back by the arm
        constexpr float end = 0.3f;
        if (t > end || !hand.valid || !frik || !frik->setHandWorldTransform) {
            if (frik && frik->clearHandWorldTransform) {
                frik->clearHandWorldTransform(kJoltTag, frikHand(g_settings.gunHand));
            }
            g_jolt.on = false;
            return;
        }
        const float f = t < rise ? t / rise : std::exp(-(t - rise) / back) * std::cos((t - rise) * 9.0f);
        auto target = compose(hand.wand, g_jolt.wristInWand);
        target.scale = 1.0f;
        const auto dir = compose(hand.wand, pointTransform(g_jolt.dirInWand)).translate - hand.wand.translate;
        target.translate = target.translate + dir * (g_jolt.amplitude * f);
        frik->setHandWorldTransform(kJoltTag, frikHand(g_settings.gunHand), target, 125);
        if (t * 1000.0f < static_cast<float>(g_settings.slideJoltBuzzMs)) {
            buzz(g_settings.gunHand, 3999);   // a thump, not a tick: a pulse every frame while it lasts
        }
    }

    void unpinHand() noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (g_pinned && frik && frik->clearHandWorldTransform) {
            frik->clearHandWorldTransform(kFrikTag, frikHand(freeIndex()));
        }
        g_pinned = false;
    }

    void poseFingers(bool on, bool pinch) noexcept
    {
        const auto* frik = FRIKApiV2::inst;
        if (!frik || (on == g_posed && (!on || pinch == g_posedPinch))) {
            return;
        }
        if (on && pinch && frik->setHandPoseCustom) {
            // 0 = curled, 1 = open
            FRIKApiV2::HandPoseData pose{};
            const float t = std::clamp(g_settings.pinchThumb, 0.0f, 1.0f);
            const float i = std::clamp(g_settings.pinchIndex, 0.0f, 1.0f);
            const float o = std::clamp(g_settings.pinchOthers, 0.0f, 1.0f);
            pose.thumb = { t, t, t, 0.0f };
            pose.index = { i, i, i, 0.0f };
            pose.middle = { o, o, o, 0.0f };
            pose.ring = { std::min(o + 0.05f, 1.0f), std::min(o + 0.05f, 1.0f), std::min(o + 0.05f, 1.0f), 0.0f };
            pose.pinky = { std::min(o + 0.1f, 1.0f), std::min(o + 0.1f, 1.0f), std::min(o + 0.1f, 1.0f), 0.0f };
            g_posed = frik->setHandPoseCustom(kFrikTag, frikHand(freeIndex()), pose, 120);
            g_posedPinch = g_posed;
            note("HAND: the handle is pinched (thumb and index), FRIK %s", g_posed ? "took it" : "refused");
        } else if (on && frik->setHandPoseCustom) {
            g_posedPinch = false;
            const float c = std::clamp(g_settings.fingerCurl, 0.0f, 1.0f);
            FRIKApiV2::HandPoseData pose{};
            const float t = std::min(c + 0.25f, 1.0f);
            pose.thumb = { t, t, t, 0.0f };
            pose.index = { c, c, c, 0.0f };
            pose.middle = { c, c, c, 0.0f };
            pose.ring = { c, c, c, 0.0f };
            pose.pinky = { c, c, c, 0.0f };
            g_posed = frik->setHandPoseCustom(kFrikTag, frikHand(freeIndex()), pose, 120);
        } else if (!on && frik->clearHandPose) {
            frik->clearHandPose(kFrikTag, frikHand(freeIndex()));
            g_posed = false;
        }
    }
}
