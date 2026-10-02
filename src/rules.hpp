#pragma once

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace qp
{
    inline constexpr std::string_view DefaultConfig = "ReservedSlots=0\nSteamID=\n";

    inline std::optional<std::string> ReadConfig(const std::filesystem::path& path)
    {
        std::ifstream file{path, std::ios::binary};
        if (!file)
        {
            std::ofstream defaults{path, std::ios::binary | std::ios::noreplace};
            defaults << DefaultConfig;
            defaults.close();
            if (!defaults)
            {
                return std::nullopt;
            }
            file.open(path, std::ios::binary);
        }
        if (!file)
        {
            return std::nullopt;
        }
        std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        return file.bad() ? std::nullopt : std::optional{std::move(text)};
    }

    struct Settings
    {
        int Reserved = 0;
        std::unordered_set<std::string> Ids;
        std::vector<std::string> Errors;
    };

    inline std::string_view Trim(std::string_view text)
    {
        const std::size_t first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos)
        {
            return {};
        }
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    inline bool IsSteamId(std::string_view text)
    {
        return text.size() == 17 && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; });
    }

    inline Settings Parse(std::string_view text)
    {
        Settings settings;
        if (text.starts_with("\xEF\xBB\xBF"))
        {
            text.remove_prefix(3);
        }
        while (!text.empty())
        {
            const std::size_t newline = text.find('\n');
            const std::string_view line = Trim(text.substr(0, newline));
            text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
            if (line.empty() || line.front() == '#' || line.front() == ';')
            {
                continue;
            }
            const std::size_t equals = line.find('=');
            const std::string_view key = Trim(line.substr(0, equals));
            const std::string_view value = equals == std::string_view::npos ? std::string_view{} : Trim(line.substr(equals + 1));
            if (equals != std::string_view::npos && key == "ReservedSlots")
            {
                int reserved = 0;
                const auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), reserved);
                if (error != std::errc{} || last != value.data() + value.size() || reserved < 0)
                {
                    settings.Errors.push_back(std::format("ReservedSlots must be a number from 0, got '{}'", value));
                    continue;
                }
                settings.Reserved = reserved;
            }
            else if (equals != std::string_view::npos && (key == "SteamID" || key == "SteamIDs"))
            {
                for (std::size_t at = value.find_first_not_of(" \t,;"); at != std::string_view::npos;)
                {
                    const std::size_t stop = value.find_first_of(" \t,;", at);
                    const std::string_view id = value.substr(at, stop - at);
                    if (IsSteamId(id))
                    {
                        settings.Ids.emplace(id);
                    }
                    else
                    {
                        settings.Errors.push_back(std::format("'{}' is not a 17-digit SteamID", id));
                    }
                    at = value.find_first_not_of(" \t,;", stop);
                }
            }
            else
            {
                settings.Errors.push_back(std::format("unknown line '{}'", line));
            }
        }
        return settings;
    }

    inline int Public(int maxPlayers, int reserved)
    {
        return std::max(0, maxPlayers - reserved);
    }

    inline int Limit(int maxPlayers, int reserved, int listedOnline)
    {
        return std::min(maxPlayers, Public(maxPlayers, reserved) + std::min(listedOnline, reserved));
    }
}
