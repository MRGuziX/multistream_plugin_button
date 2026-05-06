#include "destination_rules.h"

#include <algorithm>
#include <cctype>

std::string to_lower_copy(const std::string &value)
{
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lower;
}

bool starts_with_case_insensitive(const std::string &value, const std::string &prefix)
{
    if (value.size() < prefix.size()) {
        return false;
    }

    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }

    return true;
}

bool looks_like_rtmp_url(const std::string &url)
{
    return starts_with_case_insensitive(url, "rtmp://") || starts_with_case_insensitive(url, "rtmps://");
}

PlatformKind detect_platform_kind(const std::string &platform)
{
    const std::string platform_lower = to_lower_copy(platform);
    if (platform_lower.find("youtube") != std::string::npos || platform_lower.find("yt") != std::string::npos) {
        return PlatformKind::YouTube;
    }
    if (platform_lower.find("twitch") != std::string::npos) {
        return PlatformKind::Twitch;
    }
    if (platform_lower.find("kick") != std::string::npos) {
        return PlatformKind::Kick;
    }
    return PlatformKind::Other;
}

const char *platform_kind_name(PlatformKind kind)
{
    switch (kind) {
    case PlatformKind::YouTube:
        return "youtube";
    case PlatformKind::Twitch:
        return "twitch";
    case PlatformKind::Kick:
        return "kick";
    default:
        return "custom";
    }
}

std::string default_server_for_platform(PlatformKind kind)
{
    switch (kind) {
    case PlatformKind::YouTube:
        return "rtmps://a.rtmps.youtube.com:443/live2";
    case PlatformKind::Twitch:
        return "rtmps://live.twitch.tv/app";
    case PlatformKind::Kick:
        return "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app";
    default:
        return "";
    }
}

bool has_duplicate_destination(const std::vector<Destination> &destinations, const Destination &candidate, int skip_index)
{
    for (int index = 0; index < static_cast<int>(destinations.size()); ++index) {
        if (index == skip_index) {
            continue;
        }

        const Destination &existing = destinations[static_cast<size_t>(index)];
        if (existing.server == candidate.server && existing.stream_key == candidate.stream_key) {
            return true;
        }
    }

    return false;
}

void normalize_destination(Destination &dst)
{
    const PlatformKind platform_kind = detect_platform_kind(dst.platform);

    if (dst.server.empty()) {
        dst.server = default_server_for_platform(platform_kind);
    }

    if ((platform_kind == PlatformKind::YouTube || platform_kind == PlatformKind::Twitch) &&
        starts_with_case_insensitive(dst.server, "rtmp://")) {
        dst.server.replace(0, 7, "rtmps://");
    }

    if (starts_with_case_insensitive(dst.server, "rtmps://")) {
        dst.protocol = "rtmps";
    } else if (starts_with_case_insensitive(dst.server, "rtmp://")) {
        dst.protocol = "rtmp";
    }

    const std::string platform_lower = to_lower_copy(dst.platform);
    if (platform_lower.find("instagram") != std::string::npos || platform_lower.find("tiktok") != std::string::npos) {
        dst.requires_vertical = true;
    }

    if (platform_kind != PlatformKind::Kick) {
        return;
    }

    if (starts_with_case_insensitive(dst.server, "rtmp://")) {
        dst.server.replace(0, 7, "rtmps://");
    }

    if (!starts_with_case_insensitive(dst.server, "rtmps://")) {
        return;
    }

    const size_t host_start = dst.server.find("://");
    if (host_start == std::string::npos) {
        return;
    }

    const size_t authority_start = host_start + 3;
    const size_t path_pos = dst.server.find('/', authority_start);
    const std::string authority = path_pos == std::string::npos
        ? dst.server.substr(authority_start)
        : dst.server.substr(authority_start, path_pos - authority_start);

    if (authority.find(':') == std::string::npos) {
        if (path_pos == std::string::npos) {
            dst.server += ":443";
        } else {
            dst.server.insert(path_pos, ":443");
        }
    }

    dst.protocol = "rtmps";
}

std::set<std::string> allowed_codecs_for_platform(PlatformKind kind)
{
    switch (kind) {
    case PlatformKind::YouTube:
        return {"h264", "hevc", "av1"};
    case PlatformKind::Twitch:
        return {"h264"};
    case PlatformKind::Kick:
        return {"h264"};
    case PlatformKind::Other:
    default:
        return {};
    }
}

DestinationValidationResult validate_destination(const std::vector<Destination> &destinations, const Destination &dst,
                                                 int skip_duplicate_index)
{
    if (dst.platform.empty() || dst.server.empty() || dst.stream_key.empty()) {
        return {false, DestinationValidationError::EmptyRequiredField};
    }

    if (!looks_like_rtmp_url(dst.server)) {
        return {false, DestinationValidationError::InvalidServerUrl};
    }

    const PlatformKind platform_kind = detect_platform_kind(dst.platform);
    if ((platform_kind == PlatformKind::YouTube || platform_kind == PlatformKind::Twitch ||
         platform_kind == PlatformKind::Kick) &&
        !starts_with_case_insensitive(dst.server, "rtmps://")) {
        return {false, DestinationValidationError::PlatformRequiresRtmps};
    }

    if (platform_kind == PlatformKind::Kick && dst.server.find(":443") == std::string::npos) {
        return {false, DestinationValidationError::KickRequiresPort443};
    }

    if (has_duplicate_destination(destinations, dst, skip_duplicate_index)) {
        return {false, DestinationValidationError::Duplicate};
    }

    return {true, DestinationValidationError::None};
}

