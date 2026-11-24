#include "Breech.h"

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
#include "StateLog.h"
#include "Zones.h"

namespace mrf
{
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;

    // ------------------------------------------------------------ revolvers and break actions
    // Turns the rows (the node's own axes as its parent sees them) about one of the parent's axes.

    void turnRows(RE::NiMatrix3& rotation, int axis, float angle) noexcept
    {
        const float c = std::cos(angle);
        const float sn = std::sin(angle);
        auto m = toMat(rotation);
        for (auto& row : m) {
            const float x = row[0], y = row[1], z = row[2];
            if (axis == 1) {
                row[0] = x * c + z * sn;
                row[2] = -x * sn + z * c;
            } else {
                row[1] = y * c - z * sn;
                row[2] = y * sn + z * c;
            }
        }
        rotation = fromMat(m);
    }

    // The laser musket: the free hand takes the crank's handle and winds it round; each full turn charges one more shot.
    // The node the handle (zones "route lever") hangs on: of WeaponBolt and WeaponExtra1..3, the one with a model
    // nearest to where the handle is drawn.

    RE::NiAVObject* leverNodeOf(RE::NiAVObject* root) noexcept
    {
        auto& st = g_state;
        if (!st.leverNodeChecked) {
            st.leverNodeChecked = true;
            RE::NiPoint3 where = st.leverPts.empty() ? RE::NiPoint3{} : st.leverPts.front();
            if (const auto box = st.zoneBoxes.find("lever"); box != st.zoneBoxes.end()) {
                where = (box->second.first + box->second.second) * 0.5f;
            }
            float best = 1e9f;
            for (const char* name : { "WeaponBolt", "WeaponExtra1", "WeaponExtra2", "WeaponExtra3" }) {
                auto* node = findNamed(root, name);
                if (!looksLikeObject(node)) {
                    continue;
                }
                float nearest = 1e9f;
                forEachNode(node, [&](RE::NiAVObject* object, int depth) {
                    if (depth > 0 && !object->IsNode()) {
                        const auto c = relative(root->world, pointTransform(object->worldBound.center)).translate;
                        nearest = std::min(nearest, distance(c, where));
                    }
                    return true;
                });
                note("LEVER: node %s, its model %.1f from the handle", name, nearest);
                if (nearest < best) {
                    best = nearest;
                    st.leverNodeName = name;
                }
            }
            note("LEVER: the handle hangs on %s", st.leverNodeName.empty() ? "nothing" : st.leverNodeName.c_str());
        }
        return st.leverNodeName.empty() ? nullptr : findNamed(root, st.leverNodeName.c_str());
    }

    // The handle on its way: turned about the drawn arc first, then moved along the rest of the way.

    void poseLever(RE::NiAVObject* root, RE::NiAVObject* node, float lever, const RE::NiTransform& restInRoot) noexcept
    {
        const auto& st = g_state;
        const auto& fit = st.leverFit;
        const float turnShare = fit.arc ? std::clamp(lever / std::max(fit.arcAt, 0.01f), 0.0f, 1.0f) : 0.0f;
        const float straightFrom = fit.arc ? fit.arcAt : 0.0f;
        const RE::NiPoint3 lin = lever > straightFrom ? alongWay(st.leverPts, st.leverAt, lever) - alongWay(st.leverPts, st.leverAt, straightFrom)
                                                      : RE::NiPoint3{};
        const auto posed = fit.arc ? turnedPose(restInRoot, fit.axis, fit.pivot, fit.angle * turnShare, lin)
                                   : turnedPose(restInRoot, RE::NiPoint3{ 0.0f, 0.0f, 1.0f }, RE::NiPoint3{}, 0.0f, lin);
        node->local = relative(node->parent->world, compose(root->world, posed));
        refreshSubtree(node, RE::NiPoint3{});
    }

    // A magazine gun with a handle (the gatling laser): pulled out, it throws the core out; put back home.

    void updateMagLever(const Gun& gun, RE::NiAVObject* root, Clock::time_point now) noexcept
    {
        auto& st = g_state;
        st.leverTouch = false;
        if (!st.leverKnown || !looksLikeObject(root)) {
            return;
        }
        auto* node = leverNodeOf(root);
        if (!looksLikeObject(node) || !looksLikeObject(node->parent)) {
            return;
        }
        if (!st.leverRestKnown) {
            if (now - st.equippedAt < std::chrono::milliseconds(700)) {
                return;
            }
            st.leverRestLocal = node->local;
            st.leverRestInRoot = relative(root->world, node->world);
            st.leverRestKnown = true;
        }
        const auto& spare = g_hands[freeIndex()];
        const auto palmIn = spare.valid ? relative(root->world, pointTransform(spare.palm)).translate : RE::NiPoint3{};
        st.leverTouch = spare.valid && distance(palmIn, alongWay(st.leverPts, st.leverAt, st.lever)) < 5.0f;
        const bool press = spare.valid && spare.grip && !spare.previousGrip;
        if (press && st.leverTouch && griphook::owned()) {
            st.leverHeld = true;
            buzz(freeIndex(), 600);
            note("LEVER: the free hand takes the handle at %.2f", st.lever);
        }
        if (st.leverHeld && !(spare.valid && spare.grip)) {
            st.leverHeld = false;
            note("LEVER: let go at %.2f", st.lever);
        }
        if (st.leverHeld) {
            const float before = st.lever;
            st.lever = std::clamp(nearestOnWay(st.leverPts, st.leverAt, palmIn, nullptr), before - 0.12f, before + 0.12f);
            if (st.lever >= 0.95f && before < 0.95f) {
                buzz(freeIndex(), 900);
                if (st.place == kSeated && st.railKnown) {
                    startEject(gun, false, now);   // the handle all the way out: the core comes out
                }
            }
            if (st.lever > 0.2f) {
                st.leverArmed = true;
            }
            if (st.lever <= 0.02f && before > 0.0f && st.leverArmed) {
                st.leverArmed = false;
                st.lever = 0.0f;
                buzz(freeIndex(), 900);
                playSound(3);
                note("LEVER: home");
            }
        }
        if (st.lever > 0.0f) {
            poseLever(root, node, st.lever, st.leverRestInRoot);
            st.leverDrawn = true;
        } else if (st.leverDrawn) {
            node->local = st.leverRestLocal;
            refreshSubtree(node, RE::NiPoint3{});
            st.leverDrawn = false;
        }
    }

    void updateCrank(RE::PlayerCharacter* player, const Gun& gun, RE::NiAVObject* root, Clock::time_point now) noexcept
    {
        auto& st = g_state;
        const auto& spare = g_hands[freeIndex()];
        rocklink::Frame link{};
        link.gun = true;
        link.freeHand = freeIndex();
        link.magazine.node = kMovingParts[0];
        link.bolt.node = kMovingParts[1];
        g_rock = rocklink::frame(link);

        auto* crank = findNamed(root, kMovingParts[0]);
        if (!looksLikeObject(crank) || !looksLikeObject(crank->parent)) {
            return;
        }
        if (!st.crankRestKnown) {
            if (now - st.equippedAt < std::chrono::milliseconds(700)) {
                return;
            }
            st.crankRest = relative(root->world, crank->world);
            st.crankRestKnown = true;
            note("Crank: rest pose remembered, %u of %u charges", gun.data->ammoCount, capacityOf(gun));
        }
        const auto inRoot = [&](const RE::NiPoint3& p) { return relative(root->world, pointTransform(p)).translate; };
        const auto& axis = st.crankAxis;
        const auto e1 = normalized((st.crankStart - st.crankCenter) - axis * dot(st.crankStart - st.crankCenter, axis));
        const auto e2 = cross(axis, e1);
        const auto handAngle = [&](const RE::NiPoint3& p) {
            const auto v = inRoot(p) - st.crankCenter;
            return std::atan2(dot(v, e2), dot(v, e1));
        };
        const auto handle = rotateAbout(st.crankStart - st.crankCenter, axis, st.crankAngle) + st.crankCenter;
        const float scale = gunScale(root);
        const bool onHandle = spare.valid && distance(inRoot(spare.palm), handle) < 4.5f;
        const bool press = spare.valid && spare.grip && !spare.previousGrip;
        if (!spare.grip) {
            st.armReason = onHandle ? kArmBlock : kArmNone;
            griphook::setArmed(onHandle);   // the grip on the handle is ours, not a second-hand hold
        }
        if (press && onHandle && griphook::owned()) {
            st.crankHeld = true;
            st.crankPrev = handAngle(spare.palm);
            buzz(freeIndex(), 700);
            detail("Crank: the free hand takes the handle");
        }
        if (st.crankHeld && !(spare.valid && spare.grip)) {
            st.crankHeld = false;
            detail("Crank: let go after %.0f deg", st.crankTurned * 57.29578f);
        }
        if (st.crankHeld) {
            const float a = handAngle(spare.palm);
            float turn = a - st.crankPrev;
            while (turn > 3.14159265f) turn -= 6.2831853f;
            while (turn < -3.14159265f) turn += 6.2831853f;
            st.crankPrev = a;
            st.crankAngle += turn;
            st.crankTurned += std::fabs(turn);
            if (std::fabs(turn) > 0.15f) {
                buzz(freeIndex(), 300);
            }
            if (st.crankTurned >= 6.2831853f) {
                st.crankTurned -= 6.2831853f;
                const auto capacity = std::max(capacityOf(gun), 1u);
                const auto inventory = gun.data->ammo ? inventoryCount(player, gun.data->ammo) : 0u;
                if (gun.data->ammoCount < capacity && inventory > gun.data->ammoCount) {
                    gun.data->ammoCount += 1;
                    playSound(0);
                    buzz(freeIndex(), 1500);
                    buzz(g_settings.gunHand, 1500);
                    note("CRANK: one more charge, %u of %u", gun.data->ammoCount, capacity);
                } else {
                    hud(inventory <= gun.data->ammoCount ? "No cells left" : "Fully loaded");
                    note("CRANK: a full turn, nothing to charge (%u of %u, %u cells)", gun.data->ammoCount, capacity, inventory);
                }
            }
        }
        const auto posed = turnedPose(st.crankRest, axis, st.crankCenter, st.crankAngle, RE::NiPoint3{});
        crank->local = relative(crank->parent->world, compose(root->world, posed));
        refreshSubtree(crank, RE::NiPoint3{});
        // the hand stays on the handle it turns
        if (st.crankHeld && spare.valid) {
            if (!st.crankPinned) {
                st.crankPinned = true;
                st.wristInCrank = relative(crank->world, spare.wrist);
                st.gripInCrank = relative(crank->world, pointTransform(spare.palm)).translate;
                auto wrist = spare.wrist;
                wrist.scale = 1.0f;
                st.palmInWristCrank = relative(wrist, pointTransform(spare.palm)).translate;
            }
            pinHand(handOnPart(spare.wrist, crank->world, st.gripInCrank, st.palmInWristCrank));
            poseFingers(true);
        } else if (st.crankPinned) {
            st.crankPinned = false;
            unpinHand();
            poseFingers(false);
        }
        (void)scale;
    }

    // writes "<key> x y z r00 .. r22" (a pose against the controller) into the gun's zones file, replacing the old one

    bool writeHoldLine(std::uint32_t weapon, const char* key, const RE::NiTransform& hold) noexcept
    {
        const auto path = zonesPathOf(weapon);
        const std::string prefix = std::string(key) + " ";
        std::vector<std::string> lines;
        {
            std::ifstream in(path);
            for (std::string line; std::getline(in, line);) {
                if (line.rfind(prefix, 0) != 0) {
                    lines.push_back(line);
                }
            }
        }
        char text[512];
        std::snprintf(text, sizeof(text), "%s %.4f %.4f %.4f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f", key, hold.translate.x, hold.translate.y, hold.translate.z,
            hold.rotate.entry[0][0], hold.rotate.entry[0][1], hold.rotate.entry[0][2], hold.rotate.entry[1][0], hold.rotate.entry[1][1], hold.rotate.entry[1][2],
            hold.rotate.entry[2][0], hold.rotate.entry[2][1], hold.rotate.entry[2][2]);
        lines.insert(lines.begin() + (lines.empty() || lines[0].rfind(";", 0) != 0 ? 0 : 1), text);
        std::error_code ignored;
        std::filesystem::create_directories("Data/F4SE/Plugins/ManualReloadF_zones", ignored);
        std::ofstream out(path);
        for (const auto& line : lines) {
            out << line << "\n";
        }
        note("HOLD: saved to %s: %s", path.c_str(), text);
        return static_cast<bool>(out);
    }

    // a pose against the controller, its rotation made square (a scaled node leaves its scale in the rows)

    RE::NiTransform holdAgainst(const RE::NiTransform& wandIn, const RE::NiTransform& node) noexcept
    {
        auto wand = wandIn;
        wand.scale = 1.0f;
        auto hold = relative(wand, node);
        hold.scale = 1.0f;
        auto x = axisOf(hold, 0);
        auto y = axisOf(hold, 1);
        if (dot(x, x) < 1e-4f) {
            x = normalized(cross(y, axisOf(hold, 2)));
        }
        x = normalized(x);
        y = y - x * dot(y, x);
        if (dot(y, y) < 1e-4f) {
            y = cross(axisOf(hold, 2), x);   // two rows alike (a node squashed flat): rebuilt from the third
        }
        y = normalized(y);
        setRows(hold, x, y, cross(x, y));
        return hold;
    }

    // The reload button opens and closes the gun. Open, with the muzzle up, everything falls out of it. Rounds
    // come from the belt in the free hand (a full clip for a revolver, one shell at a time for a break action)
    // and go in at the breech. The free hand on the gun, or a jerk of the gun, closes it.

    void updateBreech(RE::PlayerCharacter* player, const Gun& gun, RE::NiAVObject* root, Clock::time_point now, float dt) noexcept
    {
        auto& st = g_state;
        auto& b = st.breech;
        const auto& spare = g_hands[freeIndex()];
        const auto& firing = g_hands[g_settings.gunHand];
        hideRound();
        looseMagazines(nullptr, nullptr);
        rocklink::Frame link{};
        link.gun = true;
        link.freeHand = freeIndex();
        const char* partName = b.mech == 4 ? "WeaponExtra1" : kMovingParts[0];   // the part that opens
        link.magazine.node = partName;
        link.bolt.node = kMovingParts[1];
        if (b.drawn) {
            link.magazine.drive = true;   // ROCK's collision of the opening part goes where it is drawn
            link.magazine.parentLocal = toLinkPose(b.drawnLocal);
        }
        g_rock = rocklink::frame(link);

        auto* mag = findNamed(root, partName);
        if (!looksLikeObject(mag) || !looksLikeObject(mag->parent)) {
            return;
        }
        const bool revolver = b.mech == 1;
        const bool single = b.mech == 3;   // a mini nuke / a missile: nothing is left in it after the shot
        const bool cellCover = b.mech == 4;   // alien blaster: one cell is all its rounds
        const bool capacitors = b.mech == 5;  // gamma gun: three capacitors, then it is loaded
        auto* cellNode = cellCover ? findNamed(root, kMovingParts[0]) : nullptr;
        const bool played = !cellCover && !capacitors && gun.weapon && loadPlayed(gun.weapon->GetFormID());
        const bool zoneHinge = !played && st.zoneHinge.arc;   // no recording: the turn drawn in Blender
        if (zoneHinge) {
            g_play.axis = st.zoneHinge.axis;
            g_play.pivot = st.zoneHinge.pivot;
            g_play.openAngle = st.zoneHinge.angle;
        }
        const bool hinged = played || zoneHinge;
        std::array<RE::NiAVObject*, 5> child{};
        for (int i = 0; i < 5; ++i) {
            char name[32]{};
            std::snprintf(name, sizeof(name), "WeaponMagazineChild%d", i + 1);
            child[i] = findNamed(mag, name);
        }
        const auto gunState = static_cast<std::uint32_t>(player->gunState);
        const auto reloading = static_cast<std::uint32_t>(g_settings.reloadGunState);
        const bool reloadStarted = gunState == reloading && st.lastGunState != reloading;
        if (gunState != st.lastGunState) {
            detail("gunState %u -> %u", st.lastGunState, gunState);
        }
        if (reloadStarted) {
            b.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
        }
        st.lastGunState = gunState;
        const auto capacity = std::max(capacityOf(gun), 1u);
        const float scale = gunScale(root);

        if (!st.dumped && now - st.equippedAt > std::chrono::seconds(2)) {
            st.dumped = true;
            logNote("Weapon node tree:");
            dumpTree(root);
        }

        // the closed pose
        if (!b.known) {
            const bool idle = gunState == static_cast<std::uint32_t>(g_settings.idleGunState) && now - st.equippedAt > std::chrono::milliseconds(700);
            if (!idle) {
                return;
            }
            b.rest = mag->local;
            b.kids.clear();
            forEachNode(mag, [&](RE::NiAVObject* object, int depth) {
                if (depth > 0 && object->IsNode() && b.kids.size() < 40) {
                    b.kids.emplace_back(object, object->local);
                }
                return true;
            });
            b.known = true;
            b.restInRoot = relative(root->world, mag->world);
            if (looksLikeObject(cellNode)) {
                b.cellRest = cellNode->local;
                b.cellRestKnown = true;
            }
            b.caps = capacitors ? 3u : 0u;
            if (st.leverKnown) {
                if (auto* bolt = leverNodeOf(root); looksLikeObject(bolt) && looksLikeObject(bolt->parent)) {
                    b.leverRestLocal = bolt->local;
                    b.leverRestInRoot = relative(root->world, bolt->world);
                    b.leverRestKnown = true;
                }
            }
            if (!st.zonesChecked && gun.weapon) {
                st.zonesChecked = true;
                zonesFor(gun.weapon->GetFormID(), false);
            }
            b.live = gun.data->ammoCount;
            b.lastAmmo = b.live;
            note("Breech: closed pose remembered (%zu nodes below), %u of %u rounds in it", b.kids.size(), b.live, capacity);
        }

        // the game's own reload: its animation moves the opening part off where it stands closed (in VR the gun's
        // state does not always say "reloading"), or at the latest the game refills the gun
        bool gameReloaded = false;
        if (!b.open && b.t <= 0.0f && gunState != 0 && now - b.toggled > std::chrono::milliseconds(800) && now >= b.freezeUntil) {
            const float moved = distance(mag->local.translate, b.rest.translate);
            const float turned = 1.0f - std::min({ dot(axisOf(mag->local, 0), axisOf(b.rest, 0)), dot(axisOf(mag->local, 1), axisOf(b.rest, 1)),
                                                 dot(axisOf(mag->local, 2), axisOf(b.rest, 2)) });
            if (moved > 0.3f || turned > 0.003f) {
                gameReloaded = true;
                detail("The game moves the opening part (%.2f off, turn %.4f): its reload began", moved, turned);
            }
        }

        // the rounds
        auto ammo = gun.data->ammoCount;
        const bool leverOut = st.leverKnown && b.lever > 0.05f;
        if (b.open) {
            if (ammo != 0) {
                gun.data->ammoCount = 0;   // an open gun does not fire, and the game's own refill is undone
            }
            b.lastAmmo = 0;
        } else if (leverOut) {
            if (ammo != 0) {
                gun.data->ammoCount = 0;   // the handle is pulled out: no shot until it is put back
            }
            b.leverBlocked = true;
        } else {
            if (b.leverBlocked) {
                b.leverBlocked = false;   // the handle is home again: the round in it can be fired
                gun.data->ammoCount = b.live;
                ammo = b.live;
                b.lastAmmo = b.live;
            }
            if (ammo > b.lastAmmo && g_settings.blockReload) {
                note("Game refill %u -> %u reverted", b.lastAmmo, ammo);
                ammo = b.lastAmmo;
                gun.data->ammoCount = ammo;
                gameReloaded = true;
            } else if (ammo < b.lastAmmo) {
                b.spent = single || capacitors ? 0u : cellCover ? (ammo == 0 ? 1u : 0u) : std::min(b.spent + (b.lastAmmo - ammo), capacity - std::min(ammo, capacity));
                detail("Shot: %u rounds and %u fired cases in the gun", ammo, b.spent);
            }
            b.live = ammo;
            b.lastAmmo = ammo;
        }
        st.lastAmmo = gun.data->ammoCount;

        const auto close = [&](const char* how) {
            b.open = false;
            b.held = 0;
            b.toggled = now;
            gun.data->ammoCount = b.live;
            b.lastAmmo = b.live;
            st.lastAmmo = b.live;
            playSound(3);
            buzz(g_settings.gunHand, 2000);
            note("CLOSE (%s): %u rounds, %u fired cases", how, b.live, b.spent);
        };

        // the reload button: a tap opens or closes
        const bool buttonNow = (firing.buttons & buttonMask(g_settings.reloadButton)) != 0;
        const bool buttonBefore = (firing.previousButtons & buttonMask(g_settings.reloadButton)) != 0;
        if (buttonNow && !buttonBefore) {
            st.buttonSince = now;
        }
        if (!buttonNow && buttonBefore && now - st.buttonSince < std::chrono::milliseconds(600) && now - b.toggled > std::chrono::milliseconds(400) &&
            !menuOpen()) {
            if (b.open) {
                close("the button");
            } else {
                b.open = true;
                b.opened = now;
                b.toggled = now;
                gun.data->ammoCount = 0;
                b.lastAmmo = 0;
                playSound(1);
                buzz(g_settings.gunHand, 1200);
                note("OPEN: %u rounds, %u fired cases", b.live, b.spent);
                if (capacitors) {
                    // the three capacitors drop out: what was left in them goes back to the pouch
                    ejectCasings(gun, root, mag->world.translate, axisOf(root->world, 2) * -1.0f, b.caps, "the gamma gun's capacitors");
                    b.caps = 0;
                    b.live = 0;
                    b.spent = 0;
                }
            }
        } else if ((reloadStarted || gameReloaded) && !b.open && b.known) {
            // the game's own reload began (an empty gun reloads by itself): held shut, the hand opens it
            if (now >= b.freezeUntil) {
                note("The game's reload of the closed gun is held still: open it by hand");
            }
            b.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(std::max(g_settings.freezeSeconds, 1.0f) * 1000.0f));
        }
        // the reload button opens and closes this gun by hand: the game, seeing it open (no rounds), would also start its own
        // reload on the same press and play it (the double barrel, 0.50). Cut short while open and just after the button.
        if (g_settings.blockReload && gunState == reloading && (b.open || now - b.toggled < std::chrono::milliseconds(1200))) {
            player->gunState = static_cast<RE::GUN_STATE>(g_settings.idleGunState);
            if (reloadStarted) {
                note("The game's reload of a gun opened by hand is cut short (gunState %u -> %d)", gunState, g_settings.idleGunState);
            }
        }
        // a break action's barrels (or a launcher's breech) hang on a hinge: gravity and the gun's own jerks swing them;
        // they fall open freely, and only a pull shut stronger than fHingeHold (a flick) carries them shut
        const bool swinging = hinged && !revolver && !single && !cellCover && !capacitors && b.open && b.known && g_play.openAngle > 0.1f;
        if (!b.driving && !b.pushing && swinging) {
            const float range = g_play.openAngle;
            float theta = b.t * range;
            RE::NiPoint3 heavy = b.restInRoot.translate + RE::NiPoint3{ 0.0f, 10.0f, 0.0f };
            for (const char* name : { "barrels", "breech" }) {
                if (const auto it = st.zoneBoxes.find(name); it != st.zoneBoxes.end()) {
                    heavy = (it->second.first + it->second.second) * 0.5f;
                    break;
                }
            }
            const auto arm = rotateAbout(heavy - g_play.pivot, g_play.axis, theta);
            const float arm2 = std::max(dot(arm, arm), 4.0f);
            b.accel = b.accel * 0.75f + (b.velocity - b.lastVelocity) * (0.25f / dt);
            RE::NiPoint3 accel = b.accel;
            const float a2 = dot(accel, accel);
            if (a2 > 6000.0f * 6000.0f) {
                accel = accel * (6000.0f / std::sqrt(a2));
            }
            const RE::NiPoint3 pull = RE::NiPoint3{ 0.0f, 0.0f, -686.0f } - accel;   // gravity, and the gun moving away under it
            const RE::NiPoint3 pullInGun{ dot(axisOf(root->world, 0), pull), dot(axisOf(root->world, 1), pull), dot(axisOf(root->world, 2), pull) };
            float alpha = dot(cross(arm, pullInGun), g_play.axis) / arm2;
            if (g_settings.hingeHold > 0.0f && alpha < 0.0f && alpha > -g_settings.hingeHold && b.omega > -0.5f) {
                alpha = 0.0f;   // the hinge's stiffness holds them open
            }
            b.omega = (b.omega + alpha * dt) * std::exp(-1.5f * dt);
            theta += b.omega * dt;
            if (theta >= range) {
                theta = range;
                if (b.omega > 0.0f) {
                    if (b.omega > 2.0f) buzz(g_settings.gunHand, 500);
                    b.omega = -b.omega * 0.2f;   // a small bounce at the stop
                }
            }
            if (theta <= 0.03f * range) {
                if (b.omega < -2.5f && now - b.opened > std::chrono::milliseconds(400)) {
                    theta = 0.0f;
                    b.t = 0.0f;
                    b.omega = 0.0f;
                    close("swung shut");
                } else {
                    theta = 0.03f * range;
                    b.omega = std::max(b.omega, 0.0f);
                }
            }
            if (b.open) {
                b.t = theta / range;
            }
        } else if (!b.driving && !b.pushing && single && !st.leverKnown && b.open) {
            // a launcher opened by the button only cracks open: the hand lifts it the rest of the way by its handle
            if (b.t < 0.15f) {
                b.t = approach(b.t, 0.15f, 3.0f * dt);
            }
            b.omega = 0.0f;
        } else if (!b.driving && !b.pushing) {
            b.t = approach(b.t, b.open ? 1.0f : 0.0f, 7.0f * dt);
            b.omega = 0.0f;
        } else {
            b.omega = 0.0f;
        }
        b.lastVelocity = b.velocity;

        // the pose: ours while it is open or moving, and held still while the game plays its own reload
        if (b.t > 0.0f || gunState == reloading || now < b.freezeUntil) {
            mag->local = b.rest;
            if (b.t > 0.0f) {
                const float eased = b.t * b.t * (3.0f - 2.0f * b.t);
                if (!hinged && !capacitors) {
                    turnRows(mag->local.rotate, revolver ? 1 : 0, (revolver ? g_settings.cylinderSwing : g_settings.breakAngle) * 0.01745329f * eased);
                }
            }
            for (auto& [object, local] : b.kids) {
                if (looksLikeObject(object)) {
                    object->local = local;
                }
            }
            refreshSubtree(mag, RE::NiPoint3{});
            if (played) {
                const float eased = (b.driving || swinging) ? b.t : b.t * b.t * (3.0f - 2.0f * b.t);
                posePlayed(root, eased);
            } else if (zoneHinge && b.t > 0.0f) {
                // turned about the hinge drawn in Blender, from where it stands closed
                const float eased = (b.driving || swinging) ? b.t : b.t * b.t * (3.0f - 2.0f * b.t);
                const auto posed = turnedPose(b.restInRoot, st.zoneHinge.axis, st.zoneHinge.pivot, st.zoneHinge.angle * eased, RE::NiPoint3{});
                mag->local = relative(mag->parent->world, compose(root->world, posed));
                refreshSubtree(mag, RE::NiPoint3{});
            }
            b.drawnLocal = mag->local;
            b.drawn = b.t > 0.0f;
        } else {
            b.drawn = false;
        }

        // the gun's speed against the player (a jerk closes it)
        const auto at = root->world.translate - player->GetPosition();
        if (b.prevKnown) {
            b.velocity = b.velocity * 0.5f + (at - b.prevAt) * (0.5f / dt);
        }
        b.prevAt = at;
        b.prevKnown = true;

        const auto barrel = cellCover || capacitors ? axisOf(root->world, 1) : axisOf(mag->world, 1);   // the cylinder's / the barrels' own line, forward
        const auto breechNode = cellCover && looksLikeObject(cellNode) ? cellNode : looksLikeObject(child[0]) ? child[0] : mag;
        const auto breechAt = breechNode->world.translate - (revolver ? barrel * (2.0f * scale) : RE::NiPoint3{});
        // cases out of the chambers drawn in the zones file (point chamberN, the gun closed): each out of its own tunnel,
        // along the barrel; with none drawn, out of the breech side by side
        const auto ejectFromChambers = [&](std::uint32_t count, const char* why) {
            if (st.chambers.empty() || cellCover || capacitors) {
                ejectCasings(gun, root, breechAt, barrel * -1.0f, count, why);
                return;
            }
            std::vector<RE::NiPoint3> points;
            for (std::uint32_t i = 0; i < count && i < st.chambers.size(); ++i) {
                const auto inPart = relative(b.restInRoot, pointTransform(st.chambers[i])).translate;
                points.push_back(compose(mag->world, pointTransform(inPart)).translate);
            }
            ejectCasingsFrom(gun, root, points, barrel * -1.0f, true, why);
        };
        const bool fullyOpen = b.open && b.t >= (swinging || single ? 0.6f : 0.95f);   // hanging barrels need not be all the way down
        // zones (Blender): the palm against the opening part, taken back to where the part stands closed
        const auto partZone = [&]() -> const std::pair<RE::NiPoint3, RE::NiPoint3>* {
            for (const char* name : { "cylinder", "barrels", "breech" }) {
                const auto it = st.zoneBoxes.find(name);
                if (it != st.zoneBoxes.end()) {
                    return &it->second;
                }
            }
            return nullptr;
        }();
        const auto palmOnPart = [&](const RE::NiPoint3& palmWorld) {
            const auto inPart = relative(mag->world, pointTransform(palmWorld));
            return compose(b.restInRoot, inPart).translate;
        };
        // the whole hand, not one point: wrist, palm, fingers, fingertips
        const auto fingers = spare.palm - spare.wrist.translate;
        const std::array<RE::NiPoint3, 4> handPoints{ spare.wrist.translate, spare.palm, spare.palm + fingers * 0.8f, spare.palm + fingers * 1.5f };
        const auto handIn = [&](const std::pair<RE::NiPoint3, RE::NiPoint3>& box, float margin, RE::NiPoint3* where) {
            for (const auto& point : handPoints) {
                const auto at = palmOnPart(point);
                if (inBox(box.first, box.second, at, margin)) {
                    if (where) *where = at;
                    return true;
                }
            }
            return false;
        };
        const auto touchesPart = [&](float fallback) {
            if (!spare.valid) {
                return false;
            }
            if (partZone) {
                return handIn(*partZone, g_settings.contactMargin, nullptr);
            }
            return distance(spare.palm, mag->world.translate) < fallback * scale;
        };

        // the latch (zones file: box latch): a closed break action opens only after the free hand pressed it;
        // until then a hand on the barrels just holds the gun (ROCK's two-handed hold)
        const auto latchBox = st.zoneBoxes.find("latch");
        const bool hasLatch = latchBox != st.zoneBoxes.end() && !revolver;
        if (hasLatch && !b.open && spare.valid && handIn(latchBox->second, g_settings.contactMargin, nullptr)) {
            if (now - b.unlatchedAt > std::chrono::milliseconds(1500)) {
                buzz(freeIndex(), 700);
                detail("The latch is pressed");
            }
            b.unlatchedAt = now;
        }
        const bool unlatched = (!hasLatch || now - b.unlatchedAt < std::chrono::milliseconds(1500)) && (!st.leverKnown || b.lever > 0.9f);

        // the handle (zones file "route lever", the Fat Man): pulled back along its way it opens the breech,
        // pushed home it closes it; while it is out the gun does not fire
        bool leverTouch = false;
        if (st.leverKnown && b.leverRestKnown && b.known) {
            auto* bolt = leverNodeOf(root);
            const auto palmInRoot = spare.valid ? relative(root->world, pointTransform(spare.palm)).translate : RE::NiPoint3{};
            const auto handleAt = alongWay(st.leverPts, st.leverAt, b.lever);
            leverTouch = spare.valid && b.held == 0 && distance(palmInRoot, handleAt) < 6.0f;
            const bool grab = spare.valid && spare.grip && !spare.previousGrip;
            if (grab && griphook::owned() && leverTouch && !b.driving) {
                b.leverHeld = true;
                buzz(freeIndex(), 600);
                detail("The free hand takes the handle at %.2f", b.lever);
            }
            if (b.leverHeld && !(spare.valid && spare.grip)) {
                b.leverHeld = false;
                detail("The handle is let go at %.2f", b.lever);
            }
            if (b.leverHeld) {
                float away = 0.0f;
                const float want = nearestOnWay(st.leverPts, st.leverAt, palmInRoot, &away);
                const float before = b.lever;
                b.lever = std::clamp(want, before - 0.12f, before + 0.12f);
                if (b.lever >= 0.95f && before < 0.95f) {
                    buzz(freeIndex(), 900);
                    if (!b.open) {
                        b.open = true;   // the handle all the way back: the breech opens
                        b.opened = now;
                        b.toggled = now;
                        gun.data->ammoCount = 0;
                        b.lastAmmo = 0;
                        playSound(1);
                        buzz(g_settings.gunHand, 1200);
                        note("OPEN (the handle): %u rounds", b.live);
                    }
                }
                if (b.lever > 0.2f) {
                    b.leverArmed = true;
                }
                if (b.lever <= 0.02f && before > 0.0f && b.leverArmed) {
                    b.leverArmed = false;
                    b.lever = 0.0f;
                    buzz(freeIndex(), 900);
                    playSound(3);
                    note("The handle is home: %u round(s), ready to fire", b.live);
                }
            }
            if (b.leverHeld && b.open && b.lever < 0.5f && !b.driving && b.held == 0) {
                close("the handle pushed forward");
            }
            if (looksLikeObject(bolt) && looksLikeObject(bolt->parent)) {
                if (b.leverHeld && spare.valid) {
                    if (!st.leverPinned) {
                        st.leverPinned = true;
                        st.wristInLever = relative(bolt->world, spare.wrist);
                        st.gripInLever = relative(bolt->world, pointTransform(spare.palm)).translate;
                        auto wrist = spare.wrist;
                        wrist.scale = 1.0f;
                        st.palmInWristLever = relative(wrist, pointTransform(spare.palm)).translate;
                    }
                } else if (st.leverPinned) {
                    st.leverPinned = false;
                    unpinHand();
                    poseFingers(false);
                }
                if (b.lever > 0.0f) {
                    poseLever(root, bolt, b.lever, b.leverRestInRoot);
                    b.leverDrawn = true;
                } else if (b.leverDrawn) {
                    bolt->local = b.leverRestLocal;
                    refreshSubtree(bolt, RE::NiPoint3{});
                    b.leverDrawn = false;
                }
                if (st.leverPinned) {
                    {
                        pinHand(handOnPart(spare.wrist, bolt->world, st.gripInLever, st.palmInWristLever));   // the hand stays on the handle
                    }
                    poseFingers(true);
                }
            }
        }

        // the ejector rod (zones file: box + route "ejector"): pushed along its way by the hand, it throws out what is in
        const auto ejectBox = st.zoneBoxes.find("ejector");
        const auto ejectWay = st.zoneRoutes.find("ejector");
        if (fullyOpen && spare.valid && ejectBox != st.zoneBoxes.end() && ejectWay != st.zoneRoutes.end() && ejectWay->second.size() >= 2) {
            const auto& box = ejectBox;
            const auto& way = ejectWay;
            RE::NiPoint3 rod{};
            const bool touch = handIn(box->second, 1.5f, &rod);
            if (touch) {
                const auto run = way->second.back() - way->second.front();
                const float length = std::sqrt(dot(run, run));
                if (b.ejectTouch && length > 0.1f) {
                    b.ejectPushed = std::max(0.0f, b.ejectPushed + dot(rod - b.ejectPrev, run * (1.0f / length)));
                    if (b.ejectPushed > 0.6f * length && b.live + b.spent > 0) {
                        note("EJECTOR: pushed %.1f of %.1f: %u rounds back to the pouch, %u fired cases out", b.ejectPushed, length, b.live, b.spent);
                        ejectFromChambers(b.live + b.spent, "pushed out by the ejector");
                        if (b.live > 0) {
                            hud("Rounds ejected, returned to inventory");
                        }
                        b.live = 0;
                        b.spent = 0;
                        b.ejectPushed = 0.0f;
                        playSound(4);
                        buzz(freeIndex(), 1500);
                        buzz(g_settings.gunHand, 1500);
                    }
                }
                b.ejectPrev = rod;
            } else {
                b.ejectPushed = 0.0f;
            }
            b.ejectTouch = touch;
        }

        // a break action throws its fired cases out by itself as it opens (extractor); live rounds stay in
        if (b.mech == 2 && fullyOpen && b.spent > 0 && now - b.opened > std::chrono::milliseconds(150)) {
            note("EXTRACTED: %u fired case(s) thrown out as the gun opened, %u live round(s) stay in", b.spent, b.live);
            ejectFromChambers(b.spent, "thrown out by the extractor");
            b.spent = 0;
            buzz(g_settings.gunHand, 1000);
        }

        // the alien blaster: its cell pops out by itself as the cover opens (as pulling it by hand)
        if (cellCover && g_settings.cellPopsOut && b.open && b.t >= 0.6f && b.live + b.spent > 0 && looksLikeObject(cellNode) &&
            now - b.opened > std::chrono::milliseconds(200)) {
            note("The cell popped out as the cover opened: %u rounds back to the pouch", b.live);
            ejectCasings(gun, root, cellNode->world.translate, (axisOf(root->world, 2) * 0.6f + axisOf(root->world, 1) * -0.4f), 1, "the alien blaster's cell");
            b.live = 0;
            b.spent = 0;
            playSound(4);
            buzz(g_settings.gunHand, 1200);
        }

        // muzzle up: everything falls out (not from a break action: its rounds stay in)
        // (held up for a moment and fairly still: a jerk to shut it does not throw them out on the way)
        const bool muzzleUp = barrel.z > (revolver ? 0.5f : 0.3f) && dot(b.velocity, b.velocity) < 90.0f * 90.0f * scale * scale;
        if (!muzzleUp) {
            b.upSince = now;
        }
        if (!single && b.mech != 2 && fullyOpen && b.live + b.spent > 0 && muzzleUp && now - b.upSince > std::chrono::milliseconds(250) &&
            now - b.opened > std::chrono::milliseconds(350)) {
            note("EMPTIED: %u rounds back to the pouch, %u fired cases gone", b.live, b.spent);
            ejectFromChambers(cellCover ? 1u : b.live + b.spent, "fell out, muzzle up");
            if (b.live > 0) {
                hud("Rounds fell out, returned to inventory");
            }
            b.live = 0;
            b.spent = 0;
            playSound(4);
            buzz(g_settings.gunHand, 1500);
        }

        // the free hand
        const bool press = spare.valid && spare.grip && !spare.previousGrip;
        const bool atBelt = spare.valid && inBelt(player, spare.palm);
        const float toGun = spare.valid ? distance(spare.palm, breechAt) : 1e9f;
        if (press && griphook::owned() && !b.open && hinged && unlatched && b.held == 0 && touchesPart(14.0f)) {
            b.driving = true;   // the closed gun taken by its opening part: turning it opens the gun
            b.driveAngle0 = hingeAngle(root, spare.palm);
            b.driveT0 = 0.0f;
            buzz(freeIndex(), 600);
            detail("The free hand takes the closed opening part");
        }
        if (press && griphook::owned() && b.open) {
            if (atBelt && b.held == 0) {
                const auto inventory = gun.data->ammo ? inventoryCount(player, gun.data->ammo) : 0u;
                const auto lying = gun.weapon ? looseRoundsFor(gun.weapon->GetFormID()) : 0u;
                const auto have = inventory > b.live + lying ? inventory - b.live - lying : 0u;
                const auto room = cellCover ? (b.live + b.spent > 0 ? 0u : capacity) :
                                  capacitors ? (b.caps < 3 ? 1u : 0u) : capacity - std::min(capacity, b.live + b.spent);
                const auto take = capacitors ? std::min(have, room) : std::min(have, revolver || cellCover ? room : std::min(room, 1u));
                if (take == 0) {
                    note("BELT: nothing taken (%u carried, %u in the gun, room %u)", inventory, b.live, room);
                }
                if (cellCover && room == 0) {
                    hud("Take the cell out first: pull it by hand or tip the gun up");
                } else if (b.spent > 0 && room == 0) {
                    hud("Shake the empty shells out first: tip the gun up");
                } else if (take == 0) {
                    hud(have == 0 ? "No ammo left" : "Fully loaded");
                } else {
                    b.held = take;
                    dropHandRound("a new one from the belt");
                    g_beltRound.wanted = g_settings.realRounds;
                    playSound(4);
                    buzz(freeIndex(), 1200);
                    note("BELT: %u round(s) in the free hand", take);
                }
            } else if (cellCover && !atBelt && b.held == 0 && b.live + b.spent > 0 && looksLikeObject(cellNode) &&
                       distance(spare.palm, cellNode->world.translate) < 8.0f * scale) {
                note("The cell is pulled out by hand: %u rounds back to the pouch", b.live);
                ejectCasings(gun, root, cellNode->world.translate, axisOf(root->world, 2) * -1.0f, 1, "the alien blaster's cell");
                b.live = 0;
                b.spent = 0;
                playSound(4);
                buzz(freeIndex(), 1200);
            } else if (hinged && !atBelt && b.held == 0 && touchesPart(14.0f)) {
                b.driving = true;
                b.driveAngle0 = hingeAngle(root, spare.palm);
                b.driveT0 = b.t;
                buzz(freeIndex(), 800);
                detail("The free hand takes the opening part at %.2f open", b.t);
            } else if (!hinged && !capacitors && !atBelt && b.held == 0 && fullyOpen && toGun < 12.0f * scale) {
                close("the free hand");
            }
        }
        if (b.driving) {
            if (!(spare.valid && spare.grip)) {
                b.driving = false;
                if (b.open && b.t < 0.3f) {
                    close("pushed shut by hand");
                }
            } else {
                float turn = hingeAngle(root, spare.palm) - b.driveAngle0;
                while (turn > 3.14159265f) turn -= 6.2831853f;
                while (turn < -3.14159265f) turn += 6.2831853f;
                const float before = b.t;
                b.t = std::clamp(b.driveT0 + turn / std::max(g_play.openAngle, 0.1f), 0.0f, 1.0f);
                if (!b.open && b.t > 0.15f) {
                    b.open = true;   // turned open by hand
                    b.opened = now;
                    b.toggled = now;
                    gun.data->ammoCount = 0;
                    b.lastAmmo = 0;
                    playSound(1);
                    buzz(g_settings.gunHand, 1200);
                    note("OPEN (by hand): %u rounds, %u fired cases", b.live, b.spent);
                } else if (b.open && b.t <= 0.05f && before > 0.05f) {
                    b.driving = false;
                    close("pushed shut by hand");
                } else if (b.t >= 0.98f && before < 0.98f) {
                    buzz(freeIndex(), 600);
                }
            }
        }
        // the open palm pushes it shut (no grip needed): only a move towards closed carries it
        const bool touching = hinged && !single && b.open && !b.driving && b.held == 0 && spare.valid && !spare.grip && !atBelt &&
                              now - b.loadedAt > std::chrono::milliseconds(600) && touchesPart(8.0f);
        if (touching) {
            const float angle = hingeAngle(root, spare.palm);
            if (b.pushing) {
                float turn = angle - b.pushAngle;
                while (turn > 3.14159265f) turn -= 6.2831853f;
                while (turn < -3.14159265f) turn += 6.2831853f;
                if (turn < 0.0f) {
                    b.t = std::clamp(b.t + turn / std::max(g_play.openAngle, 0.1f), 0.0f, 1.0f);
                    b.opened = std::min(b.opened, now);
                    if (b.t <= 0.08f) {
                        b.pushing = false;
                        close("pushed shut by the palm");
                    }
                }
            }
            b.pushAngle = angle;
            b.pushing = b.open;
        } else {
            if (b.pushing && b.open && b.t < 0.5f) {
                close("pushed shut by the palm");
            }
            b.pushing = false;
        }
        // the round from the belt as a real object: ROCK is asked to put it in the hand; held, it is not drawn.
        // Let go once ROCK had it, it is gone (the rounds stay with the ammo carried); not taken in time, it is drawn.
        RE::NiAVObject* roundObject = nullptr;
        if (b.held == 0 && !g_beltRound.asked && gun.weapon && !g_looseRounds.empty()) {
            // a real round lying about (dropped, or kept when the gun was put away) taken again by the hand
            const auto held = rocklink::heldBy(freeIndex());
            for (auto it = g_looseRounds.begin(); held && it != g_looseRounds.end(); ++it) {
                if (it->ref == held && it->weapon == gun.weapon->GetFormID()) {
                    b.held = it->rounds;
                    g_beltRound = BeltRound{};
                    g_beltRound.asked = true;
                    g_beltRound.ref = it->ref;
                    g_beltRound.weapon = it->weapon;
                    g_beltRound.held = true;
                    g_beltRound.rounds = it->rounds;
                    g_beltRound.askedAt = now;
                    note("ROUND: the real round %08X is taken again (%u round(s))", it->ref, it->rounds);
                    g_looseRounds.erase(it);
                    break;
                }
            }
        }
        if (g_beltRound.ref) {
            const bool rockHas = rocklink::heldBy(freeIndex()) == g_beltRound.ref;
            auto* ref = lookupRef(g_beltRound.ref);
            auto* object = ref ? ref->Get3D() : nullptr;
            if (rockHas && !g_beltRound.held) {
                g_beltRound.held = true;
                note("ROUND: ROCK holds the real round %08X (%.2f s, %d asks)", g_beltRound.ref,
                    std::chrono::duration<float>(now - g_beltRound.askedAt).count(), g_beltRound.grabTries);
            }
            if (g_beltRound.held && !rockHas) {
                if (atBelt) {
                    dropHandRound("put back at the belt");
                    note("The rounds in the hand are put away");
                } else {
                    keepHandRound("let go: it lies there to be taken again");
                }
                b.held = 0;
            } else if (!g_beltRound.held && now - g_beltRound.askedAt > std::chrono::milliseconds(2000)) {
                dropHandRound("ROCK did not take it: drawn in the hand instead");
            } else if (!g_beltRound.held && spare.valid && spare.grip && g_beltRound.grabTries < 6 &&
                       now - g_beltRound.grabAt > std::chrono::milliseconds(300)) {
                g_beltRound.grabAt = now;
                ++g_beltRound.grabTries;
                griphook::setOwned(false);   // the grip is ROCK's to take it with
                const auto c = looksLikeObject(object) ? object->worldBound.center : spare.palm;
                const float middle[3]{ c.x, c.y, c.z };
                rocklink::forceGrab(freeIndex(), g_beltRound.ref, middle);
            }
            if (g_beltRound.held && looksLikeObject(object)) {
                roundObject = object;
                // the stick click: the way it is held now is kept, the next one from the belt comes like this
                static Clock::time_point clickDown{};
                static bool clickWas = false;
                const bool click = spare.valid && (spare.buttons & buttonMask(g_settings.markButton)) != 0;
                if (click && !clickWas) {
                    clickDown = now;
                }
                if (!click && clickWas && now - clickDown < std::chrono::milliseconds(500) && gun.weapon) {
                    const auto hold = holdAgainst(spare.wand, object->world);
                    if (squareRotation(hold))
                    writeHoldLine(gun.weapon->GetFormID(), "roundhold", hold);
                    st.roundHold = hold;
                    st.roundHoldKnown = true;
                    hud("Round grip saved: rounds from the belt will come like this");
                    buzz(freeIndex(), 3000);
                }
                clickWas = click;
            }
        } else if (g_beltRound.asked && now - g_beltRound.askedAt > std::chrono::seconds(3)) {
            dropHandRound("the object never came: drawn in the hand instead");
        }
        if (b.held > 0 && !g_beltRound.held && !(spare.valid && spare.grip)) {
            note("The rounds in the hand are put away");
            dropHandRound("let go before ROCK had it");
            b.held = 0;
        }
        bool atChamber = toGun < g_settings.breechRadius * scale;
        if ((single || cellCover) && spare.valid) {
            // the palm, or the nose of the round held ahead of the fingers, at the breech
            const auto palmIn = relative(root->world, pointTransform(spare.palm)).translate;
            const auto noseIn = relative(root->world, pointTransform(spare.palm + axisOf(spare.wand, 1) * (10.0f * scale))).translate;
            float d = std::min(distance(palmIn, st.breechMouth), distance(noseIn, st.breechMouth));
            if (st.breechZone) {
                // the socket's cylinder as drawn: way out from the mouth, `side` round its line, `ahead` long.
                // The palm or the nose in it, or the real round touching it (its bound sphere).
                const auto test = [&](const RE::NiPoint3& at, float reach) {
                    const auto off = at - st.breechMouth;
                    const float a = dot(off, st.breechAxis);
                    const auto across = off - st.breechAxis * a;
                    const float r = std::sqrt(dot(across, across));
                    const float outside = std::max({ r - st.breechSide - reach, a - st.breechAhead - reach, -a - st.breechSide - reach, 0.0f });
                    d = std::min(d, outside);
                    return outside <= 0.0f;
                };
                d = 1e9f;
                bool in = test(palmIn, 0.0f);
                in = test(noseIn, 0.0f) || in;
                if (roundObject) {
                    const float radius = roundObject->worldBound.fRadius / std::max(scale, 0.01f);
                    const auto centerIn = relative(root->world, pointTransform(roundObject->worldBound.center)).translate;
                    in = test(centerIn, radius * 0.6f) || in;
                }
                atChamber = in;
            } else {
                if (roundObject) {
                    const auto centerIn = relative(root->world, pointTransform(roundObject->worldBound.center)).translate;
                    d = std::min(d, distance(centerIn, st.breechMouth));
                }
                atChamber = d < st.breechReach;
            }
            static Clock::time_point toldNear{};
            if (b.held > 0 && !atChamber && d < st.breechReach + 15.0f && now - toldNear > std::chrono::milliseconds(500)) {
                toldNear = now;
                note("NEAR BREECH: the round in the hand %.1f %s (%s), open %.2f", d, st.breechZone ? "outside the socket drawn" : "from where it goes in",
                    roundObject ? "a real round" : "drawn", b.t);
            }
        } else if (!st.chambers.empty() && spare.valid) {
            const auto palm = palmOnPart(roundObject ? roundObject->worldBound.center : spare.palm);
            float nearest = 1e9f;
            for (const auto& c : st.chambers) {
                nearest = std::min(nearest, distance(palm, c));
            }
            atChamber = nearest < g_settings.chamberReach;
        }
        if (!single && !cellCover && st.chambers.empty() && roundObject && b.held > 0 && !atChamber) {
            atChamber = distance(roundObject->worldBound.center, breechAt) < g_settings.breechRadius * scale;   // the round itself at the breech
        }
        if (b.held > 0 && fullyOpen && atChamber) {
            dropHandRound("it went in");
            if (capacitors) {
                b.caps = std::min(b.caps + 1, 3u);
                note("LOAD: capacitor %u of 3 in", b.caps);
            } else {
                b.live += b.held;
                b.loadedAt = now;
                note("LOAD: %u round(s) in, %u of %u in the gun", b.held, b.live, capacity);
            }
            b.held = 0;
            playSound(0);
            buzz(0, 2000);
            buzz(1, 2000);
            griphook::setOwned(false);
            if (capacitors && b.caps >= 3) {
                const auto inventory = gun.data->ammo ? inventoryCount(player, gun.data->ammo) : 0u;
                b.live = std::min(inventory, capacity);
                close("all three capacitors in");
            }
        }
        // a flick of the wrist: a revolver rolled sharply about its barrel, a break action's muzzle snapped up
        const auto forwardNow = axisOf(root->world, 1);
        const auto sideNow = axisOf(root->world, 0);
        float flick = 0.0f;
        if (b.turnKnown && dt > 1e-4f) {
            flick = revolver || cellCover ? std::fabs(sideNow.z - b.prevSide.z) / dt : (forwardNow.z - b.prevForward.z) / dt;
        }
        b.prevForward = forwardNow;
        b.prevSide = sideNow;
        b.turnKnown = true;
        if (hasLatch && !b.open && !b.driving && unlatched && now - b.unlatchedAt < std::chrono::milliseconds(1500) && b.unlatchedAt != Clock::time_point{} &&
            -flick > g_settings.flickTurn) {
            b.open = true;   // the latch pressed and the muzzle snapped down: the barrels fall open
            b.t = 0.05f;
            b.omega = 4.0f;
            b.opened = now;
            b.toggled = now;
            gun.data->ammoCount = 0;
            b.lastAmmo = 0;
            playSound(1);
            buzz(g_settings.gunHand, 1200);
            note("OPEN (latch and a flick down, %.1f/s): %u rounds, %u fired cases", -flick, b.live, b.spent);
        }
        if (!swinging && fullyOpen && b.held == 0 && !b.driving && now - b.opened > std::chrono::milliseconds(600)) {
            const float jerk = revolver || cellCover ? std::fabs(dot(b.velocity, axisOf(root->world, 0))) : b.velocity.z;
            if ((jerk > g_settings.flickSpeed * scale || flick > g_settings.flickTurn) && !(barrel.z > 0.3f && b.live + b.spent > 0)) {
                note("Flick: moved %.0f, turned %.1f/s", jerk / std::max(scale, 0.01f), flick);
                close("a flick of the gun");
            }
        }

        // what is seen in it, and the rounds carried in the hand
        const auto show = [&](int index, bool on) {
            if (looksLikeObject(child[index])) {
                setCulled(child[index], !on);
            }
        };
        const std::uint32_t filled = b.live + b.spent;
        const std::uint32_t shownHeld = g_beltRound.held ? 0u : b.held;   // a round held as a real object is not drawn too
        // a mini nuke / a missile is a node of the gun's own model (CNuke / CMissile): seen only when it is in or in the hand
        RE::NiAVObject* bigRound = single ? findNamed(root, "CNuke") : nullptr;
        if (single && !looksLikeObject(bigRound)) {
            bigRound = findNamed(root, "CMissile");
        }
        if (cellCover) {
            if (looksLikeObject(cellNode)) {
                setCulled(cellNode, !(b.live > 0 || b.spent > 0 || shownHeld > 0));
                if (shownHeld == 0 && b.cellRestKnown) {
                    cellNode->local = b.cellRest;   // back in its place in the gun
                    refreshSubtree(cellNode, RE::NiPoint3{});
                }
            }
        } else if (capacitors) {
            for (std::uint32_t i = 0; i < 3; ++i) {
                show(static_cast<int>(i), i < b.caps || (shownHeld > 0 && i == b.caps));
            }
        } else if (single) {
            if (looksLikeObject(bigRound)) {
                setCulled(bigRound, !(b.live > 0 || shownHeld > 0));
            }
        } else if (revolver) {
            show(4, b.live > 0 || shownHeld > 0);   // the bullets
            show(3, filled > 0);                 // the cases
            show(2, shownHeld > 0);                 // the clip they come on
        } else {
            show(0, filled >= 1 || (shownHeld > 0 && filled == 0));
            show(1, filled >= 2 || (shownHeld > 0 && filled == 1));
        }
        if (shownHeld > 0 && spare.valid) {
            const auto y = normalized(axisOf(spare.wrist, 0));   // along the fingers
            auto z = axisOf(spare.wrist, 2);
            z = normalized(z - y * dot(z, y));
            RE::NiTransform pose = identityTransform();
            setRows(pose, cross(y, z), y, z);
            pose.scale = scale;
            pose.translate = spare.palm + y * ((revolver ? 2.0f : 0.0f) * scale);
            if (single) {
                pose.translate = spare.palm + y * (6.0f * scale);   // held by its tail, the warhead ahead of the fingers
            }
            if (st.roundHoldKnown) {
                pose = compose(spare.wand, st.roundHold);
                pose.scale = scale;
            }
            st.roundFrozen = pose;
            // the real round, made where the drawn one is: ROCK takes it from there
            if (g_beltRound.wanted && !g_beltRound.asked && gun.weapon) {
                g_beltRound.wanted = false;
                if (revolver || prepareHeldRoundModel(gun, single || cellCover || capacitors)) {
                    PendingCommand command;
                    command.type = kCasing;
                    command.kind = revolver ? kSpeedloaderKind : kHeldRoundKind;
                    command.token = kPoseToken;
                    command.pose = poseForScript(pose);
                    command.impulse = { 0.0f, 0.0f, 1.0f, 0.0f };
                    pushCommand(command);
                    g_beltRound.asked = true;
                    g_beltRound.askedAt = now;
                    g_beltRound.weapon = gun.weapon->GetFormID();
                    g_beltRound.rounds = b.held;
                    detail("ROUND: a real %s is asked for", revolver ? "speedloader" : "round");
                }
            }
            if (cellCover) {
                if (looksLikeObject(cellNode)) {
                    placeNode(cellNode, pose);
                }
            } else if (capacitors) {
                if (b.caps < 3) {
                    placeNode(child[b.caps], pose);
                }
            } else if (single) {
                if (looksLikeObject(bigRound)) {
                    placeNode(bigRound, pose);
                }
            } else if (revolver) {
                placeNode(child[2], pose);
                placeNode(child[4], pose);
            } else {
                placeNode(child[filled == 0 ? 0 : 1], pose);
            }
        }

        // a revolver with its own clip (WeaponMagazineChild3): the real speedloader ROCK holds is the physics only, unseen;
        // the gun's own clip and its bullets are drawn where it is (the .45 speedloader fits no other gun)
        if (revolver && roundObject && looksLikeObject(child[2])) {
            setCulled(roundObject, true);
            show(2, true);
            show(4, true);
            RE::NiTransform pose = roundObject->world;
            pose.scale = scale;
            placeNode(child[2], pose);
            // the clip's own middle onto the speedloader's middle
            const auto off = roundObject->worldBound.center - child[2]->worldBound.center;
            pose.translate += off;
            placeNode(child[2], pose);
            if (looksLikeObject(child[4])) {
                placeNode(child[4], pose);
            }
        }

        // an open gun: the free hand's grip near it or at the belt is this plugin's (no second-hand hold, no ROCK grab)
        if (!spare.grip) {
            const bool mine = spare.valid && ((b.open && (atBelt || distance(spare.palm, root->world.translate) < g_settings.gunRadius)) ||
                                              (!b.open && hinged && unlatched && touchesPart(0.0f)) ||   // closed: only on the opening part, unlatched
                                              leverTouch);
            st.armReason = mine ? (atBelt ? kArmBelt : kArmBlock) : kArmNone;
            griphook::setArmed(mine);
        }
    }
}
