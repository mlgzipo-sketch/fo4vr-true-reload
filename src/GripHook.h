#pragma once

#include <cstdint>

// See GripHook.cpp.
namespace griphook
{
    using Log = void (*)(const char* text) noexcept;

    void install(Log log, bool hide) noexcept;    // once, when OpenVR is up
    bool installed() noexcept;
    void arbitrate(bool rawGrip) noexcept;        // the free hand's grip as it really is, once a frame
    void setArmed(bool armed) noexcept;           // the next press of the free hand's grip belongs to the plugin
    bool owned() noexcept;                        // this press belongs to the plugin (and is hidden)
    void setOwned(bool value) noexcept;
    void setDevice(std::uint32_t device) noexcept;
    void setGripMask(std::uint64_t mask) noexcept;
    void setAxis2IsGrip(bool value) noexcept;
    std::uint32_t hiddenCount() noexcept;
    void setEditing(bool value) noexcept;         // the zone editor: the free controller is hidden from the game
    bool keyDown(int virtualKey) noexcept;        // a key of the keyboard is held (zone editor)
    // bTriggerGrab: the free hand's trigger takes the things of the reload. A trigger press that begins where the
    // reload is (setTriggerZone: at the belt, at the magazine or the slide, or with one in the hand) is shown to everyone
    // as the grip (ROCK holds what the plugin puts in the hand) and the trigger itself is hidden (no flashlight).
    void setTriggerGrab(bool on, std::uint64_t triggerMask) noexcept;
    void setTriggerZone(bool atReload) noexcept;
    bool triggerLatched() noexcept;               // the trigger is down and works as the reload's grip
    // The controller as it really is. False when there is no hook: read it the usual way.
    bool rawState(std::uint32_t device, void* state, std::uint32_t size) noexcept;
}
