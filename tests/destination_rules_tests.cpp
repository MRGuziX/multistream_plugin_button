#include "destination_rules.h"

#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

namespace {

Destination make_destination(const std::string &platform, const std::string &server, const std::string &key)
{
    Destination dst;
    dst.platform = platform;
    dst.server = server;
    dst.stream_key = key;
    return dst;
}

} // namespace

TEST_CASE("default server autofill after normalization")
{
    Destination youtube = make_destination("YouTube", "", "k1");
    normalize_destination(youtube);
    REQUIRE(youtube.server == "rtmps://a.rtmp.youtube.com/live2");

    Destination twitch = make_destination("Twitch", "", "k2");
    normalize_destination(twitch);
    REQUIRE(twitch.server == "rtmps://live.twitch.tv/app");

    Destination kick = make_destination("Kick", "", "k3");
    normalize_destination(kick);
    REQUIRE(kick.server == "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app");
}

TEST_CASE("YouTube and Twitch require rtmps")
{
    Destination youtube = make_destination("YouTube", "rtmp://a.rtmp.youtube.com/live2", "k1");
    normalize_destination(youtube);
    REQUIRE(starts_with_case_insensitive(youtube.server, "rtmps://"));
    REQUIRE(validate_destination({}, youtube).ok);

    Destination twitch = make_destination("Twitch", "rtmp://live.twitch.tv/app", "k2");
    normalize_destination(twitch);
    REQUIRE(starts_with_case_insensitive(twitch.server, "rtmps://"));
    REQUIRE(validate_destination({}, twitch).ok);

    Destination invalid = make_destination("YouTube", "rtmp://a.rtmp.youtube.com/live2", "k3");
    const DestinationValidationResult invalid_result = validate_destination({}, invalid);
    REQUIRE_FALSE(invalid_result.ok);
    REQUIRE(invalid_result.error == DestinationValidationError::PlatformRequiresRtmps);
}

TEST_CASE("Kick requires rtmps and port 443")
{
    Destination kick = make_destination("Kick", "rtmp://ingest.kick.com/app", "k1");
    normalize_destination(kick);
    REQUIRE(starts_with_case_insensitive(kick.server, "rtmps://"));
    REQUIRE(kick.server.find(":443") != std::string::npos);
    REQUIRE(validate_destination({}, kick).ok);

    Destination missing_port = make_destination("Kick", "rtmps://ingest.kick.com/app", "k2");
    const DestinationValidationResult missing_port_result = validate_destination({}, missing_port);
    REQUIRE_FALSE(missing_port_result.ok);
    REQUIRE(missing_port_result.error == DestinationValidationError::KickRequiresPort443);
}

TEST_CASE("duplicate detection")
{
    const std::vector<Destination> existing = {
        make_destination("YouTube", "rtmps://a.rtmp.youtube.com/live2", "same-key"),
        make_destination("Twitch", "rtmps://live.twitch.tv/app", "other-key"),
    };

    Destination duplicate = make_destination("Custom", "rtmps://a.rtmp.youtube.com/live2", "same-key");
    REQUIRE(has_duplicate_destination(existing, duplicate));

    const DestinationValidationResult duplicate_result = validate_destination(existing, duplicate);
    REQUIRE_FALSE(duplicate_result.ok);
    REQUIRE(duplicate_result.error == DestinationValidationError::Duplicate);

    REQUIRE_FALSE(has_duplicate_destination(existing, existing[0], 0));
}
