// ManualReloadF: hand reloading for Fallout 4 VR (Into the Radius style), 10mm pistol first.
// A second take on ManualReload (same repo, folder reload/). What is kept from it: the rounds in the gun
// are EquippedWeaponData::ammoCount, the game's own refill is undone, the slide is racked by hand.
//
// What is different here:
//   - near the gun the magazine is never a game object. It is the gun's own magazine node, drawn by this
//     plugin every frame: on a rail along the magazine well, or in the free hand. Nothing is spawned, so
//     nothing pops. A magazine becomes a real object only when it is dropped.
//   - the free hand takes the magazine and is pinned to it (FRIK hand transform) while it is on the rail.
//   - while the free hand works a magazine its grip is hidden from every other reader of the controller
//     (hooks on IVRSystem::GetControllerState*), so ROCK does not take the pistol with that hand.
//   - the reload button is read from the controller: a tap drops the magazine, a hold lets it stick out
//     and wait for the free hand.
#include "Runtime.h"

#include <F4SE/F4SE.h>
#include <RE/Fallout.h>

#include "api/FRIKApiV2.h"
#include "api/openvr.h"

#include "AnimHook.h"
#include "Angles.h"
#include "GripHook.h"
#include "Frame.h"
#include "Breech.h"
#include "StateLog.h"
#include "Editor.h"
#include "Playback.h"
#include "Recorder.h"
#include "Magazine.h"
#include "Objects.h"
#include "Zones.h"
#include "Palm.h"
#include "Rail.h"
#include "Hands.h"
#include "State.h"
#include "Paths.h"
#include "ScriptBridge.h"
#include "Nodes.h"
#include "Gun.h"
#include "Log.h"
#include "NiMath.h"
#include "RockLink.h"
#include "SafeMemory.h"
#include "Settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    using Clock = std::chrono::steady_clock;
    using frik::api::FRIKApiV2;
    using manualreload::runtime::logInfo;
    using manualreload::runtime::logNote;
    using manualreload::runtime::logWarning;
    using namespace mrf;

    void update() noexcept
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Get3D()) {
            return;
        }
        takeScriptNews();
        reloadSettingsIfChanged();
        tidyCasings(Clock::now());
        throwCasings(Clock::now());
        tidyLooseRounds(Clock::now());
        readHand(0);
        readHand(1);
        // a real round in the hand belongs to the gun it came for: another gun, or none, and it is gone
        if (g_beltRound.asked && !(g_state.weapon && g_state.weapon->GetFormID() == g_beltRound.weapon && g_state.breech.held > 0)) {
            if (g_beltRound.ref && rocklink::heldBy(freeIndex()) == g_beltRound.ref) {
                keepHandRound("the gun was put away: it stays in the hand");
            } else {
                dropHandRound("the gun is not open for it any more");
            }
        }
        auto& st = g_state;
        const auto now = Clock::now();
        float dt = st.lastFrame == Clock::time_point{} ? 0.011f : std::chrono::duration<float>(now - st.lastFrame).count();
        st.lastFrame = now;
        dt = std::clamp(dt, 0.001f, 0.05f);

        const auto gun = equippedGun(player);
        animhook::install(player);
        if (gun.weapon != st.weapon) {
            releaseHand();
            griphook::setArmed(false);
            st = State{};
            st.lastFrame = now;
            st.weapon = gun.weapon;
            animhook::takeRefused();   // a refusal for the last gun is not a reload of this one
            dropRound();
            g_round.modelReady = false;
            std::string magModel;
            int kind = gun.weapon && gun.data ? magazineKindOf(player, gun.weapon, magModel) : -1;
            if (kind < 0 && gun.weapon && gun.weapon->GetFormID() == kInstituteLaser) {
                kind = kLaserCellKind;   // the Institute laser: the laser gun's cell
                magModel = "Weapons\\LaserWeapons\\FusionCell.nif";
            }
            if (kind < 0 && gun.weapon && gun.weapon->GetFormID() == kCryolator) {
                kind = kCryoKind;   // the cryolator: its cryo cell is the magazine
                magModel = "Ammo\\CryoAmmo\\CryoCell.nif";
            }
            if (kind < 0 && gun.weapon && gun.weapon->GetFormID() == kLaserGun) {
                kind = kLaserCellKind;   // the laser gun has no magazine mod: its cell is the magazine
                magModel = "Weapons\\LaserWeapons\\FusionCell.nif";
            }
            if (kind < 0 && gun.weapon && gun.weapon->GetFormID() == kGatlingLaser) {
                kind = kFusionCoreKind;   // the gatling laser: its fusion core is the magazine
                magModel = "Ammo\\FusionCore\\FusionCore.nif";
            }
            if (kind < 0 && gun.weapon && gun.weapon->GetFormID() == kMinigun) {
                kind = kMinigunKind;   // the minigun: its drum comes off and a new one goes on
                magModel = "Ammo\\5mm\\5mmAmmo.nif";
            }
            const bool pistol10 = gun.weapon && gun.data && gun.weapon->GetFormID() == k10mmPistol;
            st.managed = gun.weapon && gun.data && (pistol10 || (kind >= 0 && g_settings.allGuns));
            st.equippedAt = now;
            g_noRealHandNow = gun.weapon && g_noRealHandGuns.count(gun.weapon->GetFormID()) > 0;
            const auto total = gun.data ? gun.data->ammoCount : 0u;
            st.chamber = total > 0;
            st.rounds = total > 0 ? total - 1 : 0;
            st.kind = kind >= 0 ? kind : (capacityOf(gun) > 12 ? 1 : 0);
            st.gameBoltAction = true;
            if (gun.instance && readable(gun.instance, sizeof(RE::TESObjectWEAP::InstanceData))) {
                const auto flags = gun.instance->flags.underlying();
                st.gameBoltAction = flags == 0 || gun.instance->flags.any(RE::WEAPON_FLAGS::kBoltAction) || gun.weapon->GetFormID() == 0x0004F46A;
                note("Weapon flags %08X: %s", static_cast<unsigned>(flags), st.gameBoltAction ? "a bolt action" : "not a bolt action (no working of the bolt after a shot)");
            }
            st.cellFed = kind == kLaserCellKind || kind == kSyringerKind || kind == kFusionCoreKind || kind == kMinigunKind || kind >= kPlasmaKind;   // nothing to rack
            note("Equipped weapon %08X, capacity %u, rounds %u, magazine '%s' (kind %d): %s", gun.weapon ? gun.weapon->GetFormID() : 0u,
                capacityOf(gun), total, magModel.c_str(), kind, st.managed ? "reloaded by hand" : "not handled (no magazine of group 1)");
            if (gun.weapon) {
                loadZoneRoutes(gun.weapon->GetFormID());
            }
        }

        if (zoneEditor(player, gun, now, dt)) {
            return;
        }

        const bool unrecorded = g_settings.autoRecord && gun.weapon && gun.data && gun.data->ammo && !hasRecording(gun.weapon->GetFormID()) && !st.zoneHinge.arc && !st.hasZonesFile;
        // the game's reload never starts for a gun reloaded by hand (its animation played on an empty gun); a gun still to be
        // recorded is left to the game
        animhook::setBlocking(st.managed && gun.weapon != nullptr && !(g_settings.recordAnim || unrecorded || g_rec.active));
        if (g_settings.recordAnim || unrecorded || g_rec.active) {
            static Clock::time_point toldAt{};
            if (now - toldAt > std::chrono::seconds(10)) {
                toldAt = now;
                note("IDLE: recording mode (bRecordAnim %d, not recorded yet %d, a take running %d): the game reloads by itself",
                    g_settings.recordAnim ? 1 : 0, unrecorded ? 1 : 0, g_rec.active ? 1 : 0);
            }
            if (unrecorded && !st.recordTold) {
                st.recordTold = true;
                hud("This gun is not recorded yet: fire once and reload as usual, the mod records the animation");
                note("RECORD: %08X has no recording yet, the next reload is the game's own and is written down", gun.weapon->GetFormID());
            }
            releaseHand();
            griphook::setArmed(false);
            recordReload(player, gun, now);
            return;
        }

        if (!st.managed && !st.breech.checked && !st.crankKnown && g_settings.breech && gun.weapon && gun.data) {
            if (auto* drawnRoot = weaponRoot(player); looksLikeObject(drawnRoot)) {
                st.breech.checked = true;
                bool cylinder = false;
                bool shells = false;
                forEachNode(drawnRoot, [&](RE::NiAVObject* object, int) {
                    const auto name = nodeName(object);
                    cylinder = cylinder || name.find("Cylinder") != std::string::npos;
                    shells = shells || name.find("ShotgunShell") != std::string::npos || name.find("BarrelCatch") != std::string::npos;
                    return true;
                });
                st.breech.mech = cylinder ? 1 : (shells ? 2 : (st.singleShot && st.zoneHinge.arc ? 3 : 0));
                if (gun.weapon->GetFormID() == kAlienBlaster) {
                    st.breech.mech = 4;
                } else if (gun.weapon->GetFormID() == kGammaGun) {
                    st.breech.mech = 5;
                }
                if (st.breech.mech != 0 && looksLikeObject(findNamed(drawnRoot, kMovingParts[0]))) {
                    st.managed = true;
                    note("This gun opens: %s, reloaded by hand",
                        st.breech.mech == 1 ? "a revolver (the cylinder swings out)" : st.breech.mech == 2 ? "a break action" :
                        st.breech.mech == 4 ? "a cover and a cell" : st.breech.mech == 5 ? "three capacitors" : "one big round at the breech");
                } else {
                    st.breech.mech = 0;
                }
            }
        }
        if (!st.managed && st.crankKnown && g_settings.breech && gun.weapon && gun.data) {
            if (auto* crankRoot = weaponRoot(player); looksLikeObject(crankRoot)) {
                updateCrank(player, gun, crankRoot, now);
                return;
            }
        }
        auto* root = st.managed ? weaponRoot(player) : nullptr;
        if (st.managed && st.breech.mech != 0 && looksLikeObject(root)) {
            updateBreech(player, gun, root, now, dt);
            return;
        }
        if (!st.managed || !looksLikeObject(root)) {
            if (st.managed && !st.dumped && now - st.equippedAt > std::chrono::seconds(2)) {
                st.dumped = true;   // why nothing happens: the drawn gun has no WeaponMagazine node where it is looked for
                auto* fp = player->firstPerson3D.get();
                note("IDLE: the gun is reloaded by hand but its model is not found (first person 3D %s, third person %s); nodes named Weapon*:",
                    looksLikeObject(fp) ? "present" : "MISSING", looksLikeObject(player->Get3D()) ? "present" : "MISSING");
                int shown = 0;
                RE::NiAVObject* tops[2]{ fp, player->Get3D() };
                for (auto* top : tops) {
                    forEachNode(top, [&](RE::NiAVObject* object, int depth) {
                        const auto name = nodeName(object);
                        if (shown < 40 && name.rfind("Weapon", 0) == 0) {
                            ++shown;
                            note("  %s (depth %d, culled %d)", name.c_str(), depth, (object->flags.flags & 1ull) ? 1 : 0);
                        }
                        return depth < 40;
                    });
                }
            }
            if (st.managed) {
                putAway("the gun is not drawn");
            } else {
                releaseHand();
                griphook::setArmed(false);
            }
            looseMagazines(nullptr, nullptr);
            hideRound();
            // a gun this plugin does not reload yet: ROCK is only asked what parts it has (for the log)
            rocklink::Frame link{};
            auto* otherRoot = !st.managed && gun.weapon && gun.data ? weaponRoot(player) : nullptr;
            link.gun = looksLikeObject(otherRoot);
            if (link.gun && !st.dumped && now - st.equippedAt > std::chrono::seconds(2)) {
                st.dumped = true;   // the nodes of guns not reloaded by hand yet: what the next mechanisms are built from
                logNote("Weapon node tree (not handled yet):");
                dumpTree(otherRoot);
            }
            link.freeHand = freeIndex();
            link.magazine.node = kMovingParts[0];
            link.bolt.node = kMovingParts[1];
            g_rock = rocklink::frame(link);
            return;
        }

        const auto& free = g_hands[freeIndex()];
        const auto& firing = g_hands[g_settings.gunHand];
        const auto inventory = gun.data->ammo ? inventoryCount(player, gun.data->ammo) : 0u;

        if (!st.dumped && now - st.equippedAt > std::chrono::seconds(2)) {
            st.dumped = true;
            logNote("Weapon node tree:");
            dumpTree(root);
        }

        // the game's own reload (raw VR gunState)
        const auto gunState = static_cast<std::uint32_t>(player->gunState);
        const auto reloading = static_cast<std::uint32_t>(g_settings.reloadGunState);
        // the game asked for its reload: its state shows it, or its reload event was refused (then the state may never show it)
        static Clock::time_point refusedAt{};
        const bool refused = animhook::takeRefused();
        if (refused) {
            refusedAt = now;
        }
        const bool stateEdge = gunState == reloading && st.lastGunState != reloading && st.lastGunState != 0xFFFFFFFF;
        const bool pressed = refused || (stateEdge && now - refusedAt > std::chrono::milliseconds(800));
        if (gunState != st.lastGunState) {
            detail("gunState %u -> %u", st.lastGunState, gunState);
        }
        st.lastGunState = gunState;

        // the moving parts: remembered at rest, held still while the game plays its reload
        if (!st.magNodeChecked) {
            st.magNodeChecked = true;
            // pipe guns hang the magazine on another node (WeaponExtra3 > P-Mag); WeaponMagazine is left empty there
            bool drawn = false;
            if (auto* wm = findNamed(root, "WeaponMagazine")) {
                forEachNode(wm, [&](RE::NiAVObject* object, int) {
                    drawn = drawn || !object->IsNode();
                    return !drawn;
                });
            }
            if (!drawn) {
                if (auto* pmag = findNamed(root, "P-Mag"); looksLikeObject(pmag) && looksLikeObject(pmag->parent)) {
                    const auto name = nodeName(pmag->parent);
                    if (!name.empty() && name != "WeaponMagazine") {
                        st.magNodeName = name;
                    }
                }
            }
            note("Magazine node: '%s'%s", st.magNodeName.c_str(), drawn ? "" : " (WeaponMagazine has nothing drawn under it)");
        }
        std::array<RE::NiAVObject*, 5> parts{};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            parts[i] = findNamed(root, kMovingParts[i]);
        }
        if (st.magNodeName != kMovingParts[0]) {
            parts[0] = findNamed(root, st.magNodeName.c_str());
            for (std::size_t i = 1; i < parts.size(); ++i) {
                if (st.magNodeName == kMovingParts[i]) parts[i] = nullptr;
            }
        }
        auto* magazine = parts[0];
        // an energy gun's 'bolt' is not worked by hand, nor the assault rifle's (a fresh magazine chambers by itself)
        auto* bolt = (st.cellFed || st.kind == 2 || st.kind == 3) ? nullptr : parts[1];
        if (st.hasBolt != looksLikeObject(bolt)) {
            st.hasBolt = looksLikeObject(bolt);
            note("Slide / bolt node: %s", st.hasBolt ? "found" : "none (rounds are chambered by the magazine)");
        }
        // an empty gun: the game would play its own reload by itself; the parts stay put (the slide stays open)
        const bool frozen = g_settings.blockReload && (gunState == reloading || now < st.freezeUntil || pressed || st.slideLocked || st.lastAmmo == 0);
        const bool quiet = !frozen && st.place == kSeated && st.s <= 0.0f && gunState == static_cast<std::uint32_t>(g_settings.idleGunState) &&
                           !st.racking && st.rackPull <= 0.0f && !st.slideLocked && now - st.lastShot > std::chrono::milliseconds(600) &&
                           now - st.equippedAt > std::chrono::milliseconds(700) && now - st.lastDrive > std::chrono::milliseconds(700);
        if (quiet && gun.data->ammoCount > 0 && !(g_settings.core2 && st.idleKnown)) {
            // core2: taken once, with the gun at rest after it is drawn; later the game's own reload may have moved the parts
            bool all = true;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (looksLikeObject(parts[i])) {
                    st.idle[i] = parts[i]->local;
                } else {
                    all = all && i != 0;
                }
            }
            st.idleKnown = all;
            learnSlide(root, bolt);
        }
        // the rail (where the magazine sits) is learned at rest even when the gun was taken empty: an empty gun froze the parts
        // (lastAmmo 0) and so was never quiet, and its magazine could never be taken out (EJECT refused, rail known 0; 0.50)
        const bool restingForRail = g_settings.blockReload && st.place == kSeated && st.s <= 0.0f &&
                                    gunState == static_cast<std::uint32_t>(g_settings.idleGunState) && !pressed && now >= st.freezeUntil &&
                                    !st.racking && now - st.equippedAt > std::chrono::milliseconds(700) &&
                                    now - st.lastDrive > std::chrono::milliseconds(700);
        if ((quiet || restingForRail) && !st.railKnown && looksLikeObject(magazine)) {
            learnRail(root, magazine);
        }
        useRockBoxes(root, bolt, quiet && gun.data->ammoCount > 0);
        if (g_settings.core2 && !st.zonesEarly && st.railKnown && gun.weapon) {
            // the zones file at once (the hand finds the parts from the start); read again after ROCK's boxes come
            st.zonesEarly = true;
            zonesFor(gun.weapon->GetFormID(), false);
        }
        if (g_settings.core2 && !st.zonesChecked && st.magBoxKnown && (st.slideBoxKnown || !looksLikeObject(bolt))) {
            st.zonesChecked = true;
            zonesFor(gun.weapon->GetFormID());
        }
        if (g_settings.core2 && st.idleKnown && looksLikeObject(magazine) && st.place == kSeated) {
            magazine->local = st.idle[0];   // seated: always where it sits at rest (the game's reload moves it away)
            refreshSubtree(magazine, RE::NiPoint3{});
        }
        if (frozen && st.idleKnown) {
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (looksLikeObject(parts[i])) {
                    parts[i]->local = st.idle[i];
                    refreshSubtree(parts[i], RE::NiPoint3{});
                }
            }
        }

        // the rounds: shots take them, the game's own refills are undone
        auto ammo = gun.data->ammoCount;
        if (st.lastAmmo != 0xFFFFFFFF) {
            if (ammo > st.lastAmmo && g_settings.blockReload) {
                note("Game refill %u -> %u reverted", st.lastAmmo, ammo);
                ammo = st.lastAmmo;
                gun.data->ammoCount = ammo;
            } else if (ammo < st.lastAmmo) {
                st.lastShot = now;
                st.chamber = ammo > 0;
                if (st.boltAction && st.gameBoltAction && st.place == kSeated) {
                    // a bolt action: the fired case stays in the chamber until the bolt is worked
                    st.rounds = ammo;
                    st.chamber = false;
                    st.spentInChamber = true;
                    ammo = 0;
                    gun.data->ammoCount = 0;
                } else if (st.place == kSeated) {
                    st.rounds = ammo > 0 ? ammo - 1 : 0;
                    if (ammo == 0 && st.lastAmmo == 1) {
                        st.slideLocked = true;   // the last round of a magazine: the slide stays open
                        detail("Last round fired: slide locked open");
                    }
                } else if (ammo != 0) {
                    ammo = 0;                    // no magazine: the chambered round was the only one
                    st.chamber = false;
                    gun.data->ammoCount = 0;
                }
                detail("Shot: %u left (chamber %d, magazine %u)", ammo, st.chamber ? 1 : 0, st.rounds);
            }
        }
        st.lastAmmo = ammo;

        // the reload button: a tap reaches the game (ROCK sends it on release) and shows as gunState; a hold is read here
        const bool triggerHeld = (firing.buttons & buttonMask(g_settings.triggerButton)) != 0;
        const bool buttonNow = (firing.buttons & buttonMask(g_settings.reloadButton)) != 0;
        const bool buttonBefore = (firing.previousButtons & buttonMask(g_settings.reloadButton)) != 0;
        if (buttonNow != buttonBefore) {
            st.buttonEdge = now;
        }
        if (buttonNow && !buttonBefore) {
            st.buttonSince = now;
            st.buttonUsed = false;
        }
        st.buttonDown = buttonNow;
        const bool ready = st.railKnown && looksLikeObject(magazine);
        if (st.buttonDown && !st.buttonUsed && g_settings.blockReload &&
            std::chrono::duration<float>(now - st.buttonSince).count() >= g_settings.holdSeconds) {
            st.buttonUsed = true;
            if (st.place == kSeated && ready && !triggerHeld && !st.racking && !menuOpen()) {
                startEject(gun, true, now);
            } else {
                note("EJECT refused: place %d, rail known %d, magazine node %d, trigger held %d, racking %d", st.place, st.railKnown ? 1 : 0,
                    looksLikeObject(magazine) ? 1 : 0, triggerHeld ? 1 : 0, st.racking ? 1 : 0);
            }
        }
        if (now < st.stopReloadUntil && gunState == reloading) {
            static int cut = 0;
            player->gunState = static_cast<RE::GUN_STATE>(g_settings.idleGunState);
            if (++cut % 30 == 1) {
                note("The game's reload of an empty gun is cut short (gunState %u -> %d)", gunState, g_settings.idleGunState);
            }
        }
        if (pressed && g_settings.blockReload) {
            st.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
            const bool byButton = now - st.buttonEdge < std::chrono::milliseconds(700);
            const bool justEmptied = ammo == 0 && now - st.lastShot < std::chrono::milliseconds(2500);
            if (!byButton && (triggerHeld || justEmptied)) {
                // the game reloads by itself when the trigger is pulled on an empty gun: a dry click, not a reload
                logNote("The game's own reload of an empty gun: ignored");
                if (g_settings.stopEmptyReload) {
                    st.stopReloadUntil = now + std::chrono::milliseconds(1500);
                }
            } else if (st.place == kSeated) {
                if (ready) {
                    startEject(gun, false, now);
                } else {
                    logNote("Reload asked before the gun was measured at rest: ignored");
                }
            } else if (st.place == kRailAuto) {
                st.autoHold = false;
            } else if (st.place == kGone) {
                hud("No magazine: take a new one from your belt (grip with the free hand at the belt)");
            }
        }

        const float magScale = gunScale(root) * (st.magRest.scale > 0.0f ? st.magRest.scale : 1.0f);

        // the free hand closes
        const bool press = free.valid && free.grip && !free.previousGrip;
        if (press) {
            detail("Free hand grip: %s, reason %d, place %d, hidden reads %u", griphook::owned() ? "ours" : "everyone's", st.armReason,
                st.place, griphook::hiddenCount());
        }
        // a grip closed a moment before the palm got there is not lost (the first try used to do nothing: closed beside
        // the magazine, the press was held back as a hold on the gun, and only a second press took it)
        static Clock::time_point pressedAt{};
        if (press) {
            pressedAt = now;
        }
        const bool lateGrip = free.valid && free.grip && !press && now - pressedAt < std::chrono::milliseconds(g_settings.lateGripMs);
        // the same at the belt: the hand closed just above it, then went in
        const bool lateBelt = lateGrip && st.armReason == kArmNone && st.place == kGone && inBelt(player, free.palm) && !g_rock.onGun && !g_rock.onBolt &&
                              rocklink::heldBy(freeIndex()) == 0;
        if (lateBelt) {
            note("BELT (late): the grip closed %.2f s before the palm reached the belt", std::chrono::duration<float>(now - pressedAt).count());
            st.armReason = kArmBelt;
            pressedAt = Clock::time_point{};
        }
        const bool rockArm = rockOn() && (st.armReason == kArmRail || st.armReason == kArmSlide);   // ROCK sees this press too
        const bool rockTook = g_rock.onMagazine && !st.rockWasOnMagazine && free.valid && free.grip && st.place == kRailAuto;
        st.rockWasOnMagazine = g_rock.onMagazine;
        if (rockTook && !(press && st.armReason == kArmRail)) {
            logNote("ROCK closed the free hand on the magazine");
            takeFromRail(root, free);
        } else if ((press && (griphook::owned() || rockArm)) || lateBelt) {
            if (st.armReason == kArmRail && st.place == kRailAuto) {
                takeFromRail(root, free);
            } else if (st.armReason == kArmBelt && st.place == kGone) {
                const auto rounds = freshMagazineRounds(gun, inventory);
                if (rounds == 0) {
                    hud("No ammo for this magazine");
                } else {
                    const auto pose = beltPose(root, free, magScale);
                    st.place = kHand;
                    st.rounds = rounds;
                    st.magInWand = relative(free.wand, pose);
                    st.wristInMag = relative(pose, free.wrist);
                    st.mustLeave = false;
                    st.velocityValid = false;
                    st.blendSeconds = 0.0f;
                    st.lastBelt = now;
                    st.handoverAt = now + std::chrono::milliseconds(60);
                    st.forceTargets = now + std::chrono::milliseconds(50);   // ROCK learns of the magazine before it sees the grip
                    st.handoverDue = g_settings.rockBeltHandover && !realHand();
                    if (realHand()) {
                        st.handToken = spawnLoose(pose, rounds, "from the belt into the hand");
                        st.handTokenDropped = false;
                        griphook::setOwned(true);
                    }
                    playSound(4);
                    buzz(freeIndex(), 1200);
                    note("BELT: a magazine with %u rounds in the free hand (%u in the pouch)", rounds, inventory);
                    for (int i = 0; i < 3; ++i) {
                        const auto a = axisOf(free.wrist, i);
                        const auto b = axisOf(pose, i);
                        detail("  wrist axis %d (%.2f, %.2f, %.2f), magazine axis %d (%.2f, %.2f, %.2f)", i, a.x, a.y, a.z, i, b.x, b.y, b.z);
                    }
                }
            }
        } else if (press && g_settings.spareFromBelt && st.place != kGone && !st.racking && inBelt(player, free.palm) &&
                   distance(free.palm, root->world.translate) > g_settings.gunRadius && st.armReason != kArmSlide && !g_rock.onBolt && !g_rock.onGun &&
                   now - st.lastBelt > std::chrono::milliseconds(700)) {
            // the gun still has its magazine: a spare one is a real object for ROCK to hold
            const auto rounds = freshMagazineRounds(gun, inventory);
            st.lastBelt = now;
            if (rounds > 0 && g_settings.core2) {
                if (!st.handToken && !st.handRef) {
                    st.handToken = spawnLoose(beltPose(root, free, magScale), rounds, "a spare from the belt");
                    st.handTokenDropped = false;
                    st.handSpare = true;
                    playSound(4);
                    buzz(freeIndex(), 1200);
                }
            } else if (rounds > 0) {
                spawnLoose(beltPose(root, free, magScale), rounds, "a spare from the belt");
            }
        }

        if (lateGrip && (st.armReason == kArmBlock || st.armReason == kArmNone) && st.place == kRailAuto && st.railKnown &&
            palmOnMagazine(root, free.palm) && !g_rock.onBolt && !g_rock.onGun) {
            note("TAKE (late): the grip closed %.2f s before the palm reached the magazine",
                std::chrono::duration<float>(now - pressedAt).count());
            st.armReason = kArmRail;
            takeFromRail(root, free);
        }

        // the magazine's way
        switch (st.place) {
        case kRailAuto:
            if (g_settings.core2 && free.valid && !free.grip && st.prevHandKnown && palmOnMagazine(root, free.palm)) {
                // the open palm pushes it back in (only inwards: a palm cannot pull)
                const auto moved = inGun(root, free.wand.translate) - st.prevHandLocal;
                const float along = dot(moved, st.axis);
                if (along < 0.0f && dot(moved, moved) < 36.0f) {
                    st.s = std::max(0.0f, st.s + along);
                    st.autoHold = false;
                    st.resting = true;
                    if (st.s <= g_settings.seatAt) {
                        seat(gun);
                        note("CORE2: the palm pushed the magazine home");
                        break;
                    }
                }
            }
            if (st.resting) {
                break;   // half in the well: it stays where it was left
            }
            if (st.autoHold) {
                if (!st.buttonDown) {
                    st.autoHold = false;   // the button is let go: it falls
                }
                st.s = approach(st.s, std::min(g_settings.extendDistance, st.length - 0.5f), g_settings.slideSpeed * dt);
            } else {
                st.s = approach(st.s, st.length + 0.5f, g_settings.slideSpeed * 1.6f * dt);
                if (st.s >= st.length) {
                    startDrop(railPose(root, st.s), axisWorld(root) * (g_settings.slideSpeed * 1.6f * gunScale(root)), "it fell out of the gun");
                }
            }
            break;
        case kRailHand:
            {
                if (!free.valid || !free.grip) {
                    if (st.s <= g_settings.seatRelease) {
                        seat(gun);
                    } else if (g_settings.core2 && st.s < st.length - 0.5f) {
                        st.place = kRailAuto;   // let go half in: it stays in the well, a push of the palm seats it
                        st.resting = true;
                        st.autoHold = false;
                        releaseHand();
                        note("CORE2: the magazine is let go %.1f out: it stays in the well", st.s);
                    } else {
                        startDrop(railPose(root, st.s), st.velocityValid ? st.velocity : RE::NiPoint3{}, "let go in the well");
                    }
                    break;
                }
                const auto carried = compose(free.wand, st.magInWand);
                const float target = st.driveS0 + (alongRail(root, carried.translate) - st.driveAlong0);
                // core2: caught low (the gun's collision stops a real magazine below the mouth), it leaves again only when
                // pulled 4 further out than it has been since it was caught
                if (g_settings.core2) {
                    st.exitAt = std::min(st.exitAt > 0.0f ? st.exitAt : 1e9f, std::max(st.length + 2.0f, st.s + 4.0f));
                }
                const float limit = g_settings.core2 ? st.exitAt : st.length + g_settings.captureAhead + 1.0f;
                // the hand pulled across the way out (the pipe guns throw the magazine out sideways, a hand pulls it down):
                // past the mouth it comes free, and a hand far off is never left glued to a magazine stuck on the rail
                // (it hung at 7.3 of 6.5 for two seconds and ROCK's hand flew off with it, 0.55)
                const float offRail = distance(carried.translate, railPose(root, std::clamp(target, 0.0f, st.length + 4.0f)).translate);
                // measured from where the hand was against the way when it took or caught the magazine (a catch snaps the
                // magazine onto the line, the hand stays a few units off: that is not a pull; 0.56 caught and let go in a loop)
                if (st.offRail0 < 0.0f) {
                    st.offRail0 = offRail;
                }
                const float pull = offRail - st.offRail0;
                const bool pulledAcross = now - st.railHandSince > std::chrono::milliseconds(300) &&
                                          ((st.s >= st.length - 1.0f && pull > g_settings.railBreakAway) || pull > g_settings.railBreakAway * 3.0f);
                if (pulledAcross) {
                    note("OUT: pulled %.1f across the way out (%.1f off at the take) at %.1f of %.1f: the magazine comes free", offRail, st.offRail0, st.s, st.length);
                }
                if (target > limit || pulledAcross) {
                    // clear of the well: from here it goes with the hand
                    if (g_settings.holdPoseOnExit && !g_rock.onMagazine) {
                        startBlend(st.lastDrawnValid ? st.lastDrawn : railPose(root, st.s), 0.20f);
                        st.magInWand = relative(free.wand, holdPose(free, gunScale(root)));
                    } else {
                        // it stays in the hand the way it was taken: no jump, and ROCK's hand stays on it
                        st.magInWand = relative(free.wand, st.lastDrawnValid ? st.lastDrawn : railPose(root, st.s));
                    }
                    st.place = kHand;
                    st.mustLeave = true;
                    unpinHand();
                    playSound(1);
                    note("OUT: the magazine left the well in the free hand (%u rounds)", st.rounds);
                    st.forceTargets = now + std::chrono::milliseconds(1500);   // ROCK is told the gun's magazine is no longer to be held
                    if (realHand()) {
                        // drawn at the hand only until the real one is there; then ROCK holds the real one
                        st.handToken = spawnLoose(compose(free.wand, st.magInWand), st.rounds, "out of the well into the hand");
                        st.handTokenDropped = false;
                        st.handoverDue = false;
                        griphook::setOwned(true);
                    }
                    break;
                }
                const float before = st.s;
                st.s = std::clamp(target, 0.0f, limit);
                if (static_cast<int>(before) != static_cast<int>(st.s)) {
                    buzz(freeIndex(), 250);   // it rubs along the well
                }
                if (st.s <= g_settings.seatAt) {
                    seat(gun);
                }
            }
            break;
        case kHand:
            {
                const auto target = compose(free.wand, st.magInWand);
                if (st.handToken) {
                    setFresh(st.handToken, target);
                }
                if (st.handToken && (!free.valid || !free.grip)) {
                    // let go before the real one came: it appears where the hand is and falls (or goes back to the pouch)
                    st.handTokenDropped = true;
                    st.handStash = inBelt(player, free.palm) || overShoulder(player, free.palm);
                    st.place = kGone;
                    st.rounds = 0;
                    note("The magazine is let go before the real one came (%s)", st.handStash ? "put away" : "it falls");
                    break;
                }
                if (!free.valid || !free.grip) {
                    const auto pose = st.lastDrawnValid ? st.lastDrawn : target;
                    if (inBelt(player, free.palm)) {
                        stash("at the belt");
                    } else if (overShoulder(player, free.palm)) {
                        stash("over the shoulder");
                    } else {
                        startDrop(pose, st.velocityValid ? st.velocity : RE::NiPoint3{}, "let go");
                    }
                    break;
                }
                if (!st.railKnown) {
                    break;
                }
                float side = 0.0f;
                float top = alongRail(root, topOf(target), &side) + st.topAt * st.magRest.scale;   // as if it were on the rail this far out
                // the same window as for a real magazine (zones file, cells either way round)
                const float tilt = -dot(wayOutOf(target), axisWorld(root));
                const bool upright = st.cellFed ? true : tilt < -0.5f;
                const float mouth = st.length;
                const float sideMax = g_settings.core2 ? (st.zoneCatchSide > 0.0f ? st.zoneCatchSide : g_settings.catchSide) : g_settings.wellSide;
                const float ahead = g_settings.core2 ? (st.zoneCatchAhead > 0.0f ? st.zoneCatchAhead : g_settings.catchAhead) : g_settings.captureAhead;
                const float below = st.cellFed ? mouth + 1.5f : 3.0f;
                if (st.zoneMouthKnown) {
                    float along = 0.0f;
                    inWellZone(root, target, sideMax, ahead, below, &along, &side);
                    top = mouth + along;
                }
                g_liveTop = catchPointOf(target, false);
                g_liveTopAt = now;
                if (st.mustLeave) {
                    if (top > mouth + ahead + 2.5f || side > sideMax + 2.5f) {
                        st.mustLeave = false;
                    }
                } else if (upright && side <= sideMax && top <= mouth + ahead && top >= mouth - below) {
                    // the mouth of the well takes the top of the magazine: from here it moves only along the well
                    st.place = kRailHand;
                    st.railHandSince = Clock::now();
                    st.offRail0 = -1.0f;
                    const float seat = st.zoneMouthKnown ? seatAlong(root, target) : top;
                    st.s = std::clamp(seat, 0.0f, st.cellFed ? mouth + 0.5f : mouth + ahead);
                    st.exitAt = st.s + 4.0f;
                    st.driveS0 = st.s;
                    st.driveAlong0 = alongRail(root, target.translate);
                    startAlign(railPose(root, st.s), st.lastDrawnValid ? st.lastDrawn : target, st.s);
                    buzz(freeIndex(), 900);
                    note("CATCH: the well has the magazine, %.1f out, %.1f off the line", st.s, side);
                } else {
                    static Clock::time_point missTold{};
                    if (top < mouth + ahead + 6.0f && side < sideMax + 6.0f && now - missTold > std::chrono::milliseconds(400)) {
                        missTold = now;
                        note("NEAR WELL (drawn): top %.1f past the mouth (catch from %.1f), %.1f off the line (max %.1f), upright %.2f (needs > 0.50)",
                            top - mouth, ahead, side, sideMax, st.cellFed ? std::fabs(tilt) : -tilt);
                    }
                }
            }
            break;
        default:
            break;
        }

        // a dropped magazine falls until the real one is there
        if (st.dropActive) {
            st.dropVelocity.z -= kGravity * dt;
            st.dropPose.translate += st.dropVelocity * dt;
            const float floor = player->GetPosition().z + 1.5f;
            if (st.dropPose.translate.z < floor) {
                st.dropPose.translate.z = floor;
                st.dropVelocity = {};
            }
            setFresh(st.dropToken, st.dropPose);
            if (now - st.dropStart > std::chrono::seconds(3)) {
                st.dropActive = false;
            }
        }

        // where the magazine is drawn
        bool draw = false;
        bool carried = false;
        RE::NiTransform drawn{};
        std::uint32_t shown = st.rounds;
        if (st.place == kRailAuto) {
            drawn = railPose(root, st.s);
            draw = true;
        } else if (st.place == kRailHand) {
            drawn = blended(aligned(railPose(root, st.s)), now);
            draw = carried = true;
        } else if (st.place == kHand) {
            drawn = blended(compose(free.wand, st.magInWand), now);
            drawn.scale = magScale;
            draw = carried = true;
        } else if (st.place == kGone && st.dropActive) {
            drawn = st.dropPose;
            shown = st.dropRounds;
            draw = true;
        }
        if (carried) {
            if (st.velocityValid) {
                st.velocity = st.velocity * 0.5f + (drawn.translate - st.lastMagAt) * (0.5f / dt);
            } else {
                st.velocity = {};
            }
            st.lastMagAt = drawn.translate;
            st.velocityValid = true;
        } else {
            st.velocityValid = false;
        }
        if (looksLikeObject(magazine)) {
            if (st.place == kSeated) {
                setCulled(magazine, false);
            } else if (draw) {
                setCulled(magazine, false);
                // in the hand and held by ROCK: ROCK has already put the node where it was told a frame ago and has its
                // hand on that very pose; placing it again here would give the magazine a second, slightly different, hold
                bool leave = false;
                if (st.place == kHand && g_rock.onMagazine) {
                    const auto apart = relative(drawn, magazine->world).translate;
                    const float off = std::sqrt(dot(apart, apart));
                    leave = g_settings.rockPlacesHeld && off < 25.0f;
                    static int tick = 0;
                    if (++tick % 90 == 0) {
                        detail("Held magazine: ROCK has it (%.2f, %.2f, %.2f) from where the controller puts it (%.2f apart)%s", apart.x, apart.y, apart.z,
                            off, leave ? "" : ": too far, placed by the plugin");
                    }
                }
                if (!leave) {
                    placeNode(magazine, drawn);
                } else {
                    const auto& w = magazine->world;
                    carryBounds(magazine, w.translate + (axisOf(w, 0) * st.sideInNode.x + axisOf(w, 1) * st.sideInNode.y +
                                                         axisOf(w, 2) * ((st.topAt + st.bottomAt) * 0.5f)) * w.scale);
                }
            } else {
                setCulled(magazine, true);
            }
            keepGunPartsOnMag(root, magazine, st.place == kSeated);
            showRounds(magazine, shown);
        }
        st.lastDrawn = drawn;
        st.lastDrawnValid = draw;

        // the slide: the game shows it open on an empty gun, but here it is open only after the last shot
        if (st.idleKnown && looksLikeObject(bolt) && (gun.data->ammoCount == 0 || st.slideLocked || st.racking || st.rackPull > 0.0f)) {
            bolt->local = st.idle[1];
            refreshSubtree(bolt, RE::NiPoint3{});
        }
        rackByHand(gun, root, bolt, dt);
        updateMagLever(gun, root, now);
        st.prevHandLocal = inGun(root, free.wand.translate);
        st.prevHandKnown = free.valid;
        drawRound(gun, root);
        if (g_settings.core2) {
            realMagazineInHand(player);
        }

        // the free hand stays on what it holds: the magazine in the well, or the slide
        const bool rockHolds = g_rock.onMagazine || g_rock.onBolt;   // ROCK draws the hand and the fingers on the part itself
        if (rockHolds) {
            st.rackPinned = false;
            unpinHand();
        } else if (st.leverHeld && free.valid && looksLikeObject(leverNodeOf(root))) {
            auto* handle = leverNodeOf(root);   // the hand stays on the handle it pulls
            if (!st.leverPinned) {
                st.leverPinned = true;
                st.wristInLever = relative(handle->world, free.wrist);
                st.gripInLever = relative(handle->world, pointTransform(free.palm)).translate;
                auto wrist = free.wrist;
                wrist.scale = 1.0f;
                st.palmInWristLever = relative(wrist, pointTransform(free.palm)).translate;
            }
            pinHand(handOnPart(free.wrist, handle->world, st.gripInLever, st.palmInWristLever));   // where the handle goes, turned as the controller is
        } else if (st.racking && looksLikeObject(bolt) && free.valid) {
            if (!st.rackPinned) {
                st.rackPinned = true;
                st.wristInBolt = relative(bolt->world, free.wrist);
            }
            pinHand(compose(bolt->world, st.wristInBolt));
        } else {
            st.rackPinned = false;
            if (st.place == kRailHand && free.valid) {
                // the palm stays where it took the magazine; the wrist is not turned round to the magazine's own
                // turn on the rail (that wrung the hand)
                if (!st.railPinned) {
                    st.railPinned = true;
                    const auto taken = compose(drawn, st.wristInMag);
                    const auto palm = compose(taken, pointTransform(relative(free.wrist, pointTransform(free.palm)).translate)).translate;
                    st.gripInMag = relative(drawn, pointTransform(palm)).translate;
                    auto wrist = free.wrist;
                    wrist.scale = 1.0f;
                    st.palmInWristMag = relative(wrist, pointTransform(free.palm)).translate;
                }
                pinHand(handOnPart(free.wrist, drawn, st.gripInMag, st.palmInWristMag));
            } else {
                st.railPinned = false;
                unpinHand();
            }
        }
        if (!st.leverHeld) {
            st.leverPinned = false;
        }
        if (st.place != kRailHand) {
            st.railPinned = false;
            st.alignActive = false;
        }
        poseFingers(!rockHolds && (st.place == kRailHand || st.place == kHand || st.racking || st.leverHeld));
        // a magazine on the rail is moved by the plugin alone: if ROCK saw the grip it would take the gun's
        // magazine part with this hand and pull it straight back out (the laser cell, 0.35)
        if (st.place == kRailHand && free.grip && !griphook::owned()) {
            griphook::setOwned(true);
            detail("RAIL: the grip is hidden from ROCK again");
        }

        // a magazine from the belt: once it is in the hand the grip is shown again, so that ROCK closes the hand on it
        if (st.handoverDue && now >= st.handoverAt) {
            st.handoverDue = false;
            if (rockOn() && st.place == kHand && free.grip && griphook::owned()) {
                griphook::setOwned(false);
                detail("Belt magazine: the grip is shown to ROCK");
            }
        }

        looseMagazines(&gun, root);

        // whose the free hand's next grip press will be
        if (!free.grip) {
            int reason = kArmNone;
            if (free.valid) {
                // a magazine sticking out comes first: the slide is not taken until it is dealt with
                float toSlide = looksLikeObject(bolt) && st.place != kRailAuto ? distance(free.palm, slideGrabPoint(root, bolt)) : 1e9f;
                float toMag = st.place == kRailAuto && st.railKnown ? distance(free.palm, grabPoint(root)) : 1e9f;
                if (g_settings.core2) {
                    // touching, not near a point: the box of the part as it stands
                    toMag = st.place == kRailAuto && st.railKnown && palmOnMagazine(root, free.palm) ? 0.0f : 1e9f;
                    if (toMag > 0.0f && looksLikeObject(bolt) && st.slideBoxKnown) {
                        const float pull = st.rackPull;
                        const float pullU = pull / std::max(g_settings.rackTravel, 0.1f);
                        const auto palmIn = inGun(root, free.palm);
                        const auto shift = hasRoute() ? palmIn - routeInverse(pullU, palmIn)
                                         : g_settings.recordedBolt && g_play.parsed && g_play.boltKnown && g_play.weapon == gun.weapon->GetFormID()
                                               ? boltWay(pull / std::max(g_settings.rackTravel, 0.1f)).translate - boltWay(0.0f).translate
                                               : RE::NiPoint3{ 0.0f, -pull, 0.0f };
                        toSlide = inBox(st.slideLo, st.slideHi, inGun(root, free.palm) - shift, g_settings.contactMargin) ? 0.5f : 1e9f;
                    }
                }
                if (toMag < g_settings.magGrabRadius && toMag <= toSlide) {
                    reason = kArmRail;
                } else if (toSlide < g_settings.slideGrabRadius) {
                    reason = kArmSlide;
                } else if (st.place == kGone && inBelt(player, free.palm)) {
                    reason = kArmBelt;
                } else if (g_settings.blockTwoHand && st.place != kSeated && distance(free.palm, root->world.translate) < g_settings.gunRadius) {
                    reason = kArmBlock;
                }
            }
            if (st.leverTouch && !st.leverHeld) {
                reason = kArmBlock;   // the handle (gatling laser): the grip is ours
            }
            st.armReason = reason;
            // at the magazine and at the slide ROCK must see the press (it takes the part); elsewhere it is hidden as before
            const bool leaveToRock = rockOn() && (reason == kArmRail || reason == kArmSlide);
            griphook::setArmed(reason != kArmNone && !leaveToRock);
        }

        // ROCK: what the free hand may take, and where the parts have been moved
        {
            rocklink::Frame link{};
            link.gun = true;
            link.freeHand = freeIndex();
            link.exclusive = st.place != kSeated || st.armReason == kArmSlide || st.racking;
            const bool justSeated = now - st.seatedAt < std::chrono::milliseconds(700);   // the hand must not jump from the magazine to the slide
            link.keepTargets = free.grip && !justSeated && now > st.forceTargets && !(g_settings.core2 && st.handRef && !st.grabDone);
            link.chamber = st.chamber ? 2 : 1;
            const bool magOut = draw && (st.place == kRailAuto || st.place == kRailHand || st.place == kHand) && looksLikeObject(magazine);
            link.magazine.node = st.magNodeName.c_str();
            link.magazine.grab = magOut && st.railKnown;
            link.magazine.point[0] = st.sideInNode.x;
            link.magazine.point[1] = st.sideInNode.y;
            link.magazine.point[2] = st.bottomAt + g_settings.grabUp;
            link.magazine.radius = g_settings.magGrabRadius;
            if (magOut && looksLikeObject(magazine->parent)) {
                // ROCK uses this pose a frame later, against the gun as it stands then: with a magazine carried in the
                // hand both are moved ahead by one frame's travel, or the hand ROCK draws on it trails and shakes
                auto target = drawn;
                auto parentWorld = magazine->parent->world;
                if (st.place == kHand && st.prevDriveKnown) {
                    const auto magStep = drawn.translate - st.prevDrawnAt;
                    const auto parentStep = parentWorld.translate - st.prevParentAt;
                    if (dot(magStep, magStep) < 36.0f && dot(parentStep, parentStep) < 36.0f) {
                        target.translate += magStep;
                        parentWorld.translate += parentStep;
                    }
                }
                st.prevDrawnAt = drawn.translate;
                st.prevParentAt = magazine->parent->world.translate;
                st.prevDriveKnown = true;
                link.magazine.drive = true;
                link.magazine.parentLocal = toLinkPose(relative(parentWorld, target));
            } else if (draw && st.place == kGone && st.railKnown && looksLikeObject(magazine) && looksLikeObject(magazine->parent)) {
                // no magazine in the gun: ROCK's collider of the old one would stay in the well and push a fresh one aside
                // (the pipe pistol, 0.50): it is carried far out along the way out while the well is empty
                st.prevDriveKnown = false;
                link.magazine.drive = true;
                link.magazine.parentLocal = toLinkPose(relative(magazine->parent->world, railPose(root, 200.0f)));
            } else {
                st.prevDriveKnown = false;
            }
            if (looksLikeObject(bolt)) {
                link.bolt.node = kMovingParts[1];
                // a magazine sticking out comes first; one on its way in (in the hand, on the rail) too: the pipe guns' bolt handle is
                // beside the well, and ROCK moved the hand from the magazine onto it as the magazine was caught (0.56)
                link.bolt.grab = st.slidePointKnown && !justSeated && st.place != kRailAuto && st.place != kRailHand && st.place != kHand;   // a magazine sticking out comes first
                link.bolt.point[0] = st.slidePointInBolt.x;
                link.bolt.point[1] = st.slidePointInBolt.y;
                link.bolt.point[2] = st.slidePointInBolt.z;
                link.bolt.radius = g_settings.slideGrabRadius;
                if ((st.racking || st.rackPull > 0.0f) && looksLikeObject(bolt->parent)) {
                    link.bolt.drive = true;
                    link.bolt.parentLocal = toLinkPose(relative(bolt->parent->world, bolt->world));
                }
            }
            if (link.magazine.drive || link.bolt.drive) {
                st.lastDrive = now;
            }
            // no magazine is drawn in the gun (none, or the real one is in the hand): its collider is switched off as well
            const bool magHidden = looksLikeObject(magazine) && (magazine->flags.flags & 1ull) != 0;
            link.noMagazine = (st.place == kGone || (st.place == kHand && magHidden));
            link.ghostGun = g_settings.ghostGun && now - st.wellNearAt < std::chrono::milliseconds(250);
            link.ghostCount = st.ghostCount;
            for (int i = 0; i < st.ghostCount && i < 16; ++i) {
                link.ghostKeys[i] = st.ghostKeys[i];
            }
            g_rock = rocklink::frame(link);
        }

        // the log: every change of state, and a numbered mark when the free hand clicks its stick
        {
            static std::string lastKey;
            static int marks = 0;
            const bool markNow = free.valid && (free.buttons & buttonMask(g_settings.markButton)) != 0 &&
                                 (free.previousButtons & buttonMask(g_settings.markButton)) == 0;
            if (markNow && saveHold(gun)) {
                hud("Magazine grip saved: magazines from the belt will come like this");
                buzz(freeIndex(), 3000);
            } else if (markNow) {
                ++marks;
                char tag[32];
                std::snprintf(tag, sizeof(tag), "MARK %d", marks);
                logNote("==================================================================");
                writeState(tag, gun, root, magazine, bolt);
                char text[96];
                std::snprintf(text, sizeof(text), "Mark %d written to the log", marks);
                hud(text);
                buzz(freeIndex(), 3000);
            } else if (g_settings.traceState) {
                auto key = stateKey();
                if (key != lastKey) {
                    lastKey = std::move(key);
                    writeState("state", gun, root, magazine, bolt);
                }
            }
        }

        // the grip of some controllers is also their third axis
        if (now - st.axisChecked > std::chrono::seconds(3)) {
            st.axisChecked = now;
            const auto device = controllerOf(freeIndex());
            if (g_vr && device != vr::k_unTrackedDeviceIndexInvalid) {
                vr::ETrackedPropertyError error = vr::TrackedProp_Success;
                const auto type = g_vr->GetInt32TrackedDeviceProperty(device, static_cast<vr::ETrackedDeviceProperty>(vr::Prop_Axis0Type_Int32 + 2), &error);
                const bool isGrip = error == vr::TrackedProp_Success && type == vr::k_eControllerAxis_Trigger;
                griphook::setAxis2IsGrip(isGrip);
                if (!st.axisLogged) {
                    st.axisLogged = true;
                    note("Free hand controller: third axis type %d (%s)", static_cast<int>(type), isGrip ? "a grip trigger, hidden with the grip" : "left alone");
                }
            }
        }
    }

    // the frame update with a clock on it: a frame that takes long is written to the log (to find hitches)
    void timedUpdate() noexcept
    {
        using clock = std::chrono::steady_clock;
        static clock::time_point lastFlush = clock::now();
        static clock::time_point lastSlowNote{};
        static int slowFrames = 0;
        static double worst = 0.0;
        const auto start = clock::now();
        g_memoryEpoch.fetch_add(1, std::memory_order_relaxed);
        update();
        const auto end = clock::now();
        const double ms = std::chrono::duration<double, std::milli>(end - start).count();
        if (ms > 2.0) {
            ++slowFrames;
            worst = (std::max)(worst, ms);
            if (end - lastSlowNote > std::chrono::seconds(5)) {
                lastSlowNote = end;
                char text[128];
                std::snprintf(text, sizeof(text), "SLOW: ManualReloadF frame took %.1f ms (%d slow frames, worst %.1f ms)", ms, slowFrames, worst);
                logNote(text);
                slowFrames = 0;
                worst = 0.0;
            }
        }
        if (end - lastFlush > std::chrono::seconds(2)) {
            lastFlush = end;
            manualreload::runtime::flushLog();
        }
    }

    void FRIK_CALL onFrikFrame(std::uint32_t, void*) noexcept
    {
        static std::uint64_t frames = 0;
        if (++frames % 5400 == 1) {
            logNote("frame update is running");
        }
        __try {
            timedUpdate();
        } __except (1) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                logWarning("A fault inside the frame update was caught; ManualReloadF keeps running");
            }
        }
    }

    void hookLog(const char* text) noexcept
    {
        logNote(text);
    }

    void gameReady() noexcept
    {
        static bool done = false;
        if (!done) {
            done = true;
            const int error = FRIKApiV2::initialize(3);
            if (error != 0) {
                note("FRIK API v2.3+ unavailable (error %d); ManualReloadF cannot run", error);
            } else {
                const bool hooked = FRIKApiV2::inst->registerFrameCallback(kFrikTag,
                    static_cast<std::uint32_t>(FRIKApiV2::FramePhase::AfterWorldFinal), onFrikFrame, nullptr, 0);
                note("FRIK %s, API v2.%u: frame callback %s", FRIKApiV2::inst->getModVersion(), FRIKApiV2::inst->getVersion(),
                    hooked ? "registered" : "FAILED");
            }
        }
        if (!g_vr) {
            using GetInterface_t = void* (*)(const char*, vr::EVRInitError*);
            const auto module = REX::W32::GetModuleHandleW(L"openvr_api.dll");
            const auto get = module ? reinterpret_cast<GetInterface_t>(REX::W32::GetProcAddress(module, "VR_GetGenericInterface")) : nullptr;
            vr::EVRInitError error = vr::VRInitError_None;
            g_vr = get ? static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version, &error)) : nullptr;
            logNote(g_vr ? "OpenVR ready" : "OpenVR not found: buttons cannot be read");
        }
        if (g_vr) {
            griphook::install(&hookLog, g_settings.hideGrip);
        }
        rocklink::connect(&hookLog, g_settings.rockParts);
        g_rock = {};
        // a loaded save: magazines of the last session are gone or unknown
        releaseHand();
        griphook::setArmed(false);
        griphook::setOwned(false);
        g_loose.clear();
        g_pending.clear();
        g_state = State{};
        logNote("ManualReloadF ready (build " MANUALRELOAD_BUILD ")");
    }
}

namespace manualreload::runtime
{
    const Definition& definition() noexcept
    {
        static const Definition value{
            .pluginName = "ManualReloadF",
            .pluginVersion = 1,
            .onGameReady = &gameReady,
        };
        return value;
    }

    void onLoad() noexcept
    {
        loadSettings();
        const auto* papyrus = F4SE::GetPapyrusInterface();
        if (!papyrus || !papyrus->Register(registerNatives)) {
            logWarning("Could not register Papyrus functions; magazines cannot be dropped as objects");
        }
    }
}
