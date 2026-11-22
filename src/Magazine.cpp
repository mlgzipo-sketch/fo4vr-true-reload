#include "Magazine.h"

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
#include "Angles.h"
#include "GripHook.h"
#include "Gun.h"
#include "Hands.h"
#include "Log.h"
#include "NiMath.h"
#include "Nodes.h"
#include "Objects.h"
#include "Palm.h"
#include "Paths.h"
#include "Rail.h"
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

    // ------------------------------------------------------------ zones around the body

    bool inBelt(RE::PlayerCharacter* player, const RE::NiPoint3& palm, float* heightOut, float* sideOut) noexcept
    {
        const auto feet = player->GetPosition();
        const float height = palm.z - feet.z;
        const float dx = palm.x - feet.x;
        const float dy = palm.y - feet.y;
        const float side = std::sqrt(dx * dx + dy * dy);
        if (heightOut) *heightOut = height;
        if (sideOut) *sideOut = side;
        return height >= g_settings.beltLow && height <= g_settings.beltHigh && side <= g_settings.beltRadius;
    }

    // Behind the body and above the shoulders: where loot is put away.

    bool overShoulder(RE::PlayerCharacter* player, const RE::NiPoint3& palm) noexcept
    {
        const auto feet = player->GetPosition();
        const float yaw = player->data.angle.z;
        const float forward = (palm.x - feet.x) * std::sin(yaw) + (palm.y - feet.y) * std::cos(yaw);
        return palm.z - feet.z >= g_settings.shoulderHeight && forward < 0.0f;
    }

    // ------------------------------------------------------------ the magazine's life

    void startBlend(const RE::NiTransform& from, float seconds) noexcept
    {
        g_state.blendFrom = from;
        g_state.blendStart = Clock::now();
        g_state.blendSeconds = seconds;
    }

    RE::NiTransform blended(const RE::NiTransform& target, Clock::time_point now) noexcept
    {
        if (g_state.blendSeconds <= 0.0f) {
            return target;
        }
        const float t = std::chrono::duration<float>(now - g_state.blendStart).count() / g_state.blendSeconds;
        if (t >= 1.0f) {
            g_state.blendSeconds = 0.0f;
            return target;
        }
        return blend(g_state.blendFrom, target, t * t * (3.0f - 2.0f * t));
    }

    // Caught: the magazine keeps the turn it had in the hand and lines up with the well only as it is pushed in
    // (fully lined up a little before it is seated), so nothing jumps at the mouth and the hand is not turned.

    void startAlign(const RE::NiTransform& rail, const RE::NiTransform& caught, float s) noexcept
    {
        auto& st = g_state;
        st.blendSeconds = 0.0f;
        st.alignOffset = relative(rail, caught);
        st.alignOffset.scale = 1.0f;
        st.alignFromS = s;
        st.alignActive = s > 0.5f;
        if (!st.alignActive) {
            startBlend(caught, 0.12f);
        }
    }

    RE::NiTransform aligned(const RE::NiTransform& rail) noexcept
    {
        auto& st = g_state;
        if (!st.alignActive) {
            return rail;
        }
        const float end = st.alignFromS * 0.35f;
        const float t = std::clamp((st.s - end) / std::max(0.01f, st.alignFromS - end), 0.0f, 1.0f);
        if (t <= 0.0f) {
            st.alignActive = false;
            return rail;
        }
        const float k = t * t * (3.0f - 2.0f * t);
        auto caught = compose(rail, st.alignOffset);
        caught.scale = rail.scale;
        return blend(rail, caught, k);
    }

    void releaseHand() noexcept
    {
        unpinHand();
        poseFingers(false);
    }

    void startEject(const Gun& gun, bool hold, Clock::time_point now) noexcept
    {
        auto& st = g_state;
        st.place = kRailAuto;
        st.s = 0.0f;
        st.autoHold = hold;
        st.resting = false;
        st.blendSeconds = 0.0f;
        syncAmmo(gun);
        playSound(1);
        buzz(g_settings.gunHand, 900);
        st.freezeUntil = now + std::chrono::milliseconds(static_cast<int>(g_settings.freezeSeconds * 1000.0f));
        note("RELEASE (%s): magazine with %u rounds comes out, chamber %s", hold ? "held" : "tap", st.rounds, st.chamber ? "loaded" : "empty");
    }

    void seat(const Gun& gun) noexcept
    {
        auto& st = g_state;
        st.place = kSeated;
        st.s = 0.0f;
        st.blendSeconds = 0.0f;
        st.seatedAt = Clock::now();
        syncAmmo(gun);
        playSound(0);
        buzz(0, 2500);
        buzz(1, 2500);
        releaseHand();
        if (g_settings.regripAfterSeat) {
            griphook::setOwned(false);   // the grip is seen again: the hand may take the pistol now
        }
        if (!st.chamber && !st.hasBolt && st.rounds > 0) {
            st.rounds -= 1;   // no slide / bolt found on this model: the magazine feeds the chamber by itself
            st.chamber = true;
            syncAmmo(gun);
        }
        if (!st.chamber) {
            hud(st.rounds > 0 ? "Magazine in. Chamber empty: rack the slide" : "Magazine is empty");
        }
        note("INSERT: seated, %u rounds, ammoCount %u, chamber %s", st.rounds, gun.data->ammoCount, st.chamber ? "loaded" : "empty");
    }

    // The magazine leaves our hands and becomes a real object; it is drawn falling until that object is there.

    void startDrop(const RE::NiTransform& node, const RE::NiPoint3& velocity, const char* why) noexcept
    {
        auto& st = g_state;
        st.dropToken = spawnLoose(node, st.rounds, why);
        st.dropActive = true;
        st.dropPose = node;
        st.dropVelocity = velocity;
        st.dropRounds = st.rounds;
        st.dropStart = Clock::now();
        st.rounds = 0;
        st.place = kGone;
        st.blendSeconds = 0.0f;
        releaseHand();
    }

    void stash(const char* where) noexcept
    {
        auto& st = g_state;
        char text[96]{};
        std::snprintf(text, sizeof(text), "Magazine stowed (%u rounds returned to inventory)", st.rounds);
        hud(text);
        note("STASH (%s): magazine with %u rounds put away", where, st.rounds);
        st.rounds = 0;
        st.place = kGone;
        st.blendSeconds = 0.0f;
        playSound(4);
        buzz(freeIndex(), 700);
        releaseHand();
    }

    // The free hand closes on a magazine that sticks out: the palm goes to the magazine, not the magazine to the palm.

    void takeFromRail(RE::NiAVObject* root, const Hand& hand) noexcept
    {
        auto& st = g_state;
        const auto pose = railPose(root, st.s);
        auto shifted = pose;
        if (!g_settings.core2) {
            shifted.translate -= grabPoint(root) - hand.palm;   // where the magazine would be if the palm were already on it
        }   // core2: taken wherever the palm is on it, nothing jumps
        st.magInWand = relative(hand.wand, shifted);
        st.wristInMag = relative(shifted, hand.wrist);
        st.driveS0 = st.s;
        st.driveAlong0 = alongRail(root, shifted.translate);
        st.place = kRailHand;
        st.railHandSince = Clock::now();
        st.offRail0 = -1.0f;
        st.autoHold = false;
        st.resting = false;
        st.exitAt = 0.0f;
        st.blendSeconds = 0.0f;
        buzz(freeIndex(), 1200);
        note("TAKE: the free hand has the magazine, %.1f out of the well", st.s);
    }

    // The well's way out as the magazine itself carries it (its own frame at rest), turned with a loose magazine:
    // a cell or a drum that leaves sideways is lined up when this points along the well, not its own Z.

    RE::NiPoint3 wayOutOf(const RE::NiTransform& node) noexcept
    {
        const auto& st = g_state;
        const RE::NiPoint3 w{ dot(axisOf(st.magRest, 0), st.axis), dot(axisOf(st.magRest, 1), st.axis), dot(axisOf(st.magRest, 2), st.axis) };
        return axisOf(node, 0) * w.x + axisOf(node, 1) * w.y + axisOf(node, 2) * w.z;
    }

    // How a magazine from the belt sits in the hand: upright in the fist, rounds forward.

    RE::NiTransform holdPose(const Hand& hand, float scale) noexcept
    {
        const auto bx = axisOf(hand.wrist, 0);   // along the fingers
        const auto bz = axisOf(hand.wrist, 2);   // across the palm
        const float sign = (freeIndex() == 1 ? 1.0f : -1.0f) * (g_settings.holdFlip ? -1.0f : 1.0f);
        const auto y = normalized(bx);
        auto z = bz * sign;
        z = normalized(z - y * dot(z, y));
        RE::NiTransform base = identityTransform();
        setRows(base, cross(y, z), y, z);
        const float d = 0.01745329f;
        Mat turn = axisRotation(0, g_settings.holdRotX * d);
        turn = mul(turn, axisRotation(1, g_settings.holdRotY * d));
        turn = mul(turn, axisRotation(2, g_settings.holdRotZ * d));
        RE::NiTransform pose = identityTransform();
        pose.rotate = fromMat(mul(turn, toMat(base.rotate)));
        pose.scale = scale;
        pose.translate = hand.palm - axisOf(pose, 2) * ((g_state.bottomAt + g_settings.holdUp) * scale) +
                         axisOf(pose, 0) * ((g_settings.holdSide - g_state.sideInNode.x) * scale) - axisOf(pose, 1) * (g_state.sideInNode.y * scale) +
                         axisOf(pose, 1) * (g_settings.holdForward * scale);
        return pose;
    }

    // Where the palm holds a magazine, on the magazine as it sits in the gun (gun frame): "point maghold" from the zones
    // file, else the middle of its outer half (a cell: its middle).

    RE::NiPoint3 defaultHoldPoint() noexcept
    {
        const auto& st = g_state;
        if (st.magBoxKnown) {
            const auto mid = (st.magLo + st.magHi) * 0.5f;
            if (st.cellFed) {
                return mid;
            }
            const auto size = st.magHi - st.magLo;
            const float along = std::fabs(size.x * st.axis.x) + std::fabs(size.y * st.axis.y) + std::fabs(size.z * st.axis.z);
            return mid + st.axis * (along * 0.25f);
        }
        const auto& r = st.magRest;
        const float z = st.cellFed ? (st.topAt + st.bottomAt) * 0.5f : st.bottomAt + (st.topAt - st.bottomAt) * 0.25f;
        return r.translate + (axisOf(r, 0) * st.sideInNode.x + axisOf(r, 1) * st.sideInNode.y + axisOf(r, 2) * z) * r.scale;
    }

    // A magazine from the belt: turned exactly as it goes into the gun as the gun is held now, held at its hold point,
    // so it reaches the well lined up and nothing turns the hand there. Without a gun or a rail: the old fist pose.

    RE::NiTransform beltPose(const RE::NiAVObject* root, const Hand& hand, float scale) noexcept
    {
        const auto& st = g_state;
        if (st.holdInWandKnown) {
            auto pose = compose(hand.wand, st.holdInWand);
            pose.scale = scale;
            return pose;
        }
        if (!g_settings.holdAsInGun || !looksLikeObject(root) || !st.railKnown) {
            return holdPose(hand, scale);
        }
        auto pose = railPose(root, 0.0f);
        pose.scale = scale;
        const auto grip = compose(root->world, pointTransform(st.holdPointKnown ? st.holdPointInGun : defaultHoldPoint())).translate;
        pose.translate += hand.palm - grip;
        return pose;
    }

    std::uint32_t freshMagazineRounds(const Gun& gun, std::uint32_t inventory) noexcept
    {
        const auto& st = g_state;
        // Magazines dropped on the ground do not hold rounds back: the next one from the belt is full while the pouch
        // has the ammo (the rounds are the inventory's, a dropped magazine is only a thing lying there). Only the one
        // in the free hand counts.
        std::uint32_t inHand = 0;
        if (st.handRef) {
            if (const auto it = g_loose.find(st.handRef); it != g_loose.end()) {
                inHand = it->second.rounds;
            }
        }
        if (st.handToken) {
            if (const auto it = g_pending.find(st.handToken); it != g_pending.end()) {
                inHand += it->second.rounds;
            }
        }
        const auto used = gun.data->ammoCount + (st.place != kSeated ? st.rounds : 0u) + inHand;
        const auto available = inventory > used ? inventory - used : 0u;
        auto capacity = capacityOf(gun);
        if (capacity == 0) {
            capacity = 12;
        }
        return available < capacity ? available : capacity;
    }

    // The rounds seen on top of a magazine: as many as it holds (up to the ones the model has).

    bool isGunPartOnMag(std::string name) noexcept
    {
        for (auto& c : name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        // the cryolator's drum; the gatling laser's arms and claws that hold the core (the minigun's drum is its magazine and comes off)
        const int kind = g_state.kind;
        if (kind == kCryoKind) {
            return name.find("drum") != std::string::npos;
        }
        if (kind == kFusionCoreKind) {
            return name.find("arms") != std::string::npos || name.find("claw") != std::string::npos;
        }
        return false;
    }

    // The cryolator hangs its drum under WeaponMagazine: without this the drum leaves with the cell.
    // The drum is kept where it sits on the gun; when the magazine node would be hidden, only its
    // other branches are hidden.

    void keepGunPartsOnMag(RE::NiAVObject* root, RE::NiAVObject* magazine, bool seated) noexcept
    {
        auto& st = g_state;
        if (!looksLikeObject(root) || !looksLikeObject(magazine)) {
            return;
        }
        if (!st.gunPartsChecked && seated) {
            st.gunPartsChecked = true;
            forEachNode(magazine, [&](RE::NiAVObject* object, int depth) {
                if (depth == 0) {
                    return true;
                }
                const auto name = nodeName(object);
                if (!name.empty() && isGunPartOnMag(name) && st.gunPartsOnMag.size() < 8) {
                    st.gunPartsOnMag.emplace_back(name, relative(root->world, object->world));
                    note("MAGAZINE: %s stays on the gun when the magazine comes out", name.c_str());
                    return false;
                }
                return true;
            });
        }
        if (st.gunPartsOnMag.empty()) {
            return;
        }
        const bool wholeHidden = (magazine->flags.flags & 1ull) != 0;
        if (seated || !wholeHidden) {
            for (const auto& name : st.gunPartsHidden) {
                setCulled(findNamed(magazine, name.c_str()), false);
            }
            st.gunPartsHidden.clear();
        }
        if (seated) {
            return;
        }
        if (wholeHidden) {
            setCulled(magazine, false);
            forEachNode(magazine, [&](RE::NiAVObject* object, int depth) {
                if (depth == 0) {
                    return true;
                }
                const auto name = nodeName(object);
                if (isGunPartOnMag(name)) {
                    return false;
                }
                bool holds = false;
                forEachNode(object, [&](RE::NiAVObject* inner, int) {
                    if (holds) {
                        return false;
                    }
                    holds = isGunPartOnMag(nodeName(inner));
                    return !holds;
                });
                if (holds) {
                    return true;   // go down to the drum
                }
                if (!(object->flags.flags & 1ull) && !name.empty() && st.gunPartsHidden.size() < 64) {
                    setCulled(object, true);
                    st.gunPartsHidden.push_back(name);
                }
                return false;
            });
        }
        for (const auto& [name, rest] : st.gunPartsOnMag) {
            if (auto* part = findNamed(magazine, name.c_str())) {
                placeNode(part, compose(root->world, rest));
            }
        }
    }

    void showRounds(RE::NiAVObject* magazine, std::uint32_t rounds) noexcept
    {
        if (!looksLikeObject(magazine)) {
            return;
        }
        static const std::array<const char*, 5> names{ "WeaponMagazineChild1", "WeaponMagazineChild2", "WeaponMagazineChild3", "WeaponMagazineChild4", "WeaponMagazineChild5" };
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (auto* round = findNamed(magazine, names[i]); looksLikeObject(round)) {
                setCulled(round, rounds < i + 1);
            }
        }
    }

    // Real magazines: forgotten when they are gone, cleaned up when old, and taken over by this plugin
    // when the free hand brings one to a gun that has none.

    // The colliders of the gun around the mouth of the well: ROCK's sources whose node's bound holds the mouth.
    void pickWellColliders(RE::NiAVObject* root) noexcept
    {
        auto& st = g_state;
        st.ghostCount = 0;
        // the mouth in the gun root's frame, where ROCK's boxes are
        const auto mouth = relative(root->world, pointTransform(topOf(railPose(root, st.length)))).translate;
        const float p[3]{ mouth.x, mouth.y, mouth.z };
        std::string names;
        for (const auto& source : rocklink::bodiedSources()) {
            if (st.ghostCount >= 15) {
                break;
            }
            if (source.kind == 7) {
                continue;   // the gun's own magazine: off anyway while none is in
            }
            bool inside = true;
            for (int k = 0; k < 3; ++k) {
                inside = inside && p[k] > source.lo[k] - 1.0f && p[k] < source.hi[k] + 1.0f;
            }
            if (inside) {
                st.ghostKeys[st.ghostCount++] = source.key;
                names += names.empty() ? "" : ", ";
                names += source.name;
            }
        }
        static std::string told = "-";
        if (names != told) {
            told = names;
            note("WELL: mouth at (%.1f %.1f %.1f) in the gun; a magazine lined up under it goes through %d collider(s): %s", p[0], p[1], p[2], st.ghostCount,
                names.empty() ? "none (the gun stays solid)" : names.c_str());
        }
    }

    void looseMagazines(const Gun* gun, RE::NiAVObject* root) noexcept
    {
        const auto now = Clock::now();
        auto& st = g_state;
        const auto& free = g_hands[freeIndex()];
        std::vector<std::uint32_t> gone;
        std::uint32_t oldest = 0;
        auto oldestBorn = now;
        std::uint32_t adopt = 0;
        RE::NiTransform adoptPose{};
        for (auto& [id, loose] : g_loose) {
            auto* ref = lookupRef(id);
            auto* mag3D = ref ? ref->Get3D() : nullptr;
            if (!ref || ref->IsDeleted() || ref->IsDisabled() || !ref->GetParentCell()) {
                // gone, or put away over the shoulder through ROCK: its rounds are back with the ammo you carry
                if (ref && !ref->IsDeleted() && loose.rounds > 0) {
                    char text[96]{};
                    std::snprintf(text, sizeof(text), "Magazine stowed (%u rounds returned to inventory)", loose.rounds);
                    hud(text);
                }
                gone.push_back(id);
                continue;
            }
            if (!looksLikeObject(mag3D)) {
                continue;
            }
            float handDistance = 1e9f;
            for (const auto& hand : g_hands) {
                if (hand.valid) {
                    handDistance = std::min(handDistance, distance(mag3D->world.translate, hand.palm));
                }
            }
            const float age = std::chrono::duration<float>(now - loose.born).count();
            if (gun && looksLikeObject(root) && st.railKnown && st.place == kGone && !adopt && loose.kind == st.kind && age > 0.3f &&
                free.valid && free.grip && distance(mag3D->world.translate, free.palm) < g_settings.handRadius) {
                if (!realHand()) {
                    adopt = id;
                    adoptPose = mag3D->world;
                } else {
                    // a real magazine in the hand: the mouth of the well takes its top, then it moves only along the well
                    auto node = compose(mag3D->world, inverse(st.looseInNode));
                    node.scale = gunScale(root) * st.magRest.scale;
                    float side = 0.0f;
                    float top = alongRail(root, topOf(node), &side) + st.topAt * st.magRest.scale;
                    const float tilt = -dot(wayOutOf(node), axisWorld(root));   // -1: lined up with the well
                    // a cell or a core goes in either way round
                    // a cell, a core or a tank goes in however it is turned: it is put straight on the way in once caught
                    // (its turn in the hand never matched what the eye takes for lined up: logs of 0.33-0.37)
                    const bool upright = st.cellFed ? true : tilt < (g_settings.core2 ? -0.7f : -0.5f);
                    const float mouth = st.length;
                    // the gun's own collision stops a real magazine a hand's width short of the mouth, so
                    // the catch reaches out past it and forgives the hand a little to the side
                    const float sideMax = g_settings.core2 ? (st.zoneCatchSide > 0.0f ? st.zoneCatchSide : g_settings.catchSide) : g_settings.wellSide;
                    // a cell, a core or a drum is taken anywhere along its short way in, not only at the mouth
                    const float below = st.cellFed ? mouth + 1.5f : (g_settings.core2 ? 1.5f : 3.0f);
                    const float ahead = g_settings.core2 ? (st.zoneCatchAhead > 0.0f ? st.zoneCatchAhead : g_settings.catchAhead) : g_settings.captureAhead;
                    // the zone is taken as drawn (in Blender or in the game's zone editor): no minimum of its own.
                    // With a mouth in the zones file the cylinder itself is the test: the magazine's top inside it.
                    if (st.zoneMouthKnown) {
                        float along = 0.0f;
                        inWellZone(root, node, sideMax, ahead, below, &along, &side);
                        top = mouth + along;
                    }
                    g_liveTop = catchPointOf(node, false);
                    g_liveTopAt = now;
                    // at the well the gun's collision is switched off (ROCK), so the magazine itself goes into the mouth:
                    // then it is caught there, not pulled in from a hand's width away (0.58)
                    // only a magazine lined up under the well, close to its mouth, and only the colliders that hold the mouth:
                    // the rest of the gun stays solid (0.59: in 0.58 the whole gun went through anything near it)
                    if (g_settings.ghostGun && upright && top < mouth + ahead + g_settings.ghostReach && top > mouth - below - 2.0f &&
                        side < sideMax + g_settings.ghostSide) {
                        if (now - st.wellNearAt > std::chrono::milliseconds(250)) {
                            st.ghostSince = now;
                            pickWellColliders(root);
                        }
                        st.wellNearAt = now;
                    }
                    const bool ghosted = g_settings.ghostGun && now - st.wellNearAt < std::chrono::milliseconds(250) &&
                                         now - st.ghostSince > std::chrono::milliseconds(150);
                    const float catchAhead = ghosted ? std::min(ahead, g_settings.ghostCatchAhead) : ahead;
                    if (upright && side <= sideMax && top <= mouth + catchAhead && top >= mouth - below) {
                        st.rounds = loose.rounds;
                        st.place = kRailHand;
                        st.railHandSince = Clock::now();
                        st.offRail0 = -1.0f;
                        // a cell or a core held off by the gun's collision goes straight to the mouth of its socket
                        const float seat = st.zoneMouthKnown ? seatAlong(root, node) : top;
                        st.s = std::clamp(seat, 0.0f, st.cellFed ? mouth + 0.5f : mouth + ahead);
                        st.exitAt = st.s + 4.0f;   // a shaky hand does not pull it straight back out
                        st.driveS0 = st.s;
                        st.driveAlong0 = alongRail(root, node.translate);
                        st.magInWand = relative(free.wand, node);
                        st.wristInMag = relative(node, free.wrist);
                        st.mustLeave = false;
                        st.velocityValid = false;
                        st.dropActive = false;
                        startAlign(railPose(root, st.s), node, st.s);
                        gone.push_back(id);
                        rocklink::releaseHeld(freeIndex(), id);
                        if (st.handRef == id) {
                            st.handRef = 0;
                        }
                        PendingCommand command;
                        command.type = kDelete;
                        command.ref = id;
                        pushCommand(command);
                        griphook::setOwned(true);
                        buzz(freeIndex(), 900);
                        note("CATCH: the well has the real magazine %08X (%u rounds), %.1f out, %.1f off the line%s", id, st.rounds, st.s, side,
                            ghosted ? " (gun collision off: caught at the mouth)" : "");
                        continue;
                    }
                    static Clock::time_point missTold{};
                    if (top < mouth + ahead + 6.0f && side < sideMax + 6.0f && now - missTold > std::chrono::milliseconds(400)) {
                        missTold = now;   // near the well and not taken: why
                        note("NEAR WELL: magazine %08X top %.1f past the mouth (catch from %.1f), %.1f off the line (max %.1f), upright %.2f (needs > %.2f)",
                            id, top - mouth, catchAhead, side, sideMax, st.cellFed ? std::fabs(tilt) : -tilt, st.cellFed ? 0.5f : (g_settings.core2 ? 0.7f : 0.5f));
                    }
                }
            }
            const bool nearPlayer = handDistance < 60.0f;
            if (age > g_settings.looseLifetime && !nearPlayer) {
                gone.push_back(id);
                PendingCommand command;
                command.type = kDelete;
                command.ref = id;
                pushCommand(command);
                continue;
            }
            if (!nearPlayer && loose.born < oldestBorn) {
                oldestBorn = loose.born;
                oldest = id;
            }
        }
        for (const auto id : gone) {
            g_loose.erase(id);
        }
        if (oldest && static_cast<int>(g_loose.size()) > g_settings.maxLoose) {
            deleteMagazine(oldest);
        }
        if (adopt) {
            // a real magazine held through ROCK, and the gun is empty: from here on it is ours
            adoptPose.scale = gunScale(root) * st.magRest.scale;
            auto node = compose(adoptPose, inverse(st.looseInNode));
            node.scale = adoptPose.scale;
            st.rounds = g_loose[adopt].rounds;
            st.place = kHand;
            st.magInWand = relative(free.wand, node);
            st.wristInMag = relative(node, free.wrist);
            st.mustLeave = false;
            st.velocityValid = false;
            st.dropActive = false;
            st.blendSeconds = 0.0f;
            deleteMagazine(adopt);
            griphook::setOwned(true);
            note("ADOPT: magazine %08X with %u rounds is now carried by the plugin", adopt, st.rounds);
        }
    }
}
