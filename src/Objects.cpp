#include "Objects.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <deque>
#include <unordered_set>
#include <vector>

#include "Runtime.h"
#include "Angles.h"
#include "GripHook.h"
#include "Gun.h"
#include "Hands.h"
#include "MagSlots.h"
#include "Log.h"
#include "NiMath.h"
#include "Nodes.h"
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

    // guns whose loose magazine ROCK would not take (it hung in the air): their magazine is drawn in the hand instead
    std::unordered_set<std::uint32_t> g_noRealHandGuns;

    bool g_noRealHandNow{ false };

    bool realHand() noexcept { return (g_settings.realInHand || g_settings.core2) && !g_noRealHandNow; }

    // ------------------------------------------------------------ magazines as real objects

    std::array<float, 6> poseForScript(const RE::NiTransform& loose) noexcept
    {
        const auto angles = anglesFor(loose.rotate);
        return { loose.translate.x, loose.translate.y, loose.translate.z, angles[0], angles[1], angles[2] };
    }

    std::int32_t g_nextCasingToken = kCasingTokenBase;

    std::deque<Casing> g_casings;

    namespace
    {
        struct CasingThrow
        {
            std::int32_t      token{ 0 };
            RE::NiPoint3      way{};
            float             spin{ 0.0f };
        };
        std::deque<CasingThrow> g_casingWays;   // asked for, not here yet: which way each is thrown

        struct Flying
        {
            std::uint32_t     ref{ 0 };
            RE::NiPoint3      way{};
            float             spin{ 0.0f };
            Clock::time_point since{};
        };
        std::vector<Flying> g_flying;   // being thrown by ROCK right now
    }

    RE::NiPoint3 palmGripPoint(const RE::NiAVObject* object, const RE::NiPoint3& palm) noexcept
    {
        const auto c = object->worldBound.center;
        const float r = object->worldBound.fRadius;
        const auto off = palm - c;
        const float d = distance(palm, c);
        if (r <= 0.0f || d <= 0.8f * r) {
            return d <= 0.8f * r ? palm : c;
        }
        return c + off * (0.8f * r / d);
    }

    void throwCasings(Clock::time_point now) noexcept
    {
        const auto drive = std::chrono::milliseconds(std::max(0, g_settings.casingThrowMs));
        for (auto it = g_flying.begin(); it != g_flying.end();) {
            auto* ref = now - it->since < drive ? lookupRef(it->ref) : nullptr;
            auto* node = ref ? ref->Get3D() : nullptr;
            if (!looksLikeObject(node)) {
                it = g_flying.erase(it);
                continue;
            }
            // the target is where the case would be after `tau` at the wanted speed: ROCK gives it that velocity
            constexpr float tau = 0.1f;
            RE::NiTransform target = node->world;
            target.translate = node->world.translate + it->way * (g_settings.casingSpeed * tau);
            const float a = it->spin * tau;
            RE::NiTransform turn = identityTransform();
            setRows(turn, RE::NiPoint3{ 1.0f, 0.0f, 0.0f }, RE::NiPoint3{ 0.0f, std::cos(a), std::sin(a) }, RE::NiPoint3{ 0.0f, -std::sin(a), std::cos(a) });
            target = compose(target, turn);
            const auto pose = toLinkPose(target);
            if (!rocklink::driveLoose(it->ref, pose.rotate, pose.translate, tau, "a thrown case")) {
                it = g_flying.erase(it);   // ROCK cannot: the Havok push from the script is all it gets
                continue;
            }
            ++it;
        }
    }

    RE::TESAmmo* g_casingModelFor{ nullptr };

    bool g_casingModelReady{ false };

    bool g_throwAmmoItself{ false };

    bool g_casingModelIsAmmo{ false };

    bool prepareCasingModel(const Gun& gun) noexcept
    {
        auto* ammo = gun.instance && readable(gun.instance, sizeof(RE::TESObjectWEAP::InstanceData)) ? gun.instance->ammo : nullptr;
        if (g_casingModelFor == ammo && g_casingModelIsAmmo == g_throwAmmoItself) {
            return g_casingModelReady;
        }
        g_casingModelFor = ammo;
        g_casingModelIsAmmo = g_throwAmmoItself;
        g_casingModelReady = false;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = data ? data->LookupForm(kCasingForm, "ManualReloadF.esp") : nullptr;
        if (!form || !ammo || !readable(ammo, sizeof(RE::TESAmmo))) {
            note("Casing: %s", !form ? "MRF_MagCasing was not found in ManualReloadF.esp" : "the gun has no ammo");
            return false;
        }
        const char* m = ammo->shellCasing.GetModel();
        if (g_throwAmmoItself) {   // the alien blaster's cell: the ammo's own model is thrown out, it has no case
            m = static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*ammo)).GetModel();
        }
        if (!m || !*m) {
            note("Casing: ammo %08X has no case model: nothing is thrown out", ammo->GetFormID());
            return false;
        }
        auto* misc = form->As<RE::TESObjectMISC>();
        if (!misc) {
            return false;
        }
        static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*misc)).SetModel(m);
        g_casingModelReady = true;
        note("Casing: model '%s' (ammo %08X)", m, ammo->GetFormID());
        return true;
    }

    // `count` cases (or unfired rounds) out of the gun at `at`, thrown along `way` (world), a little apart side by side

    void ejectCasings(const Gun& gun, const RE::NiAVObject* root, const RE::NiPoint3& at, const RE::NiPoint3& way, std::uint32_t count, const char* why) noexcept
    {
        if (!looksLikeObject(root)) {
            return;
        }
        count = std::min(count, 8u);
        std::vector<RE::NiPoint3> points;
        for (std::uint32_t i = 0; i < count; ++i) {
            const float spread = static_cast<float>(i) - 0.5f * static_cast<float>(count - 1);
            // well apart, or the physics throws two cases born inside each other far away
            points.push_back(at + axisOf(root->world, 0) * (spread * 2.0f * gunScale(root)) + axisOf(root->world, 2) * (0.6f * static_cast<float>(i % 2) * gunScale(root)));
        }
        ejectCasingsFrom(gun, root, points, way, false, why);
    }

    // one case out of each point: `straight` = out of a chamber's tunnel along its line (break actions, revolvers), not
    // tossed up and back like out of an ejection port

    void ejectCasingsFrom(const Gun& gun, const RE::NiAVObject* root, const std::vector<RE::NiPoint3>& at, const RE::NiPoint3& way, bool straight, const char* why) noexcept
    {
        if (!g_settings.casings || at.empty() || !looksLikeObject(root) || !prepareCasingModel(gun)) {
            return;
        }
        const auto dir = normalized(way);
        const std::size_t count = std::min<std::size_t>(at.size(), 8);
        for (std::size_t i = 0; i < count; ++i) {
            RE::NiTransform pose = root->world;
            pose.translate = at[i];
            PendingCommand command;
            command.type = kCasing;
            command.kind = kCasingKind;
            command.token = g_nextCasingToken--;
            if (g_nextCasingToken < -900000) {
                g_nextCasingToken = kCasingTokenBase;
            }
            command.pose = poseForScript(pose);
            const float spread = static_cast<float>(i) - 0.5f * static_cast<float>(count - 1);
            const float wobble = straight ? 0.0f : 0.15f * spread;
            command.impulse = { dir.x + wobble, dir.y, dir.z, g_settings.casingImpulse };
            pushCommand(command);
            const float jitter = 0.12f * static_cast<float>(std::abs(command.token * 7919) % 11 - 5) / 5.0f;
            RE::NiPoint3 way2{};
            if (straight) {
                // slides out of its tunnel along the barrel's line, a touch apart; slower than out of a port
                const float speed = g_settings.casingSpeed > 0.0f ? g_settings.chamberCaseSpeed / g_settings.casingSpeed : 0.0f;
                way2 = normalized(dir + axisOf(root->world, 0) * (0.1f * spread)) * speed;
            } else {
                // a case flies out of the port a little up and back, each a little differently, tumbling
                way2 = normalized(dir + axisOf(root->world, 2) * 0.35f + axisOf(root->world, 1) * (-0.15f + jitter));
            }
            g_casingWays.push_back(CasingThrow{ command.token, way2, (straight ? 0.3f : 1.0f) * g_settings.casingSpin * (i % 2 ? -1.0f : 1.0f) * (1.0f + jitter) });
            while (g_casingWays.size() > 32) {
                g_casingWays.pop_front();
            }
        }
        note("CASINGS: %u out (%s)%s", static_cast<unsigned>(count), why, straight ? ", each out of its own chamber" : "");
    }

    BeltRound g_beltRound;

    int g_roundOrphans = 0;   // asked for and given up before they came: deleted when they come

    void dropHandRound(const char* why) noexcept
    {
        if (g_beltRound.ref) {
            if (rocklink::heldBy(freeIndex()) == g_beltRound.ref) {
                rocklink::releaseHeld(freeIndex(), g_beltRound.ref);
            }
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_beltRound.ref;
            pushCommand(command);
        } else if (g_beltRound.asked) {
            ++g_roundOrphans;
        }
        if (g_beltRound.asked) {
            note("ROUND: the real round %08X is gone (%s)", g_beltRound.ref, why);
        }
        g_beltRound = BeltRound{};
    }

    HandleProxy g_handleProxy;
    int g_proxyOrphans = 0;

    void dropHandleProxy(const char* why) noexcept
    {
        auto& px = g_handleProxy;
        if (px.ref) {
            if (rocklink::heldBy(freeIndex()) == px.ref) {
                rocklink::releaseHeld(freeIndex(), px.ref);
            }
            PendingCommand command;
            command.type = kDelete;
            command.ref = px.ref;
            pushCommand(command);
        } else if (px.asked) {
            ++g_proxyOrphans;
        }
        if (px.asked) {
            note("HANDLE: the handle proxy %08X is gone (%s)", px.ref, why);
        }
        const bool spent = px.spent;
        px = HandleProxy{};
        px.spent = spent;
    }

    bool handleProxyGun(std::uint32_t weapon) noexcept
    {
        const auto& list = g_settings.handleProxyGuns;
        if (list.find('*') != std::string::npos) {
            return true;
        }
        std::size_t i = 0;
        while (i < list.size()) {
            const auto end = list.find_first_of(",; ", i);
            const auto item = list.substr(i, end == std::string::npos ? std::string::npos : end - i);
            if (!item.empty() && static_cast<std::uint32_t>(std::strtoul(item.c_str(), nullptr, 16)) == weapon) {
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            i = end + 1;
        }
        return false;
    }

    std::vector<LooseRound> g_looseRounds;

    void deleteRef(std::uint32_t ref) noexcept
    {
        PendingCommand command;
        command.type = kDelete;
        command.ref = ref;
        pushCommand(command);
    }

    void keepHandRound(const char* why) noexcept
    {
        if (!(g_beltRound.ref && g_beltRound.held)) {
            dropHandRound(why);
            return;
        }
        g_looseRounds.push_back(LooseRound{ g_beltRound.ref, g_beltRound.weapon, std::max(g_beltRound.rounds, 1u), Clock::now() });
        note("ROUND: the real round %08X stays (%s): %u round(s) for %08X", g_beltRound.ref, why, g_beltRound.rounds, g_beltRound.weapon);
        while (g_looseRounds.size() > 6) {
            deleteRef(g_looseRounds.front().ref);
            g_looseRounds.erase(g_looseRounds.begin());
        }
        g_beltRound = BeltRound{};
    }

    std::uint32_t looseRoundsFor(std::uint32_t weapon) noexcept
    {
        std::uint32_t n = 0;
        for (const auto& r : g_looseRounds) {
            n += r.weapon == weapon ? r.rounds : 0u;
        }
        return n;
    }

    void tidyLooseRounds(Clock::time_point now) noexcept
    {
        const auto held = rocklink::heldBy(freeIndex());
        for (auto it = g_looseRounds.begin(); it != g_looseRounds.end();) {
            if (it->ref != held && now - it->born > std::chrono::seconds(180)) {
                deleteRef(it->ref);
                it = g_looseRounds.erase(it);
            } else {
                ++it;
            }
        }
    }

    // the model of MRF_MagHeldRound: a missile, a mini nuke, a cell: the ammo's own model; a shell: its case

    bool prepareHeldRoundModel(const Gun& gun, bool big) noexcept
    {
        auto* ammo = gun.instance && readable(gun.instance, sizeof(RE::TESObjectWEAP::InstanceData)) ? gun.instance->ammo : nullptr;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = data ? data->LookupForm(kHeldRoundForm, "ManualReloadF.esp") : nullptr;
        auto* misc = form ? form->As<RE::TESObjectMISC>() : nullptr;
        if (!misc || !ammo || !readable(ammo, sizeof(RE::TESAmmo))) {
            note("ROUND: %s", !misc ? "MRF_MagHeldRound is not in ManualReloadF.esp (old plugin?)" : "the gun has no ammo");
            return false;
        }
        const char* casing = ammo->shellCasing.GetModel();
        const char* world = static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*ammo)).GetModel();
        const char* model = big ? (world && *world ? world : casing) : (casing && *casing ? casing : world);
        if (!model || !*model) {
            return false;
        }
        static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*misc)).SetModel(model);
        detail("ROUND: the round in the hand is '%s'", model);
        return true;
    }

    std::string g_genericVisual;

    std::string g_genericFormModel;

    namespace
    {
        // the carrier: a vanilla magazine with its own collision, about the size of a rifle's
        constexpr const char* kCarrierModel = "Weapons\\MachineGun\\MGMagStraight.nif";
        constexpr const char* kVisualName = "MRF_Visual";

        RE::NiPointer<RE::NiNode> loadModel(const std::string& path) noexcept
        {
            RE::BSModelDB::DBTraits::ArgsType args{};
            args.prepareAfterLoad = 1;
            args.loadTextures = 1;
            RE::NiPointer<RE::NiNode> node;
            RE::BSModelDB::Demand(path.c_str(), &node, args);
            return node;
        }

        bool hasCollision(RE::NiAVObject* root) noexcept
        {
            bool found = false;
            forEachNode(root, [&](RE::NiAVObject* object, int) {
                if (object->collisionObject) {
                    found = true;
                }
                return !found;
            });
            return found;
        }
    }

    void dressCarriers() noexcept
    {
        for (auto& [refId, loose] : g_loose) {
            if (loose.visual.empty()) {
                continue;
            }
            auto* ref = lookupRef(refId);
            auto* root = ref ? ref->Get3D() : nullptr;
            auto* node = looksLikeObject(root) ? root->IsNode() : nullptr;
            if (!node) {
                continue;
            }
            auto& kids = node->GetRuntimeData().children;
            bool dressed = false;
            for (auto& kid : kids) {
                if (kid && kid->name == kVisualName) {
                    dressed = true;
                }
            }
            if (dressed) {
                continue;
            }
            auto visual = loadModel(loose.visual);
            if (!visual) {
                note("Magazine: '%s' could not be loaded for carrier %08X", loose.visual.c_str(), refId);
                loose.visual.clear();
                continue;
            }
            for (auto& kid : kids) {   // the carrier's own look: hidden, its collision stays
                if (kid && looksLikeObject(kid.get())) {
                    setCulled(kid.get(), true);
                }
            }
            visual->name = kVisualName;
            visual->local.MakeIdentity();
            visual->local.translate = loose.visualAt * -1.0f;
            node->AttachChild(visual.get(), true);
            note("Magazine: carrier %08X wears '%s' (its middle %.1f from the model's origin)", refId, loose.visual.c_str(),
                std::sqrt(dot(loose.visualAt, loose.visualAt)));
        }
    }

    const std::string& vanillaCarrierVisual(int kind) noexcept
    {
        static std::array<std::string, 30> visual;
        static std::array<bool, 30> checked{};
        static const std::string none;
        if (!g_settings.carrierMags || !g_settings.carrierVanilla || kind < 0 || kind >= 30 || kind == kMinigunKind) {
            return none;
        }
        const auto k = static_cast<std::size_t>(kind);
        if (!checked[k]) {
            checked[k] = true;
            const RE::TESFormID id = kind < 2 ? 0x800 + kind : (kind <= 18 ? 0x80F + (kind - 2) : 0x820 + (kind - 19));
            auto* data = RE::TESDataHandler::GetSingleton();
            auto* form = data ? data->LookupForm(id, "ManualReloadF.esp") : nullptr;
            auto* misc = form ? form->As<RE::TESObjectMISC>() : nullptr;
            const char* path = misc ? static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*misc)).GetModel() : nullptr;
            if (path && *path) {
                static std::array<std::string, 30> kept;   // SetModel keeps the pointer's text
                kept[k] = path;
                auto loaded = loadModel(kept[k]);
                if (loaded && !hasCollision(loaded.get())) {
                    visual[k] = kept[k];
                    static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*misc)).SetModel(kCarrierModel);
                }
                note("Magazine: the vanilla magazine of kind %d is '%s'%s", kind, kept[k].c_str(),
                    visual[k].empty() ? " (it has its own collision)" : " (no collision in it: a real one is a carrier wearing it)");
            }
        }
        return visual[k];
    }

    bool prepareGenericMagModel(const std::string& model) noexcept
    {
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = data ? data->LookupForm(kGenericMagForm, "ManualReloadF.esp") : nullptr;
        auto* misc = form ? form->As<RE::TESObjectMISC>() : nullptr;
        if (!misc || model.empty()) {
            note("Magazine: %s", !misc ? "MRF_MagGeneric is not in ManualReloadF.esp (old plugin?)" : "no model");
            return false;
        }
        static std::string kept;   // SetModel keeps the pointer's text: hold it for good
        kept = model.rfind("Meshes\\", 0) == 0 || model.rfind("meshes\\", 0) == 0 ? model.substr(7) : model;
        g_genericVisual.clear();
        if (g_settings.carrierMags) {
            auto loaded = loadModel(kept);
            if (loaded && !hasCollision(loaded.get())) {
                g_genericVisual = kept;
            }
        }
        g_genericFormModel = kept;
        static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*misc)).SetModel(g_genericVisual.empty() ? kept.c_str() : kCarrierModel);
        note("Magazine: a gun of another mod, its magazine is '%s'%s", kept.c_str(),
            g_genericVisual.empty() ? "" : " (no collision in it: a real one is a carrier wearing it)");
        return true;
    }

    void tidyCasings(Clock::time_point now) noexcept
    {
        while (!g_casings.empty() && (static_cast<int>(g_casings.size()) > std::max(1, g_settings.casingMax) ||
                                      now - g_casings.front().born > std::chrono::milliseconds(static_cast<int>(g_settings.casingSeconds * 1000.0f)))) {
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_casings.front().ref;
            pushCommand(command);
            g_casings.pop_front();
        }
    }

    // A real magazine where the gun's magazine node would stand at `node`.

    std::int32_t spawnLoose(const RE::NiTransform& node, std::uint32_t rounds, const char* why) noexcept
    {
        const auto loose = compose(node, g_state.looseInNode);
        const auto token = g_nextToken++;
        g_pending[token] = Pending{ rounds, g_state.kind, loose.rotate };
        if (g_state.kind == kGenericKind && !g_genericVisual.empty()) {
            g_pending[token].visual = g_genericVisual;
            g_pending[token].visualAt = g_state.looseInNode.translate;   // the carrier's root is the model's middle
        } else if (g_state.kind != kGenericKind && !vanillaCarrierVisual(g_state.kind).empty()) {
            g_pending[token].visual = vanillaCarrierVisual(g_state.kind);
            g_pending[token].visualAt = g_state.carrierAt;   // Rail put the carrier's root at the model's middle
        }
        PendingCommand command;
        command.type = kSpawn;
        command.kind = g_state.kind;
        if (g_state.kind == kGenericKind && g_settings.magSlots) {
            command.kind = magSlotKind(g_state.magModel, g_genericVisual.empty() ? g_genericFormModel.c_str() : kCarrierModel, !g_genericVisual.empty());   // a form of its own kind / its own
        }
        command.token = token;
        command.pose = poseForScript(loose);
        command.scale = loose.scale > 0.05f && loose.scale < 20.0f ? loose.scale : 1.0f;
        {
            std::scoped_lock guard(g_shared.lock);
            g_shared.fresh[token] = command.pose;
        }
        pushCommand(command);
        note("Spawn magazine kind %d with %u rounds (%s) at (%.0f, %.0f, %.0f)", g_state.kind, rounds, why, loose.translate.x, loose.translate.y, loose.translate.z);
        return token;
    }

    void setFresh(std::int32_t token, const RE::NiTransform& node) noexcept
    {
        const auto pose = poseForScript(compose(node, g_state.looseInNode));
        std::scoped_lock guard(g_shared.lock);
        if (const auto it = g_shared.fresh.find(token); it != g_shared.fresh.end()) {
            it->second = pose;
        }
    }

    void deleteMagazine(std::uint32_t ref) noexcept
    {
        PendingCommand command;
        command.type = kDelete;
        command.ref = ref;
        pushCommand(command);
        g_loose.erase(ref);
    }

    RoundState g_round;

    void hideRound() noexcept
    {
        if (g_round.ref) {
            if (auto* ref = lookupRef(g_round.ref); ref && looksLikeObject(ref->Get3D())) {
                setCulled(ref->Get3D(), true);
            }
        }
    }

    void takeRound(std::uint32_t ref) noexcept
    {
        if (g_round.ref && g_round.ref != ref) {
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_round.ref;
            pushCommand(command);
        }
        g_round.ref = ref;
        g_round.asked = false;
        g_round.missing = false;
        note("Round: object %08X is here", ref);
    }

    void dropRound() noexcept
    {
        if (g_round.ref) {
            PendingCommand command;
            command.type = kDelete;
            command.ref = g_round.ref;
            pushCommand(command);
        }
        g_round.ref = 0;
        g_round.asked = false;
    }

    // The model of the drawn round: sRoundModel, else the casing of the gun's ammo, else its world model.

    bool prepareRoundModel(const Gun& gun) noexcept
    {
        auto* ammo = gun.instance && readable(gun.instance, sizeof(RE::TESObjectWEAP::InstanceData)) ? gun.instance->ammo : nullptr;
        if (g_round.modelReady && g_round.modelFor == ammo) {
            return true;
        }
        g_round.modelFor = ammo;
        g_round.modelReady = false;
        auto* data = RE::TESDataHandler::GetSingleton();
        auto* form = data ? data->LookupForm(kRoundForm, "ManualReloadF.esp") : nullptr;
        auto* stat = form ? form->As<RE::TESObjectSTAT>() : nullptr;
        if (!stat) {
            note("Round: MRF_Round (%03X) was not found in ManualReloadF.esp", kRoundForm);
            return false;
        }
        std::string casing;
        std::string world;
        if (ammo && readable(ammo, sizeof(RE::TESAmmo))) {
            if (const char* m = ammo->shellCasing.GetModel(); m && *m) casing = m;
            if (const char* m = static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*ammo)).GetModel(); m && *m) world = m;
        }
        std::string model = !g_settings.roundModel.empty() ? g_settings.roundModel : !casing.empty() ? casing : world;
        note("Round: ammo %08X casing '%s', world model '%s' -> using '%s'", ammo ? ammo->GetFormID() : 0u, casing.c_str(), world.c_str(), model.c_str());
        if (model.empty()) {
            return false;
        }
        static_cast<RE::TESModel&>(static_cast<RE::BGSModelMaterialSwap&>(*stat)).SetModel(model.c_str());
        g_round.modelReady = true;
        if (g_round.ref) {
            dropRound();   // made with another model: make it again
        }
        return true;
    }

    void drawRound(const Gun& gun, RE::NiAVObject* root) noexcept
    {
        auto& st = g_state;
        if (!g_settings.showRound || !looksLikeObject(root) || !st.railKnown) {
            if (g_round.ref) {
                if (auto* ref = lookupRef(g_round.ref); ref && looksLikeObject(ref->Get3D())) {
                    setCulled(ref->Get3D(), true);
                }
            }
            return;
        }
        const bool open = st.slideLocked || st.rackPull >= g_settings.rackTravel * std::clamp(g_settings.roundOpenAt, 0.05f, 1.0f);
        const bool inChamber = (st.chamber || st.jam == 2) && !st.rackCycled;   // a stuck case is seen in the port
        const bool onMagazine = !inChamber && st.place == kSeated && st.rounds > 0;
        const bool jammed = st.jam != 0 && st.place == kSeated;   // a jam is always seen: it is no bug, the slide must be racked
        const bool visible = jammed || (open && (inChamber || onMagazine));

        if (!g_round.ref) {
            const auto now = Clock::now();
            if (!g_round.asked || now - g_round.askedAt > std::chrono::seconds(5)) {
                if (prepareRoundModel(gun)) {
                    PendingCommand command;
                    command.type = kRound;
                    pushCommand(command);
                    g_round.asked = true;
                    g_round.askedAt = now;
                }
            }
            return;
        }
        auto* ref = lookupRef(g_round.ref);
        auto* model = ref ? ref->Get3D() : nullptr;
        if (!looksLikeObject(model)) {
            const auto now = Clock::now();
            if (!g_round.missing) {
                g_round.missing = true;
                g_round.missingSince = now;
            } else if (now - g_round.missingSince > std::chrono::seconds(3)) {
                note("Round: object %08X has no 3D, made again", g_round.ref);
                dropRound();
                g_round.missing = false;
            }
            return;
        }
        g_round.missing = false;
        if (!visible) {
            setCulled(model, true);
            return;
        }
        // the magazine's top in the gun's frame, then up / forward to the chamber
        const float scale = gunScale(root);
        auto top = compose(root->world, st.magRest);
        const auto up = axisOf(top, 2);
        RE::NiPoint3 at = top.translate + up * ((st.topAt + g_settings.roundUp) * top.scale);
        if (inChamber || jammed) {
            at += axisOf(root->world, 2) * (g_settings.chamberUp * scale) + axisOf(root->world, 1) * (g_settings.chamberForward * scale);
        }
        if (jammed) {
            // the stuck case stands up out of the port; the misfed round lies nosed up against the breech
            at += axisOf(root->world, 2) * ((st.jam == 2 ? g_settings.jamOut : g_settings.jamOut * 0.5f) * scale);
        }
        const float d = 0.01745329f;
        Mat turn = axisRotation(0, g_settings.roundRotX * d);
        turn = mul(turn, axisRotation(1, g_settings.roundRotY * d));
        turn = mul(turn, axisRotation(2, g_settings.roundRotZ * d));
        RE::NiTransform pose = identityTransform();
        if (jammed) {
            turn = mul(turn, axisRotation(0, (st.jam == 2 ? g_settings.jamTilt : g_settings.jamTilt * 0.4f) * d));
        }
        pose.rotate = fromMat(mul(turn, toMat(root->world.rotate)));
        pose.translate = at;
        pose.scale = scale * (g_settings.roundScale > 0.0f ? g_settings.roundScale : 1.0f);
        setCulled(model, false);
        placeNode(model, pose);
        if (!g_round.logged) {
            g_round.logged = true;
            note("Round: drawn %s, bound radius %.2f", inChamber ? "in the chamber" : "on the magazine", model->worldBound.fRadius);
        }
    }

    void takeScriptNews() noexcept
    {
        std::vector<NewMag> mags;
        {
            std::scoped_lock guard(g_shared.lock);
            mags.swap(g_shared.newMags);
        }
        for (const auto& mag : mags) {
            if (mag.token == kRoundToken) {
                takeRound(mag.ref);
                continue;
            }
            if (mag.token == kProxyToken) {
                auto& px = g_handleProxy;
                if (g_proxyOrphans > 0 || !px.asked || px.ref) {
                    g_proxyOrphans = std::max(0, g_proxyOrphans - 1);
                    deleteRef(mag.ref);
                    detail("HANDLE: proxy %08X came too late: deleted", mag.ref);
                    continue;
                }
                px.ref = mag.ref;
                px.grabAt = Clock::time_point{};
                note("HANDLE: proxy %08X is here (%.2f s)", mag.ref, std::chrono::duration<float>(Clock::now() - px.askedAt).count());
                continue;
            }
            if (mag.token == kPoseToken) {
                if (g_roundOrphans > 0 || !g_beltRound.asked || g_beltRound.ref) {
                    g_roundOrphans = std::max(0, g_roundOrphans - 1);
                    PendingCommand command;
                    command.type = kDelete;
                    command.ref = mag.ref;
                    pushCommand(command);
                    detail("ROUND: %08X came too late: deleted", mag.ref);
                    continue;
                }
                g_beltRound.ref = mag.ref;
                g_beltRound.grabAt = Clock::time_point{};   // asked for at once
                note("ROUND: real round %08X is here (%.2f s)", mag.ref, std::chrono::duration<float>(Clock::now() - g_beltRound.askedAt).count());
                continue;
            }
            if (mag.token <= kCasingTokenBase) {
                if (g_state.weapon) {
                    rocklink::clearOfGun(mag.ref, g_state.weapon->GetFormID(), true);   // out of the port past the hands
                }
                Casing casing{ mag.ref, Clock::now() };
                for (auto it = g_casingWays.begin(); it != g_casingWays.end(); ++it) {
                    if (it->token == mag.token) {
                        casing.way = it->way;
                        casing.spin = it->spin;
                        if (g_settings.casingSpeed > 0.0f) {
                            g_flying.push_back(Flying{ mag.ref, it->way, it->spin, Clock::now() });
                        }
                        g_casingWays.erase(it);
                        break;
                    }
                }
                g_casings.push_back(casing);
                continue;
            }
            if (g_state.weapon) {
                rocklink::clearOfGun(mag.ref, g_state.weapon->GetFormID(), false);   // made where the gun's magazine was
            }
            Loose loose;
            loose.born = Clock::now();
            if (const auto it = g_pending.find(mag.token); it != g_pending.end()) {
                loose.rounds = it->second.rounds;
                loose.kind = it->second.kind;
                loose.visual = it->second.visual;
                loose.visualAt = it->second.visualAt;
                auto* ref = lookupRef(mag.ref);
                learnConvention(ref, ref ? ref->Get3D() : nullptr);
                g_pending.erase(it);
            }
            {
                std::scoped_lock guard(g_shared.lock);
                g_shared.fresh.erase(mag.token);
            }
            if (g_state.dropActive && g_state.dropToken == mag.token) {
                g_state.dropActive = false;
            }
            g_loose[mag.ref] = loose;
            note("Magazine %08X is out: %u rounds", mag.ref, loose.rounds);
            auto& st = g_state;
            if (st.handToken != 0 && mag.token == st.handToken) {
                st.handToken = 0;
                if (st.handTokenDropped) {
                    st.handSpare = false;
                    if (st.handStash) {
                        deleteMagazine(mag.ref);
                        hud("Magazine stowed, rounds returned to inventory");
                    }
                } else if (st.place == kHand || st.handSpare) {
                    // the real one is in the palm: the gun's own magazine is hidden, ROCK closes the hand on the real one
                    if (st.place == kHand) {
                        st.place = kGone;
                        st.rounds = 0;
                    }
                    st.handSpare = false;
                    st.velocityValid = false;
                    griphook::setOwned(false);
                    note("HAND: magazine %08X is now held as a real object (ROCK)", mag.ref);
                    if (g_settings.core2) {
                        st.handRef = mag.ref;
                        st.handRefAt = Clock::now();
                        st.grabTriedAt = st.handRefAt;
                        st.grabTries = 1;
                        st.grabDone = false;
                        const auto& hand = g_hands[freeIndex()];
                        const auto& palm = hand.palm;
                        float point[3]{ palm.x, palm.y, palm.z };
                        auto* ref = lookupRef(mag.ref);
                        auto* mag3D = ref ? ref->Get3D() : nullptr;
                        if (const auto held = rocklink::heldBy(freeIndex()); held != 0 && held != mag.ref) {
                            rocklink::releaseHeld(freeIndex(), held);
                        }
                        st.handRefInWandValid = false;
                        if (looksLikeObject(mag3D)) {
                            // held where the palm already is (the nearest point of the magazine), so ROCK does not pull its
                            // middle into the palm: no jump. Until ROCK has it, it is kept where it came in the hand.
                            const auto grip = palmGripPoint(mag3D, palm);
                            point[0] = grip.x;
                            point[1] = grip.y;
                            point[2] = grip.z;
                            if (hand.valid) {
                                st.handRefInWand = relative(hand.wand, mag3D->world);
                                st.handRefInWandValid = true;
                            }
                        }
                        rocklink::forceGrab(freeIndex(), mag.ref, point);
                    }
                } else {
                    deleteMagazine(mag.ref);   // it was already pushed into the well
                }
            }
        }
    }

    void forgetLastSession() noexcept
    {
        g_casings.clear();
        g_casingWays.clear();
        g_flying.clear();
        g_looseRounds.clear();
        g_beltRound = BeltRound{};
        g_roundOrphans = 0;
        g_handleProxy = HandleProxy{};
        g_proxyOrphans = 0;
        g_round = RoundState{};
        g_nextCasingToken = kCasingTokenBase;
        {
            std::scoped_lock guard(g_shared.lock);
            g_shared.commands.clear();
            g_shared.current = PendingCommand{};
            g_shared.newMags.clear();
            g_shared.fresh.clear();
        }
        note("Loaded: objects of the last session are forgotten");
    }
}
