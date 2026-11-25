#include "AnimHook.h"

#include "Log.h"
#include "Settings.h"

#include <RE/Fallout.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mrf::animhook
{
    namespace
    {
        using Notify_t = bool (*)(RE::IAnimationGraphManagerHolder*, const RE::BSFixedString&);

        Notify_t           g_original{ nullptr };
        void**             g_slot{ nullptr };
        std::atomic<bool>  g_blocking{ false };
        std::atomic<long long> g_blockingSetMs{ 0 };   // when the plugin's frame last said so: an old word does not count

        long long nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // refused only while the plugin's frame keeps saying so: a frame that stopped (a fault, a menu, the plugin
        // gone quiet) leaves the game's own reload working
        bool blockingNow()
        {
            return g_blocking && nowMs() - g_blockingSetMs < 500;
        }
        std::atomic<unsigned> g_blocked{ 0 };
        std::atomic<bool>  g_refused{ false };
        std::atomic<bool>  g_started{ false };
        std::mutex         g_seenLock;
        std::unordered_map<std::string, unsigned> g_seen;

        std::string lower(const char* text)
        {
            std::string out = text ? text : "";
            for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        // the events that start the game's reload (names as the behavior graphs of Fallout 4 use them)
        bool startsReload(const std::string& name)
        {
            return name.rfind("reloadstart", 0) == 0 || name == "reload" || name == "weaponreload";
        }

        bool hooked(RE::IAnimationGraphManagerHolder* self, const RE::BSFixedString& event)
        {
            const char* text = event.c_str();
            const auto name = lower(text);
            const bool reloadish = name.find("reload") != std::string::npos;
            {
                std::lock_guard lock(g_seenLock);
                auto& count = g_seen[name];
                ++count;
                // every name once, then reload events (a few) and everything else with bTraceAnim
                if (count == 1 || (reloadish && count <= 40) || (g_settings.traceAnim && count <= 200)) {
                    note("ANIM: event '%s'%s", text ? text : "", blockingNow() ? " (gun reloaded by hand)" : "");
                }
            }
            if (blockingNow() && g_settings.blockReload && g_settings.blockAnimReload && startsReload(name)) {
                const auto n = ++g_blocked;
                g_refused = true;
                if (n <= 20 || n % 50 == 0) {
                    note("ANIM: the game's reload '%s' is refused (the gun is reloaded by hand; %u so far)", text ? text : "", n);
                }
                return false;
            }
            if (startsReload(name)) {
                g_started = true;
            }
            return g_original ? g_original(self, event) : false;
        }
    }

    void install(RE::PlayerCharacter* player) noexcept
    {
        if (g_slot || !player) {
            return;
        }
        auto* holder = static_cast<RE::IAnimationGraphManagerHolder*>(player);
        auto** table = *reinterpret_cast<void***>(holder);
        if (!table) {
            note("ANIM: the player has no animation vtable; the game's reload cannot be stopped");
            return;
        }
        void** slot = &table[1];
        g_original = reinterpret_cast<Notify_t>(*slot);
        void* const replacement = reinterpret_cast<void*>(&hooked);
        REL::safe_write(reinterpret_cast<std::uintptr_t>(slot), &replacement, sizeof(replacement));
        g_slot = slot;
        note("ANIM: the player's animation events are watched (NotifyAnimationGraph, vtable %p slot 1, was %p)", static_cast<void*>(table),
            reinterpret_cast<void*>(g_original));
    }

    bool installed() noexcept { return g_slot != nullptr; }

    void setBlocking(bool value) noexcept
    {
        g_blockingSetMs = nowMs();
        if (g_blocking.exchange(value) != value) {
            g_blocked = 0;
            note("ANIM: the game's reload is %s", value ? "refused (gun reloaded by hand)" : "let through");
        }
    }

    unsigned blockedCount() noexcept { return g_blocked; }

    bool takeRefused() noexcept { return g_refused.exchange(false); }

    bool takeStarted() noexcept { return g_started.exchange(false); }
}
