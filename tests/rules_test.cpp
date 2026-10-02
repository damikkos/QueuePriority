#include <chrono>
#include <cstdio>

#include "rules.hpp"

namespace
{
    int g_Failures = 0;

    void Check(bool passed, const char* condition, int line)
    {
        if (!passed)
        {
            std::fprintf(stderr, "rules_test.cpp:%d: %s\n", line, condition);
            g_Failures++;
        }
    }

#define CHECK(condition) Check((condition), #condition, __LINE__)
}

int main()
{
    const qp::Settings settings = qp::Parse("\xEF\xBB\xBF# comment\r\nReservedSlots = 5\r\n; note\r\n\r\nSteamID=76561198000000001, 76561198000000002;76561198000000003\r\nSteamIDs = 76561198000000004 123\r\nPublicSlots=100\r\nSteamID\r\n");
    CHECK(settings.Reserved == 5);
    CHECK(settings.Ids.size() == 4);
    CHECK(settings.Ids.contains("76561198000000001"));
    CHECK(settings.Ids.contains("76561198000000003"));
    CHECK(settings.Ids.contains("76561198000000004"));
    CHECK(settings.Errors.size() == 3);

    CHECK(qp::Parse("").Reserved == 0);
    CHECK(qp::Parse("").Errors.empty());
    CHECK(qp::Parse("ReservedSlots=-1").Errors.size() == 1);
    CHECK(qp::Parse("ReservedSlots=5x").Errors.size() == 1);
    CHECK(qp::Parse("ReservedSlots=").Errors.size() == 1);
    CHECK(qp::Parse("SteamID=7656119800000000").Ids.empty());

    CHECK(qp::Public(40, 5) == 35);
    CHECK(qp::Public(40, 0) == 40);
    CHECK(qp::Public(40, 50) == 0);

    CHECK(qp::Limit(40, 5, 0) == 35);
    CHECK(qp::Limit(40, 5, 2) == 37);
    CHECK(qp::Limit(40, 5, 9) == 40);
    CHECK(qp::Limit(40, 0, 3) == 40);
    CHECK(qp::Limit(40, 40, 0) == 0);
    CHECK(qp::Limit(40, 50, 3) == 3);

    const auto directory = std::filesystem::temp_directory_path() / std::format("QueuePriority-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directory(directory);
    const auto config = directory / "QueuePriority.ini";
    const auto generated = qp::ReadConfig(config);
    CHECK(generated && *generated == qp::DefaultConfig);
    const auto defaults = qp::Parse(generated.value_or(""));
    CHECK(defaults.Reserved == 0 && defaults.Ids.empty() && defaults.Errors.empty());

    const std::string custom = "ReservedSlots=1\nSteamID=76561198000000001\n";
    { std::ofstream file{config, std::ios::binary}; file << custom; }
    CHECK(qp::ReadConfig(config) == custom);
    { std::ofstream file{config}; }
    CHECK(qp::ReadConfig(config) == std::string{});
    CHECK(!qp::ReadConfig(directory / "missing" / "QueuePriority.ini"));
    std::filesystem::remove(config);
    std::filesystem::remove(directory);

    if (g_Failures == 0)
    {
        std::puts("rules_test: ok");
    }
    return g_Failures == 0 ? 0 : 1;
}
