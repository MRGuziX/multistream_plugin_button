#include "config_io.h"
#include "destination_rules.h"
#include "plugin_state.h"

#include <catch2/catch_all.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path unique_temp_json(const char *suffix)
{
    static std::atomic<uint64_t> seq{0};
    return fs::temp_directory_path() /
           (std::string("obs_multistream_test_") + suffix + "_" + std::to_string(++seq) + ".json");
}

void write_file(const fs::path &p, const std::string &contents)
{
    std::ofstream out(p, std::ios::binary);
    REQUIRE(out);
    out << contents;
}

void clear_plugin_destinations()
{
    g_destinations.clear();
    {
        std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
        g_runtime_statuses.clear();
    }
}

} // namespace

TEST_CASE("load_destinations_from_file: corrupt JSON leaves list empty")
{
    const fs::path p = unique_temp_json("corrupt");
    write_file(p, "{ not json");
    load_destinations_from_file(p.string().c_str());
    REQUIRE(g_destinations.empty());
    clear_plugin_destinations();
    std::error_code ec;
    fs::remove(p, ec);
}

TEST_CASE("load_destinations_from_file: empty file is treated as no config")
{
    const fs::path p = unique_temp_json("empty");
    {
        std::ofstream out(p, std::ios::binary);
        REQUIRE(out);
    }
    load_destinations_from_file(p.string().c_str());
    REQUIRE(g_destinations.empty());
    clear_plugin_destinations();
    std::error_code ec;
    fs::remove(p, ec);
}

TEST_CASE("load_destinations_from_file: empty destinations array")
{
    const fs::path p = unique_temp_json("empty_arr");
    write_file(p, R"({"destinations":[]})");
    load_destinations_from_file(p.string().c_str());
    REQUIRE(g_destinations.empty());
    clear_plugin_destinations();
    std::error_code ec;
    fs::remove(p, ec);
}

TEST_CASE("load_destinations_from_file: incomplete row skipped")
{
    const fs::path p = unique_temp_json("incomplete");
    write_file(p, R"({"destinations":[{"platform":"Twitch","server":"","stream_key":""}]})");
    load_destinations_from_file(p.string().c_str());
    REQUIRE(g_destinations.empty());
    clear_plugin_destinations();
    std::error_code ec;
    fs::remove(p, ec);
}

TEST_CASE("save and load round-trip restores destination fields")
{
    const fs::path p = unique_temp_json("roundtrip");
    clear_plugin_destinations();

    Destination dst;
    dst.platform = "Twitch";
    dst.server = "rtmps://live.twitch.tv/app";
    dst.stream_key = "test-key-roundtrip";
    dst.enabled = true;
    dst.requires_vertical = false;
    normalize_destination(dst);

    g_destinations.push_back(std::move(dst));
    {
        std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
        g_runtime_statuses[destination_id(g_destinations.back())] = RuntimeStatus{};
    }

    save_destinations_to_file(p.string().c_str());

    clear_plugin_destinations();
    load_destinations_from_file(p.string().c_str());

    REQUIRE(g_destinations.size() == 1);
    REQUIRE(g_destinations[0].platform == "Twitch");
    REQUIRE(g_destinations[0].server == "rtmps://live.twitch.tv/app");
    REQUIRE(g_destinations[0].stream_key == "test-key-roundtrip");
    REQUIRE(g_destinations[0].enabled);
    REQUIRE_FALSE(g_destinations[0].is_default);

    clear_plugin_destinations();
    std::error_code ec;
    fs::remove(p, ec);
}

TEST_CASE("default rows in JSON are ignored on load")
{
    const fs::path p = unique_temp_json("default_row");
    write_file(p, R"({"destinations":[{"is_default":true,"platform":"OBS default","server":"x","stream_key":"y"}]})");
    load_destinations_from_file(p.string().c_str());
    REQUIRE(g_destinations.empty());
    clear_plugin_destinations();
    std::error_code ec;
    fs::remove(p, ec);
}
