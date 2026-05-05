#include "plugin_state.h"

#include "multistream_manager.h"

#include <QMetaObject>
#include <QTableWidget>
#include <obs-module.h>

std::vector<Destination> g_destinations;
QDockWidget *g_dock = nullptr;
QTableWidget *g_destinations_table = nullptr;
bool g_is_refreshing_table = false;
std::unordered_map<std::string, RuntimeStatus> g_runtime_statuses;
std::mutex g_runtime_status_mutex;
std::unique_ptr<MultistreamManager> g_multistream_manager;

std::string destination_id(const Destination &dst)
{
    return dst.server + "|" + dst.stream_key;
}

const char *runtime_state_name(RuntimeState state)
{
    switch (state) {
    case RuntimeState::Idle:
        return "Idle";
    case RuntimeState::Starting:
        return "Starting";
    case RuntimeState::Live:
        return "Live";
    case RuntimeState::Failed:
        return "Failed";
    case RuntimeState::Stopped:
        return "Stopped";
    default:
        return "Unknown";
    }
}

void request_table_refresh()
{
    if (!g_destinations_table) {
        return;
    }

    QMetaObject::invokeMethod(g_destinations_table, []() {
        refresh_destinations_table();
    }, Qt::QueuedConnection);
}

void set_runtime_status(const Destination &dst, RuntimeState state, const std::string &last_error)
{
    std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
    RuntimeStatus &status = g_runtime_statuses[destination_id(dst)];
    status.state = state;
    if (!last_error.empty()) {
        status.last_error = last_error;
    } else if (state == RuntimeState::Live || state == RuntimeState::Stopped || state == RuntimeState::Idle) {
        status.last_error.clear();
    }

    if (state == RuntimeState::Live || state == RuntimeState::Stopped || state == RuntimeState::Idle) {
        status.retry_attempt = 0;
        status.retry_max = 0;
        status.terminal_reason.clear();
    }

    request_table_refresh();
}

void set_runtime_retry_status(const Destination &dst, int attempt, int max_attempts, const std::string &retry_message)
{
    std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
    RuntimeStatus &status = g_runtime_statuses[destination_id(dst)];
    status.state = RuntimeState::Starting;
    status.retry_attempt = attempt;
    status.retry_max = max_attempts;
    status.last_error = retry_message;
    status.terminal_reason.clear();
    request_table_refresh();
}

void set_runtime_terminal_failure(const Destination &dst, const std::string &terminal_reason)
{
    std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
    RuntimeStatus &status = g_runtime_statuses[destination_id(dst)];
    status.state = RuntimeState::Failed;
    status.terminal_reason = terminal_reason;
    if (!terminal_reason.empty()) {
        status.last_error = terminal_reason;
    }
    request_table_refresh();
}

std::string last_runtime_error_for_destination(const Destination &dst)
{
    std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
    const auto it = g_runtime_statuses.find(destination_id(dst));
    if (it == g_runtime_statuses.end()) {
        return "";
    }
    return it->second.last_error;
}

bool validate_and_log_destination(const Destination &dst, int skip_duplicate_index)
{
    const DestinationValidationResult result = validate_destination(
        g_destinations, dst, skip_duplicate_index);
    if (result.ok) {
        for (int i = 0; i < static_cast<int>(g_destinations.size()); ++i) {
            if (i == skip_duplicate_index) {
                continue;
            }
            const Destination &existing = g_destinations[static_cast<size_t>(i)];
            if (existing.is_default && existing.server == dst.server && existing.stream_key == dst.stream_key) {
                blog(LOG_WARNING,
                     "[obs-multistream-plugin] Destination matches OBS primary streaming destination and was rejected");
                return false;
            }
        }
        return true;
    }

    switch (result.error) {
    case DestinationValidationError::EmptyRequiredField:
        blog(LOG_WARNING, "[obs-multistream-plugin] Platform/server/key cannot be empty");
        return false;
    case DestinationValidationError::InvalidServerUrl:
        blog(LOG_WARNING, "[obs-multistream-plugin] Server must start with rtmp:// or rtmps://");
        return false;
    case DestinationValidationError::PlatformRequiresRtmps:
        blog(LOG_WARNING, "[obs-multistream-plugin] %s destination must use rtmps://",
             platform_kind_name(detect_platform_kind(dst.platform)));
        return false;
    case DestinationValidationError::KickRequiresPort443:
        blog(LOG_WARNING, "[obs-multistream-plugin] kick destination must include port 443");
        return false;
    case DestinationValidationError::Duplicate:
        blog(LOG_WARNING, "[obs-multistream-plugin] Duplicate destination rejected for platform=%s", dst.platform.c_str());
        return false;
    case DestinationValidationError::None:
    default:
        return false;
    }
}
