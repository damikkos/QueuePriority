#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "graft/engine.hpp"
#include "graft/native.hpp"
#include "graft/scan.hpp"
#include "rules.hpp"

GRAFT_PLUGIN("QueuePriority", 3);

namespace
{
    struct Player
    {
        std::uint32_t Id;
        std::uint8_t Unknown[0xAA];
        std::uint8_t Priority;
    };

    struct Queue
    {
        std::uint8_t Unknown[0x20];
        void* Server;
        Player** Players;
        std::int32_t Capacity;
        std::int32_t Size;
    };

    static_assert(offsetof(Player, Priority) == 0xAE);
    static_assert(offsetof(Queue, Server) == 0x20);
    static_assert(offsetof(Queue, Players) == 0x28);
    static_assert(offsetof(Queue, Size) == 0x34);

    using AddFn = void (*)(Queue*, std::uint32_t, const char*, const char*);
    using ProcessFn = void (*)(Queue*);
    using CountFn = int (*)(void*);
    using DestroyFn = std::int64_t (*)(void*, std::uint32_t);

    AddFn g_Add = nullptr;
    ProcessFn g_Process = nullptr;
    CountFn g_MaxPlayers = nullptr;
    CountFn g_OnlinePlayers = nullptr;
    DestroyFn g_Destroy = nullptr;
    const void* g_ProcessReturn = nullptr;
    const void* g_SteamReturn = nullptr;

    qp::Settings g_Settings;
    std::atomic<int> g_Reserved = 0;
    std::optional<std::string> g_Text;
    bool g_Read = false;

    std::mutex g_Lock;
    std::unordered_map<std::uint32_t, std::string> g_Waiting;
    std::unordered_set<std::uint32_t> g_Online;
    Queue* g_Queue = nullptr;

    void Log(std::string_view text)
    {
        graft::log(std::format("[QueuePriority] {}", text));
    }

    std::filesystem::path ConfigPath()
    {
        wchar_t buffer[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        return std::filesystem::path(std::wstring(buffer, length)).parent_path() / L"grafted" / L"QueuePriority.ini";
    }

    void Reload()
    {
        std::optional<std::string> text = qp::ReadConfig(ConfigPath());
        if (g_Read && text == g_Text)
        {
            return;
        }
        g_Read = true;
        g_Text = std::move(text);
        g_Settings = g_Text ? qp::Parse(*g_Text) : qp::Settings{};
        g_Reserved = g_Settings.Reserved;
        if (!g_Text)
        {
            Log("error: grafted/QueuePriority.ini could not be read or created, reserved slots are off");
        }
        for (const std::string& error : g_Settings.Errors)
        {
            Log("error: QueuePriority.ini: " + error);
        }
    }

    int Position(const Queue* queue, std::uint32_t id)
    {
        for (int i = 0; i < queue->Size; ++i)
        {
            if (queue->Players[i]->Id == id)
            {
                return i;
            }
        }
        return -1;
    }

    void OnAdd(Queue* queue, std::uint32_t id, const char* steamId, const char* name)
    {
        g_Add(queue, id, steamId, name);
        Reload();
        if (!steamId || !g_Settings.Ids.contains(steamId))
        {
            return;
        }
        const std::string label = std::format("{} ({})", name ? name : "", steamId);
        const int from = Position(queue, id);
        if (from < 0)
        {
            Log(std::format("error: {} is listed but was not found in the login queue", label));
            return;
        }
        queue->Players[from]->Priority = 1;
        std::rotate(queue->Players, queue->Players + from, queue->Players + from + 1);
        {
            const std::scoped_lock lock{g_Lock};
            g_Waiting[id] = label;
        }
        Log(std::format("{} is first in the login queue, was {}", label, from + 1));
    }

    void OnProcess(Queue* queue)
    {
        g_Queue = queue;
        g_Process(queue);
        g_Queue = nullptr;
        const std::scoped_lock lock{g_Lock};
        std::erase_if(g_Waiting, [queue](const auto& waiting) {
            if (Position(queue, waiting.first) >= 0)
            {
                return false;
            }
            g_Online.insert(waiting.first);
            Log(std::format("{} left the login queue to join, listed players online: {}", waiting.second, g_Online.size()));
            return true;
        });
    }

    int OnMaxPlayers(void* server)
    {
        const int maxPlayers = g_MaxPlayers(server);
        const void* caller = _ReturnAddress();
        if (caller == g_SteamReturn)
        {
            return qp::Public(maxPlayers, g_Reserved);
        }
        if (caller != g_ProcessReturn || !g_Queue || g_Queue->Size <= 0)
        {
            return maxPlayers;
        }
        const std::scoped_lock lock{g_Lock};
        if (g_Waiting.contains(g_Queue->Players[0]->Id))
        {
            return std::min(maxPlayers, g_OnlinePlayers(server));
        }
        return qp::Limit(maxPlayers, g_Reserved, static_cast<int>(g_Online.size()));
    }

    std::int64_t OnDestroy(void* server, std::uint32_t id)
    {
        const std::int64_t result = g_Destroy(server, id);
        const std::scoped_lock lock{g_Lock};
        g_Waiting.erase(id);
        g_Online.erase(id);
        return result;
    }

    std::optional<graft::scan::found> Unique(const std::vector<graft::scan::view>& sections, graft::scan::pattern_view signature)
    {
        std::optional<graft::scan::found> result;
        for (const graft::scan::view& section : sections)
        {
            if (!section.exec)
            {
                continue;
            }
            const std::uintptr_t end = section.base + section.bytes.size();
            for (auto hit = section.find(section.base, section.bytes.size(), signature); hit; hit = section.find(hit->site + 1, end - hit->site - 1, signature))
            {
                if (result)
                {
                    return std::nullopt;
                }
                result = *hit;
            }
        }
        return result;
    }

    std::uintptr_t CallTarget(std::uintptr_t call)
    {
        std::int32_t offset = 0;
        std::memcpy(&offset, reinterpret_cast<const void*>(call + 1), sizeof offset);
        return call + 5 + offset;
    }

    bool Install()
    {
        Reload();
        const std::vector<graft::scan::view> sections = graft::scan::sections_of(GetModuleHandleW(nullptr));
        // Signatures and call offsets below are from DayZServer 1.30.164289.
        const auto add = Unique(sections, graft::scan::sig<"4C 89 4C 24 ?? 89 54 24 ?? 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 C7 45 ?? FE FF FF FF 49 8B F9 4D 8B E8 8B DA 4C 8B F9 B9 40 01 00 00">);
        const auto process = Unique(sections, graft::scan::sig<"48 89 5C 24 20 55 48 83 EC 20 8B 69 34 48 8B D9 48 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 08 85 ED 0F 84 ?? ?? ?? ?? 83 7B 3C 00 48 89 74 24 30 48 89 7C 24 38 4C 89 74 24 40 7F 05 44 8B F5 EB 13 48 8B CB E8 ?? ?? ?? ?? 44 8B 73 3C 44 2B 73 14 44 03 F0 48 8B 73 20 48 8B CE E8 ?? ?? ?? ?? BF 01 00 00 00 48 8B CE 2B F8 E8 ?? ?? ?? ?? 03 F8 85 ED 7E ?? 44 3B F7 41 0F 4E FE 85 FF 7E ?? 48 8B 53 28 48 8B CB 48 8B 12 E8 ?? ?? ?? ?? 48 8B 43 28">);
        const auto destroy = Unique(sections, graft::scan::sig<"40 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 C7 45 ?? FE FF FF FF 48 89 9C 24 ?? ?? ?? ?? 8B DA 48 8B F9 83 FA 02 75 ??">);
        const auto steam = Unique(sections, graft::scan::sig<"48 8B 18 48 8B 3B 49 8B 4E 08 E8 [disp32] 8B D0 48 8B CB FF 57 60">);
        const auto layout = Unique(sections, graft::scan::sig<"45 8B 47 34 45 85 C0 74 ?? 49 8B 57 28 0F 1F 84 00 00 00 00 00 8B C3 48 8B 0C C2 80 B9 AE 00 00 00 00 74 ?? FF C3 41 3B D8 72 ?? 41 8B D8">);
        const std::uintptr_t maxPlayers = process ? CallTarget(process->site + 0x70) : 0;
        const std::uintptr_t onlinePlayers = process ? CallTarget(process->site + 0x61) : 0;
        std::string missing;
        if (!add)
        {
            missing += " queue-add";
        }
        if (!process)
        {
            missing += " queue-process";
        }
        if (!destroy)
        {
            missing += " player-destroy";
        }
        if (!layout)
        {
            missing += " queue-layout";
        }
        if (!maxPlayers || !graft::scan::matches(maxPlayers, graft::scan::sig<"8B 91 ?? ?? ?? ?? 8B 81 ?? ?? ?? ?? 3B D0 0F 4E C2 C3">))
        {
            missing += " max-players";
        }
        if (!onlinePlayers || !graft::scan::matches(onlinePlayers, graft::scan::sig<"8B 81 ?? ?? ?? ?? C3">))
        {
            missing += " online-players";
        }
        if (!steam || steam->site + 15 + static_cast<std::uintptr_t>(steam->value) != maxPlayers)
        {
            missing += " steam-update";
        }
        if (!missing.empty())
        {
            Log("error: unknown DayZServer build, not found:" + missing + "; the plugin does nothing");
            return false;
        }
        g_OnlinePlayers = reinterpret_cast<CountFn>(onlinePlayers);
        g_ProcessReturn = reinterpret_cast<const void*>(process->site + 0x75);
        g_SteamReturn = reinterpret_cast<const void*>(steam->site + 15);
        if (!graft::hook_all({graft::hooked(reinterpret_cast<AddFn>(add->site), &OnAdd, &g_Add),
                              graft::hooked(reinterpret_cast<ProcessFn>(process->site), &OnProcess, &g_Process),
                              graft::hooked(reinterpret_cast<CountFn>(maxPlayers), &OnMaxPlayers, &g_MaxPlayers),
                              graft::hooked(reinterpret_cast<DestroyFn>(destroy->site), &OnDestroy, &g_Destroy)}))
        {
            Log("error: engine hooks were not installed; the plugin does nothing");
            return false;
        }
        return true;
    }

    GRAFT_ON_LOAD()
    {
        Install();
    }
}
