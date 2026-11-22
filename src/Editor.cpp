#include "Editor.h"

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
#include "StateLog.h"
#include "Angles.h"
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

    Editor g_ed;

    RE::NiPointer<RE::NiNode> g_dots[2];

    bool g_dotsTried{ false };

    std::vector<std::string> modesOf(const EditZone& z)
    {
        switch (z.type) {
        case 0: return { "move, stick = whole size", "size X (sideways)", "size Y (along the barrel)", "size Z (up)", "shift X (sideways)",
            "shift Y (along the barrel)", "shift Z (up)" };
        case 1: return { "move, stick = width", "move, stick = depth", "turn by hand (about the mouth)", "shift X (sideways)", "shift Y (along the barrel)",
            "shift Z (up)" };
        case 2: {
            std::vector<std::string> modes{ "move all", "turn by hand (about point 1)", "shift X (sideways)", "shift Y (along the barrel)", "shift Z (up)" };
            for (std::size_t i = 0; i < z.pts.size(); ++i) {
                modes.push_back("point " + std::to_string(i + 1) + (i == 0 ? " (rest)" : ""));
            }
            return modes;
        }
        default: return { "move", "shift X (sideways)", "shift Y (along the barrel)", "shift Z (up)" };
        }
    }

    // the mode moves the zone along one axis of the gun with the stick: 0 X, 1 Y, 2 Z; -1 not such a mode

    int shiftAxisOf(const EditZone& z, int mode) noexcept
    {
        const int first = z.type == 0 ? 4 : z.type == 1 ? 3 : z.type == 2 ? 2 : 1;
        return mode >= first && mode < first + 3 ? mode - first : -1;
    }

    std::string zonesPathOf(std::uint32_t weapon)
    {
        char path[160];
        std::snprintf(path, sizeof(path), "Data/F4SE/Plugins/ManualReloadF_zones/%08X.txt", weapon);
        return path;
    }

    bool loadEditZones(std::uint32_t weapon) noexcept
    {
        auto& ed = g_ed;
        ed.zones.clear();
        ed.keep.clear();
        std::ifstream in(zonesPathOf(weapon));
        if (!in) {
            return false;
        }
        int well = -1;
        for (std::string line; std::getline(in, line);) {
            std::istringstream words(line);
            std::string head, word;
            if (!(words >> head) || head[0] == ';') {
                continue;
            }
            if (line.rfind("slide route", 0) == 0) {
                continue;   // read in the second pass below
            }
            if (head == "hold" || head == "roundhold") {
                ed.keep.push_back(line);
                continue;
            }
            EditZone z;
            if (head == "magazine" || head == "slide" || head == "box") {
                std::string name = head;
                if (head == "box") {
                    words >> name;
                }
                if (words >> word >> z.lo.x >> z.lo.y >> z.lo.z >> word >> z.hi.x >> z.hi.y >> z.hi.z) {
                    z.type = 0;
                    z.head = head;
                    z.name = name;
                    ed.zones.push_back(z);
                }
            } else if (head == "well") {
                std::string what;
                words >> what;
                if (well < 0) {
                    z.type = 1;
                    z.head = "well";
                    z.name = "well (magazine well mouth)";
                    ed.zones.push_back(z);
                    well = static_cast<int>(ed.zones.size()) - 1;
                }
                auto& w = ed.zones[well];
                if (what == "side") {
                    words >> w.side >> word >> w.ahead;
                } else if (what == "mouth") {
                    words >> w.mouth.x >> w.mouth.y >> w.mouth.z >> word >> w.axis.x >> w.axis.y >> w.axis.z;
                }
            } else if (head == "socket") {
                z.type = 1;
                z.head = "socket";
                words >> z.name;
                while (words >> word) {
                    if (word == "mouth") {
                        words >> z.mouth.x >> z.mouth.y >> z.mouth.z;
                    } else if (word == "axis") {
                        words >> z.axis.x >> z.axis.y >> z.axis.z;
                    } else if (word == "side") {
                        words >> z.side;
                    } else if (word == "ahead") {
                        words >> z.ahead;
                    }
                }
                ed.zones.push_back(z);
            } else if (head == "route") {
                z.type = 2;
                z.head = "route";
                words >> z.name;
                float x, y, zz;
                while (words >> x >> y >> zz) {
                    z.pts.push_back(RE::NiPoint3{ x, y, zz });
                }
                if (!z.pts.empty()) {
                    ed.zones.push_back(z);
                }
            } else if (head == "point") {
                z.type = 3;
                z.head = "point";
                words >> z.name >> z.p.x >> z.p.y >> z.p.z;
                ed.zones.push_back(z);
            }
        }
        // "slide route x y z ..." (the slide's way, first point at rest)
        std::ifstream again(zonesPathOf(weapon));
        for (std::string line; std::getline(again, line);) {
            std::istringstream words(line);
            std::string head, second;
            if (words >> head >> second && head == "slide" && second == "route") {
                EditZone z;
                z.type = 2;
                z.head = "slide route";
                z.name = "slide_route";
                float x, y, zz;
                while (words >> x >> y >> zz) {
                    z.pts.push_back(RE::NiPoint3{ x, y, zz });
                }
                if (!z.pts.empty()) {
                    ed.zones.push_back(z);
                }
            }
        }
        // the point of the magazine the well catches: always there to move, from the measured top if not written yet
        const bool hasTop = std::any_of(ed.zones.begin(), ed.zones.end(), [](const EditZone& z) { return z.type == 3 && z.name == "magtop"; });
        if (!hasTop && g_state.railKnown) {
            const auto& st = g_state;
            const auto& r = st.magRest;
            EditZone z;
            z.type = 3;
            z.head = "point";
            z.name = "magtop";
            z.p = st.catchPointKnown ? compose(r, pointTransform(st.catchPointInNode)).translate :
                                       r.translate + (axisOf(r, 0) * st.sideInNode.x + axisOf(r, 1) * st.sideInNode.y + axisOf(r, 2) * st.topAt) * r.scale;
            ed.zones.push_back(z);
        }
        // where the palm holds a magazine from the belt: always there to move
        const bool hasHold = std::any_of(ed.zones.begin(), ed.zones.end(), [](const EditZone& z) { return z.type == 3 && z.name == "maghold"; });
        if (!hasHold && g_state.railKnown) {
            EditZone z;
            z.type = 3;
            z.head = "point";
            z.name = "maghold";
            z.p = g_state.holdPointKnown ? g_state.holdPointInGun : defaultHoldPoint();
            ed.zones.push_back(z);
        }
        // a part that swings open (barrels, breech): its hinge, to put where the model's hinge is
        const bool hasHinge = std::any_of(ed.zones.begin(), ed.zones.end(), [](const EditZone& z) { return z.type == 3 && z.name == "hinge"; });
        if (!hasHinge) {
            for (const auto& r : ed.zones) {
                if (r.type == 2 && (r.name == "barrels" || r.name == "breech") && r.pts.size() >= 2) {
                    EditZone z;
                    z.type = 3;
                    z.head = "point";
                    z.name = "hinge";
                    std::vector<float> at(r.pts.size(), 0.0f);
                    for (std::size_t i = 1; i < at.size(); ++i) {
                        at[i] = static_cast<float>(i) / static_cast<float>(at.size() - 1);
                    }
                    const auto fit = fitArc(r.pts, at);
                    z.p = fit.arc ? fit.pivot : r.pts.front();
                    ed.zones.push_back(z);
                    break;
                }
            }
        }
        return true;
    }

    bool saveEditZones(std::uint32_t weapon) noexcept
    {
        const auto path = zonesPathOf(weapon);
        std::error_code ignored;
        std::filesystem::create_directories("Data/F4SE/Plugins/ManualReloadF_zones", ignored);
        const auto backup = path.substr(0, path.size() - 4) + ".bak";
        if (!std::filesystem::exists(backup, ignored) && std::filesystem::exists(path, ignored)) {
            std::filesystem::copy_file(path, backup, ignored);
        }
        std::ofstream out(path);
        if (!out) {
            return false;
        }
        char text[2048];
        std::snprintf(text, sizeof(text), "; ManualReloadF zones of weapon %08X, edited in game\n", weapon);
        out << text;
        for (const auto& line : g_ed.keep) {
            out << line << "\n";
        }
        for (const auto& z : g_ed.zones) {
            if (z.type == 0) {
                const std::string head = z.head == "box" ? "box " + z.name : z.head;
                std::snprintf(text, sizeof(text), "%s lo %.3f %.3f %.3f hi %.3f %.3f %.3f\n", head.c_str(), z.lo.x, z.lo.y, z.lo.z, z.hi.x, z.hi.y, z.hi.z);
                out << text;
            } else if (z.type == 1 && z.head == "well") {
                std::snprintf(text, sizeof(text), "well side %.3f ahead %.3f\nwell mouth %.3f %.3f %.3f axis %.4f %.4f %.4f\n", z.side, z.ahead, z.mouth.x, z.mouth.y,
                    z.mouth.z, z.axis.x, z.axis.y, z.axis.z);
                out << text;
            } else if (z.type == 1) {
                std::snprintf(text, sizeof(text), "socket %s mouth %.3f %.3f %.3f axis %.4f %.4f %.4f side %.3f ahead %.3f\n", z.name.c_str(), z.mouth.x, z.mouth.y,
                    z.mouth.z, z.axis.x, z.axis.y, z.axis.z, z.side, z.ahead);
                out << text;
            } else if (z.type == 2) {
                out << (z.head == "slide route" ? std::string("slide route") : "route " + z.name);
                for (const auto& q : z.pts) {
                    std::snprintf(text, sizeof(text), " %.3f %.3f %.3f", q.x, q.y, q.z);
                    out << text;
                }
                out << "\n";
            } else {
                std::snprintf(text, sizeof(text), "point %s %.3f %.3f %.3f\n", z.name.c_str(), z.p.x, z.p.y, z.p.z);
                out << text;
            }
        }
        return static_cast<bool>(out);
    }

    RE::NiNode* dotsNode(int which) noexcept
    {
        if (!g_dotsTried) {
            g_dotsTried = true;
            const char* paths[2]{ "ManualReloadF\\DotsBlue.nif", "ManualReloadF\\DotsYellow.nif" };
            for (int i = 0; i < 2; ++i) {
                RE::BSModelDB::DBTraits::ArgsType args{};
                args.prepareAfterLoad = 1;
                args.loadTextures = 1;
                RE::NiPointer<RE::NiNode> node;
                const auto error = RE::BSModelDB::Demand(paths[i], &node, args);
                const auto count = node ? node->GetRuntimeData().children.size() : 0;
                note("EDITOR: model %s -> error %u, %s (%u dots)", paths[i], static_cast<unsigned>(error), node ? "loaded" : "not loaded", static_cast<unsigned>(count));
                g_dots[i] = node;
            }
        }
        return g_dots[which].get();
    }

    void hideDots() noexcept
    {
        for (auto& cloud : g_dots) {
            if (cloud) {
                cloud->flags.flags |= 1ull;
            }
        }
    }

    void showDots(RE::NiAVObject* root, int which, const std::vector<RE::NiPoint3>& at) noexcept
    {
        auto* cloud = dotsNode(which);
        auto* parent = looksLikeObject(root) ? root->IsNode() : nullptr;
        if (!cloud || !parent) {
            return;
        }
        if (cloud->parent != parent) {
            if (auto* old = cloud->parent; old && looksLikeObject(old)) {
                old->DetachChild(cloud);
            }
            parent->AttachChild(cloud, true);
            note("EDITOR: dots attached to the gun (%s)", which ? "yellow" : "blue");
        }
        cloud->local.MakeIdentity();
        cloud->flags.flags &= ~1ull;
        cloud->world = compose(root->world, cloud->local);
        auto& kids = cloud->GetRuntimeData().children;
        const std::size_t count = std::min<std::size_t>(kids.size(), 1024);
        for (std::size_t i = 0; i < count; ++i) {
            auto* dot = kids[i].get();
            if (!dot) {
                continue;
            }
            if (i < at.size()) {
                dot->local.MakeIdentity();
                dot->local.translate = at[i];
                dot->flags.flags &= ~1ull;
                dot->world = compose(cloud->world, dot->local);
                dot->worldBound.center = dot->world.translate;
                dot->worldBound.fRadius = 0.5f;
                if (i % 16 == 0) {
                    reachAncestors(cloud, dot->world.translate);
                }
            } else {
                dot->flags.flags |= 1ull;
            }
        }
        cloud->worldBound.center = root->world.translate;
        cloud->worldBound.fRadius = 150.0f;
    }

    void dotLine(std::vector<RE::NiPoint3>& out, const RE::NiPoint3& a, const RE::NiPoint3& b, float step, int most)
    {
        const int n = std::clamp(static_cast<int>(distance(a, b) / step) + 1, 1, most);
        for (int i = 0; i <= n; ++i) {
            out.push_back(a + (b - a) * (static_cast<float>(i) / static_cast<float>(n)));
        }
    }

    void dotCluster(std::vector<RE::NiPoint3>& out, const RE::NiPoint3& c, float r)
    {
        out.push_back(c);
        const RE::NiPoint3 d[6]{ { r, 0, 0 }, { -r, 0, 0 }, { 0, r, 0 }, { 0, -r, 0 }, { 0, 0, r }, { 0, 0, -r } };
        for (const auto& o : d) {
            out.push_back(c + o);
        }
    }

    void sideAxes(const RE::NiPoint3& axis, RE::NiPoint3& u, RE::NiPoint3& v) noexcept
    {
        const RE::NiPoint3 helper = std::fabs(axis.z) < 0.9f ? RE::NiPoint3{ 0, 0, 1 } : RE::NiPoint3{ 1, 0, 0 };
        u = normalized(cross(axis, helper));
        v = cross(axis, u);
    }

    void zoneDots(const EditZone& z, std::vector<RE::NiPoint3>& out, bool chosen, int mode)
    {
        if (z.type == 0) {
            RE::NiPoint3 c[8];
            for (int i = 0; i < 8; ++i) {
                c[i] = { (i & 1) ? z.hi.x : z.lo.x, (i & 2) ? z.hi.y : z.lo.y, (i & 4) ? z.hi.z : z.lo.z };
            }
            for (int i = 0; i < 8; ++i) {
                for (int bit = 1; bit < 8; bit <<= 1) {
                    if (!(i & bit)) {
                        dotLine(out, c[i], c[i | bit], 0.7f, 14);
                    }
                }
            }
        } else if (z.type == 1) {
            RE::NiPoint3 u, v;
            const auto axis = normalized(z.axis);
            sideAxes(axis, u, v);
            // what the game uses: from the mouth out along the way out, as far as "ahead" (the catch)
            const float rings[3]{ 0.0f, z.ahead, z.ahead * 0.5f };
            for (const float along : rings) {
                const auto centre = z.mouth + axis * along;
                const int n = along == 0.0f ? 16 : 10;
                for (int k = 0; k < n; ++k) {
                    const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(n);
                    out.push_back(centre + (u * std::cos(a) + v * std::sin(a)) * z.side);
                }
            }
            for (int k = 0; k < 4; ++k) {
                const float a = 1.5707963f * static_cast<float>(k);
                const auto off = (u * std::cos(a) + v * std::sin(a)) * z.side;
                dotLine(out, z.mouth + off, z.mouth + axis * z.ahead + off, 0.8f, 10);
            }
            dotLine(out, z.mouth, z.mouth + axis * (z.ahead + 3.0f), 0.35f, 24);   // the way out
        } else if (z.type == 2) {
            for (std::size_t i = 1; i < z.pts.size(); ++i) {
                dotLine(out, z.pts[i - 1], z.pts[i], 0.45f, 24);
            }
            dotCluster(out, z.pts.front(), 0.35f);
            if (chosen && mode >= kRoutePointMode && mode - kRoutePointMode < static_cast<int>(z.pts.size())) {
                dotCluster(out, z.pts[mode - kRoutePointMode], 0.6f);
            }
        } else {
            dotCluster(out, z.p, 0.4f);
        }
    }

    void editorHud(const char* extra) noexcept
    {
        auto& ed = g_ed;
        if (ed.zones.empty()) {
            return;
        }
        const auto& z = ed.zones[ed.sel];
        const auto modes = modesOf(z);
        char text[256];
        const std::string label = z.name == "magtop" ? "magtop (magazine tip in the gun: this point enters the well)" :
                                  z.name == "maghold" ? "maghold (where the palm holds a magazine from the belt)" :
                                  z.name == "hinge" ? "hinge (axis the barrel/breech swings about)" : z.name;
        std::snprintf(text, sizeof(text), "Zone %d/%d: %s (%s). Mode: %s%s%s", ed.sel + 1, static_cast<int>(ed.zones.size()), label.c_str(), zoneTypeName(z.type),
            modes[std::clamp(ed.mode, 0, static_cast<int>(modes.size()) - 1)].c_str(), extra ? ". " : "", extra ? extra : "");
        RE::SendHUDMessage::ShowHUDMessage(text, nullptr, false, false);
        note("EDITOR: %s", text);
    }

    // moves the chosen zone by a step in the gun's frame (keyboard), or by the hand (grip)

    void editorMove(EditZone& z, int mode, const RE::NiPoint3& shift) noexcept
    {
        if (z.type == 0) {
            z.lo = z.lo + shift;
            z.hi = z.hi + shift;
        } else if (z.type == 1) {
            z.mouth = z.mouth + shift;
        } else if (z.type == 2) {
            if (mode >= kRoutePointMode && mode - kRoutePointMode < static_cast<int>(z.pts.size())) {
                z.pts[mode - kRoutePointMode] = z.pts[mode - kRoutePointMode] + shift;
            } else {
                for (auto& q : z.pts) {
                    q = q + shift;
                }
            }
        } else {
            z.p = z.p + shift;
        }
    }

    void editorSize(EditZone& z, int mode, float change) noexcept
    {
        if (z.type == 0) {
            const RE::NiPoint3 grow = mode == 1 ? RE::NiPoint3{ change, 0, 0 } : mode == 2 ? RE::NiPoint3{ 0, change, 0 } : mode == 3 ? RE::NiPoint3{ 0, 0, change } :
                                                                                                                  RE::NiPoint3{ change, change, change };
            z.lo = z.lo - grow * 0.5f;
            z.hi = z.hi + grow * 0.5f;
            z.lo.x = std::min(z.lo.x, z.hi.x - 0.1f);
            z.lo.y = std::min(z.lo.y, z.hi.y - 0.1f);
            z.lo.z = std::min(z.lo.z, z.hi.z - 0.1f);
        } else if (z.type == 1) {
            if (mode == 1) {
                z.ahead = std::max(0.2f, z.ahead + change);
            } else {
                z.side = std::max(0.2f, z.side + change * 0.5f);
            }
        }
    }

    void editorStop(bool save, const char* why) noexcept
    {
        auto& ed = g_ed;
        if (!ed.on) {
            return;
        }
        ed.on = false;
        ed.show = true;
        griphook::setEditing(false);
        hideDots();
        if (save && ed.dirty) {
            const bool ok = saveEditZones(ed.weapon);
            note("EDITOR: off (%s), zones of %08X %s", why, ed.weapon, ok ? "saved" : "NOT saved (the file could not be written)");
            hud(ok ? "Zones saved. The gun reloads them now." : "Could not write the zones file!");
            g_state.weapon = nullptr;   // read the zones again, as if the gun were drawn anew
        } else {
            note("EDITOR: off (%s), nothing changed", why);
            hud("Zone editor closed (nothing changed)");
        }
    }

    // true while the editor has the gun: the rest of the plugin waits

    bool zoneEditor(RE::PlayerCharacter* player, const Gun& gun, Clock::time_point now, float dt) noexcept
    {
        auto& ed = g_ed;
        const auto& free = g_hands[freeIndex()];
        const std::uint32_t weapon = gun.weapon ? gun.weapon->GetFormID() : 0;
        if (ed.on && (weapon != ed.weapon || menuOpen())) {
            editorStop(true, weapon != ed.weapon ? "the gun changed" : "a menu opened");
            return false;
        }
        // the stick click: held a second toggles the editor; a short click inside it changes the mode
        const bool click = free.valid && (free.buttons & buttonMask(g_settings.markButton)) != 0;
        bool shortClick = false;
        if (click && !ed.clickWas) {
            ed.clickDown = now;
            ed.clickLongDone = false;
        }
        // both sticks held: FRIK's own menu (its weapon repositioning), not the zone editor
        const auto& gunHand = g_hands[g_settings.gunHand];
        if (click && gunHand.valid && (gunHand.buttons & buttonMask(g_settings.markButton)) != 0) {
            ed.clickLongDone = true;
        }
        if (click && !ed.clickLongDone && now - ed.clickDown > std::chrono::milliseconds(1000)) {
            ed.clickLongDone = true;
            if (ed.on) {
                editorStop(true, "the stick held");
                ed.clickWas = click;
                return true;
            }
            if (weapon) {
                ed.on = true;
                ed.weapon = weapon;
                ed.sel = 0;
                ed.mode = 0;
                ed.dirty = false;
                ed.gripWas = false;
                if (!loadEditZones(weapon) || ed.zones.empty()) {
                    ed.zones.clear();
                    hud("This gun has no zones file (export one from Blender first)");
                    note("EDITOR: no zones for %08X", weapon);
                    ed.on = false;
                } else {
                    releaseHand();
                    griphook::setArmed(false);
                    griphook::setEditing(true);
                    note("EDITOR: on for %08X, %zu zones", weapon, ed.zones.size());
                    editorHud("Stick sideways = next zone, click = mode, grip = move, hold stick = save");
                }
            }
        }
        if (!click && ed.clickWas && !ed.clickLongDone && now - ed.clickDown < std::chrono::milliseconds(500)) {
            shortClick = true;
        }
        ed.clickWas = click;
        if (!ed.on) {
            if (ed.show) {
                auto* shown = weapon == ed.weapon ? weaponRoot(player) : nullptr;
                if (!looksLikeObject(shown) || menuOpen()) {
                    ed.show = weapon == ed.weapon && !menuOpen();
                    hideDots();
                } else {
                    std::vector<RE::NiPoint3> blue, yellow;
                    for (const auto& zone : ed.zones) {
                        zoneDots(zone, blue, false, 0);
                    }
                    if (now - g_liveTopAt < std::chrono::milliseconds(300)) {
                        dotCluster(yellow, inGun(shown, g_liveTop), 0.5f);
                    }
                    showDots(shown, 0, blue);
                    showDots(shown, 1, yellow);
                }
            }
            return false;
        }
        auto* root = weaponRoot(player);
        if (!looksLikeObject(root) || ed.zones.empty()) {
            return true;
        }
        ed.sel = std::clamp(ed.sel, 0, static_cast<int>(ed.zones.size()) - 1);
        auto& z = ed.zones[ed.sel];
        const int modeCount = static_cast<int>(modesOf(z).size());
        if (shortClick) {
            ed.mode = (ed.mode + 1) % modeCount;
            editorHud();
        }

        // the stick: which axis it is, once per controller
        const auto device = controllerOf(freeIndex());
        if (device != ed.stickDevice && g_vr && device != vr::k_unTrackedDeviceIndexInvalid) {
            ed.stickDevice = device;
            ed.stickAxis = 0;
            int pad = -1;
            for (int a = 0; a < 5; ++a) {
                vr::ETrackedPropertyError error{};
                const auto type = g_vr->GetInt32TrackedDeviceProperty(device, static_cast<vr::ETrackedDeviceProperty>(vr::Prop_Axis0Type_Int32 + a), &error);
                if (error == vr::TrackedProp_Success && type == vr::k_eControllerAxis_Joystick) {
                    ed.stickAxis = a;
                    pad = -2;
                    break;
                }
                if (error == vr::TrackedProp_Success && type == vr::k_eControllerAxis_TrackPad && pad == -1) {
                    pad = a;
                }
            }
            if (pad >= 0) {
                ed.stickAxis = pad;
            }
            note("EDITOR: the free hand's stick is axis %d", ed.stickAxis);
        }
        const auto stick = free.valid ? free.axes[std::clamp(ed.stickAxis, 0, 4)] : RE::NiPoint3{};
        if (!ed.stickSwitched && std::fabs(stick.x) > 0.75f) {
            ed.stickSwitched = true;
            const int n = static_cast<int>(ed.zones.size());
            ed.sel = (ed.sel + (stick.x > 0.0f ? 1 : n - 1)) % n;
            ed.mode = 0;
            editorHud();
        } else if (ed.stickSwitched && std::fabs(stick.x) < 0.3f) {
            ed.stickSwitched = false;
        }
        auto& cur = ed.zones[ed.sel];
        if (std::fabs(stick.y) > 0.25f && std::fabs(stick.x) < 0.5f) {
            const float rate = (std::fabs(stick.y) - 0.25f) / 0.75f * 4.0f;   // up to 4 units a second
            const float change = (stick.y > 0.0f ? rate : -rate) * dt;
            if (const int along = shiftAxisOf(cur, ed.mode); along >= 0) {
                RE::NiPoint3 shift{};
                (along == 0 ? shift.x : along == 1 ? shift.y : shift.z) = change;
                editorMove(cur, 0, shift);   // the whole zone
            } else {
                editorSize(cur, ed.mode, change);
            }
            ed.dirty = true;
        }

        // the grip: the chosen zone goes with the hand (turned too, in a turning mode)
        const bool grip = free.valid && free.grip;
        const auto hand = relative(root->world, free.wand);
        if (grip && ed.gripWas) {
            const bool turn = (cur.type == 1 && ed.mode == 2) || (cur.type == 2 && ed.mode == 1);
            if (turn) {
                const auto delta = compose(hand, inverse(ed.handPrev));
                const auto moved = [&](const RE::NiPoint3& q) { return compose(delta, pointTransform(q)).translate; };
                if (cur.type == 1) {
                    const auto tip = moved(cur.mouth + normalized(cur.axis));
                    cur.mouth = moved(cur.mouth);
                    cur.axis = normalized(tip - cur.mouth);
                } else {
                    for (auto& q : cur.pts) {
                        q = moved(q);
                    }
                }
            } else {
                editorMove(cur, ed.mode, hand.translate - ed.handPrev.translate);
            }
            ed.dirty = true;
        }
        if (grip && !ed.gripWas) {
            buzz(freeIndex(), 800);
        }
        ed.handPrev = hand;
        ed.gripWas = grip;

        // the number pad: 4/6 X, 8/2 Y, 9/3 Z, +/- size, 7/1 other zone, 5 mode, 0 save
        struct Key { int vk; int what; };
        constexpr int VK_NUMPAD0_ = 0x60;
        const Key keys[]{ { VK_NUMPAD0_ + 4, 0 }, { VK_NUMPAD0_ + 6, 1 }, { VK_NUMPAD0_ + 2, 2 }, { VK_NUMPAD0_ + 8, 3 }, { VK_NUMPAD0_ + 3, 4 },
            { VK_NUMPAD0_ + 9, 5 }, { 0x6D, 6 }, { 0x6B, 7 }, { VK_NUMPAD0_ + 7, 8 }, { VK_NUMPAD0_ + 1, 9 }, { VK_NUMPAD0_ + 5, 10 }, { VK_NUMPAD0_, 11 } };
        for (const auto& key : keys) {
            const bool down = griphook::keyDown(key.vk);
            bool fire = false;
            if (down && !ed.keyWas[key.what]) {
                fire = true;
                ed.keyNext[key.what] = now + std::chrono::milliseconds(350);
            } else if (down && key.what < 8 && now >= ed.keyNext[key.what]) {
                fire = true;
                ed.keyNext[key.what] = now + std::chrono::milliseconds(70);
            }
            ed.keyWas[key.what] = down;
            if (!fire) {
                continue;
            }
            constexpr float step = 0.1f;
            auto& k = ed.zones[ed.sel];
            switch (key.what) {
            case 0: editorMove(k, ed.mode, { -step, 0, 0 }); break;
            case 1: editorMove(k, ed.mode, { step, 0, 0 }); break;
            case 2: editorMove(k, ed.mode, { 0, -step, 0 }); break;
            case 3: editorMove(k, ed.mode, { 0, step, 0 }); break;
            case 4: editorMove(k, ed.mode, { 0, 0, -step }); break;
            case 5: editorMove(k, ed.mode, { 0, 0, step }); break;
            case 6: editorSize(k, ed.mode, -step); break;
            case 7: editorSize(k, ed.mode, step); break;
            case 8:
            case 9: {
                const int n = static_cast<int>(ed.zones.size());
                ed.sel = (ed.sel + (key.what == 9 ? 1 : n - 1)) % n;
                ed.mode = 0;
                editorHud();
                break;
            }
            case 10:
                ed.mode = (ed.mode + 1) % static_cast<int>(modesOf(k).size());
                editorHud();
                break;
            case 11:
                editorStop(true, "numpad 0");
                return true;
            }
            if (key.what < 8) {
                ed.dirty = true;
            }
        }

        // draw
        std::vector<RE::NiPoint3> blue, yellow;
        for (int i = 0; i < static_cast<int>(ed.zones.size()); ++i) {
            if (i != ed.sel) {
                zoneDots(ed.zones[i], blue, false, 0);
            }
        }
        zoneDots(ed.zones[ed.sel], yellow, true, ed.mode);
        showDots(root, 0, blue);
        showDots(root, 1, yellow);
        static Clock::time_point toldAt{};
        if (now - toldAt > std::chrono::seconds(5)) {
            toldAt = now;
            note("EDITOR: %zu blue dots, %zu yellow dots", blue.size(), yellow.size());
        }
        return true;
    }
}
