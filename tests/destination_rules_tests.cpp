#include "destination_rules.h"

#include <iostream>
#include <string>
#include <vector>

namespace {
int g_failures = 0;

void expect_true(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        ++g_failures;
    }
}

void expect_equal(const std::string &actual, const std::string &expected, const std::string &message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " | expected='" << expected << "' actual='" << actual << "'" << std::endl;
        ++g_failures;
    }
}

void expect_error(DestinationValidationError actual, DestinationValidationError expected, const std::string &message)
{
    if (actual != expected) {
        std::cerr << "FAIL: " << message << " | expected error=" << static_cast<int>(expected)
                  << " actual=" << static_cast<int>(actual) << std::endl;
        ++g_failures;
    }
}

Destination make_destination(const std::string &platform, const std::string &server, const std::string &key)
{
    Destination dst;
    dst.platform = platform;
    dst.server = server;
    dst.stream_key = key;
    return dst;
}

void test_default_server_autofill()
{
    Destination youtube = make_destination("YouTube", "", "k1");
    normalize_destination(youtube);
    expect_equal(youtube.server, "rtmps://a.rtmp.youtube.com/live2", "YouTube default server autofill");

    Destination twitch = make_destination("Twitch", "", "k2");
    normalize_destination(twitch);
    expect_equal(twitch.server, "rtmps://live.twitch.tv/app", "Twitch default server autofill");

    Destination kick = make_destination("Kick", "", "k3");
    normalize_destination(kick);
    expect_equal(kick.server, "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app",
                 "Kick default server autofill");
}

void test_youtube_twitch_require_rtmps()
{
    Destination youtube = make_destination("YouTube", "rtmp://a.rtmp.youtube.com/live2", "k1");
    normalize_destination(youtube);
    expect_true(starts_with_case_insensitive(youtube.server, "rtmps://"), "YouTube normalize upgrades rtmp to rtmps");
    expect_true(validate_destination({}, youtube).ok, "YouTube validates after normalization");

    Destination twitch = make_destination("Twitch", "rtmp://live.twitch.tv/app", "k2");
    normalize_destination(twitch);
    expect_true(starts_with_case_insensitive(twitch.server, "rtmps://"), "Twitch normalize upgrades rtmp to rtmps");
    expect_true(validate_destination({}, twitch).ok, "Twitch validates after normalization");

    Destination invalid = make_destination("YouTube", "rtmp://a.rtmp.youtube.com/live2", "k3");
    const DestinationValidationResult invalid_result = validate_destination({}, invalid);
    expect_true(!invalid_result.ok, "YouTube validation fails without normalization");
    expect_error(invalid_result.error, DestinationValidationError::PlatformRequiresRtmps,
                 "YouTube validation error is PlatformRequiresRtmps");
}

void test_kick_requires_rtmps_and_443()
{
    Destination kick = make_destination("Kick", "rtmp://ingest.kick.com/app", "k1");
    normalize_destination(kick);
    expect_true(starts_with_case_insensitive(kick.server, "rtmps://"), "Kick normalize upgrades rtmp to rtmps");
    expect_true(kick.server.find(":443") != std::string::npos, "Kick normalize enforces :443");
    expect_true(validate_destination({}, kick).ok, "Kick validates after normalization");

    Destination missing_port = make_destination("Kick", "rtmps://ingest.kick.com/app", "k2");
    const DestinationValidationResult missing_port_result = validate_destination({}, missing_port);
    expect_true(!missing_port_result.ok, "Kick validation fails without :443");
    expect_error(missing_port_result.error, DestinationValidationError::KickRequiresPort443,
                 "Kick validation error is KickRequiresPort443");
}

void test_duplicate_detection()
{
    const std::vector<Destination> existing = {
        make_destination("YouTube", "rtmps://a.rtmp.youtube.com/live2", "same-key"),
        make_destination("Twitch", "rtmps://live.twitch.tv/app", "other-key"),
    };

    Destination duplicate = make_destination("Custom", "rtmps://a.rtmp.youtube.com/live2", "same-key");
    expect_true(has_duplicate_destination(existing, duplicate), "Duplicate detected by server+key");

    const DestinationValidationResult duplicate_result = validate_destination(existing, duplicate);
    expect_true(!duplicate_result.ok, "Validation fails for duplicate destination");
    expect_error(duplicate_result.error, DestinationValidationError::Duplicate,
                 "Duplicate validation error is Duplicate");

    expect_true(!has_duplicate_destination(existing, existing[0], 0), "Skip index ignores self duplicate during edit");
}
} // namespace

int main()
{
    test_default_server_autofill();
    test_youtube_twitch_require_rtmps();
    test_kick_requires_rtmps_and_443();
    test_duplicate_detection();

    if (g_failures == 0) {
        std::cout << "All destination rules tests passed" << std::endl;
        return 0;
    }

    std::cerr << g_failures << " test(s) failed" << std::endl;
    return 1;
}