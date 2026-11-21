// ManualReloadF: The zone editor in game: move and size the zones of the gun in hand, save them to its zones file
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
    // ------------------------------------------------------------ the zone editor (in game)
    // With a gun drawn, hold the free hand's stick click for a second: the zones of that gun are drawn as dots
    // (blue, the chosen one yellow) and edited by hand or from the number pad. Hold it again: the zones are
    // written to ManualReloadF_zones/<weapon>.txt (the first time, the old file is kept as .bak) and read back.
    struct EditZone
    {
        int type{ 0 };          // 0 box, 1 socket, 2 route, 3 point
        std::string head;       // how it is written: magazine / slide / box / well / socket / slide route / route / point
        std::string name;
        RE::NiPoint3 lo{}, hi{}, mouth{}, axis{ 0.0f, 0.0f, -1.0f }, p{};
        float side{ 1.0f }, ahead{ 1.0f };
        std::vector<RE::NiPoint3> pts;
    };

    struct Editor
    {
        bool on{ false };
        std::uint32_t weapon{ 0 };
        std::vector<EditZone> zones;
        std::vector<std::string> keep;   // lines the editor does not edit (the magazine's hold): written back as they were
        int sel{ 0 }, mode{ 0 };
        bool gripWas{ false };
        RE::NiTransform handPrev{};
        Clock::time_point clickDown{};
        bool clickWas{ false }, clickLongDone{ false };
        bool stickSwitched{ false };
        int stickAxis{ -1 };
        std::uint32_t stickDevice{ vr::k_unTrackedDeviceIndexInvalid };
        std::array<Clock::time_point, 16> keyNext{};
        std::array<bool, 16> keyWas{};
        bool dirty{ false };
        bool show{ false };   // after the editor: the zones stay drawn while this gun is out, with the magazine's top live
    };

    extern Editor g_ed;

    extern RE::NiPointer<RE::NiNode> g_dots[2];

    extern bool g_dotsTried;

    inline const char* zoneTypeName(int type) noexcept
    {
        switch (type) {
        case 0: return "box";
        case 1: return "well";
        case 2: return "route";
        default: return "point";
        }
    }

    std::vector<std::string> modesOf(const EditZone& z);

    // the mode moves the zone along one axis of the gun with the stick: 0 X, 1 Y, 2 Z; -1 not such a mode
    int shiftAxisOf(const EditZone& z, int mode) noexcept;

    inline constexpr int kRoutePointMode = 5;   // route modes from here on: one point each

    std::string zonesPathOf(std::uint32_t weapon);

    bool loadEditZones(std::uint32_t weapon) noexcept;

    bool saveEditZones(std::uint32_t weapon) noexcept;

    RE::NiNode* dotsNode(int which) noexcept;

    void hideDots() noexcept;

    void showDots(RE::NiAVObject* root, int which, const std::vector<RE::NiPoint3>& at) noexcept;

    void dotLine(std::vector<RE::NiPoint3>& out, const RE::NiPoint3& a, const RE::NiPoint3& b, float step, int most);

    void dotCluster(std::vector<RE::NiPoint3>& out, const RE::NiPoint3& c, float r);

    void sideAxes(const RE::NiPoint3& axis, RE::NiPoint3& u, RE::NiPoint3& v) noexcept;

    void zoneDots(const EditZone& z, std::vector<RE::NiPoint3>& out, bool chosen, int mode);

    void editorHud(const char* extra = nullptr) noexcept;

    // moves the chosen zone by a step in the gun's frame (keyboard), or by the hand (grip)
    void editorMove(EditZone& z, int mode, const RE::NiPoint3& shift) noexcept;

    void editorSize(EditZone& z, int mode, float change) noexcept;

    void editorStop(bool save, const char* why) noexcept;

    // true while the editor has the gun: the rest of the plugin waits
    bool zoneEditor(RE::PlayerCharacter* player, const Gun& gun, Clock::time_point now, float dt) noexcept;
}
