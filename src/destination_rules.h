#pragma once

#include <string>
#include <vector>

struct Destination {
    std::string platform;
    std::string server;
    std::string stream_key;
    std::string protocol = "rtmp";
    bool requires_vertical = false;
    bool enabled = true;
    bool is_default = false;     // Mirror of OBS's primary streaming service (Settings -> Stream).
                                 // Locked entry: not editable / removable, not persisted, not started by us.
    std::string notes;
};

enum class PlatformKind {
    YouTube,
    Twitch,
    Kick,
    Other,
};

enum class DestinationValidationError {
    None,
    EmptyRequiredField,
    InvalidServerUrl,
    PlatformRequiresRtmps,
    KickRequiresPort443,
    Duplicate,
};

struct DestinationValidationResult {
    bool ok = true;
    DestinationValidationError error = DestinationValidationError::None;
};

std::string to_lower_copy(const std::string &value);
bool starts_with_case_insensitive(const std::string &value, const std::string &prefix);
bool looks_like_rtmp_url(const std::string &url);

PlatformKind detect_platform_kind(const std::string &platform);
const char *platform_kind_name(PlatformKind kind);
std::string default_server_for_platform(PlatformKind kind);

bool has_duplicate_destination(const std::vector<Destination> &destinations, const Destination &candidate,
                              int skip_index = -1);
void normalize_destination(Destination &dst);
DestinationValidationResult validate_destination(const std::vector<Destination> &destinations, const Destination &dst,
                                                 int skip_duplicate_index = -1);