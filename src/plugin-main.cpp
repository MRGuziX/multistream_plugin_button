#include <obs-module.h>
#include <obs-frontend-api.h>

#include <functional>
#include <memory>
#include <algorithm>
#include <cctype>
#include <QDockWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QMetaObject>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <string>
#include <unordered_map>
#include <vector>

namespace {
struct Destination {
    std::string platform;
    std::string server;
    std::string stream_key;
    std::string protocol = "rtmp";
    bool requires_vertical = false;
    bool enabled = true;
    std::string notes;
};

enum class PlatformKind {
    YouTube,
    Twitch,
    Kick,
    Other,
};

std::vector<Destination> g_destinations;
QDockWidget *g_dock = nullptr;
QTableWidget *g_destinations_table = nullptr;

enum class RuntimeState {
    Idle,
    Starting,
    Live,
    Failed,
    Stopped,
};

struct RuntimeStatus {
    RuntimeState state = RuntimeState::Idle;
    std::string last_error;
};

std::unordered_map<std::string, RuntimeStatus> g_runtime_statuses;

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

void refresh_destinations_table();

class MultistreamManager;
std::unique_ptr<MultistreamManager> g_multistream_manager;

void request_table_refresh()
{
    if (!g_destinations_table) {
        return;
    }

    QMetaObject::invokeMethod(g_destinations_table, []() {
        refresh_destinations_table();
    }, Qt::QueuedConnection);
}

void set_runtime_status(const Destination &dst, RuntimeState state, const std::string &last_error = "")
{
    RuntimeStatus &status = g_runtime_statuses[destination_id(dst)];
    status.state = state;
    if (!last_error.empty()) {
        status.last_error = last_error;
    } else if (state == RuntimeState::Live || state == RuntimeState::Stopped || state == RuntimeState::Idle) {
        status.last_error.clear();
    }

    request_table_refresh();
}

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
        return "rtmps://a.rtmp.youtube.com/live2";
    case PlatformKind::Twitch:
        return "rtmps://live.twitch.tv/app";
    case PlatformKind::Kick:
        return "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app";
    default:
        return "";
    }
}

bool has_duplicate_destination(const Destination &candidate)
{
    return std::any_of(g_destinations.begin(), g_destinations.end(), [&candidate](const Destination &existing) {
        return existing.server == candidate.server && existing.stream_key == candidate.stream_key;
    });
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

bool validate_destination(const Destination &dst)
{
    if (dst.platform.empty() || dst.server.empty() || dst.stream_key.empty()) {
        blog(LOG_WARNING, "[obs-multistream-plugin] Platform/server/key cannot be empty");
        return false;
    }

    if (!looks_like_rtmp_url(dst.server)) {
        blog(LOG_WARNING, "[obs-multistream-plugin] Server must start with rtmp:// or rtmps://");
        return false;
    }

    const PlatformKind platform_kind = detect_platform_kind(dst.platform);
    if ((platform_kind == PlatformKind::YouTube || platform_kind == PlatformKind::Twitch ||
         platform_kind == PlatformKind::Kick) &&
        !starts_with_case_insensitive(dst.server, "rtmps://")) {
        blog(LOG_WARNING, "[obs-multistream-plugin] %s destination must use rtmps://", platform_kind_name(platform_kind));
        return false;
    }

    if (platform_kind == PlatformKind::Kick && dst.server.find(":443") == std::string::npos) {
        blog(LOG_WARNING, "[obs-multistream-plugin] kick destination must include port 443");
        return false;
    }

    if (has_duplicate_destination(dst)) {
        blog(LOG_WARNING, "[obs-multistream-plugin] Duplicate destination rejected for platform=%s", dst.platform.c_str());
        return false;
    }

    return true;
}

void refresh_destinations_table()
{
    if (!g_destinations_table) {
        return;
    }

    g_destinations_table->setRowCount(static_cast<int>(g_destinations.size()));
    for (int row = 0; row < static_cast<int>(g_destinations.size()); ++row) {
        const Destination &dst = g_destinations[static_cast<size_t>(row)];
        const auto it = g_runtime_statuses.find(destination_id(dst));
        const RuntimeStatus runtime = it == g_runtime_statuses.end() ? RuntimeStatus{} : it->second;

        auto *enabled_item = new QTableWidgetItem(dst.enabled ? "Yes" : "No");
        auto *platform_item = new QTableWidgetItem(QString::fromStdString(dst.platform));
        auto *server_item = new QTableWidgetItem(QString::fromStdString(dst.server));
        auto *protocol_item = new QTableWidgetItem(QString::fromStdString(dst.protocol));
        auto *vertical_item = new QTableWidgetItem(dst.requires_vertical ? "Yes" : "No");
        auto *status_item = new QTableWidgetItem(dst.enabled ? runtime_state_name(runtime.state) : "Disabled");
        auto *error_item = new QTableWidgetItem(QString::fromStdString(runtime.last_error));

        enabled_item->setFlags(enabled_item->flags() & ~Qt::ItemIsEditable);
        platform_item->setFlags(platform_item->flags() & ~Qt::ItemIsEditable);
        server_item->setFlags(server_item->flags() & ~Qt::ItemIsEditable);
        protocol_item->setFlags(protocol_item->flags() & ~Qt::ItemIsEditable);
        vertical_item->setFlags(vertical_item->flags() & ~Qt::ItemIsEditable);
        status_item->setFlags(status_item->flags() & ~Qt::ItemIsEditable);
        error_item->setFlags(error_item->flags() & ~Qt::ItemIsEditable);

        g_destinations_table->setItem(row, 0, enabled_item);
        g_destinations_table->setItem(row, 1, platform_item);
        g_destinations_table->setItem(row, 2, server_item);
        g_destinations_table->setItem(row, 3, protocol_item);
        g_destinations_table->setItem(row, 4, vertical_item);
        g_destinations_table->setItem(row, 5, status_item);
        g_destinations_table->setItem(row, 6, error_item);
    }

    g_destinations_table->resizeColumnsToContents();
}

class MultistreamManager {
public:
    void start_for_all_enabled(const std::vector<Destination> &destinations)
    {
        stopping_ = false;
        stop_all();
        stopping_ = false;

        obs_output_t *main_output = obs_frontend_get_streaming_output();
        if (!main_output) {
            blog(LOG_WARNING, "[obs-multistream-plugin] Main streaming output is not available");
            return;
        }

        obs_encoder_t *video_encoder = obs_output_get_video_encoder(main_output);
        obs_encoder_t *audio_encoder = obs_output_get_audio_encoder(main_output, 0);
        if (!video_encoder || !audio_encoder) {
            blog(LOG_WARNING, "[obs-multistream-plugin] Main stream encoders are not available");
            obs_output_release(main_output);
            return;
        }

        bool started_any = false;
        for (const Destination &dst : destinations) {
            if (!dst.enabled) {
                continue;
            }

            set_runtime_status(dst, RuntimeState::Starting);
            const bool started = start_single_destination(dst, video_encoder, audio_encoder);
            if (!started) {
                schedule_retry(dst, "Failed to start output");
            }
            started_any = started_any || started;
        }

        if (!started_any) {
            blog(LOG_WARNING, "[obs-multistream-plugin] No secondary destination started");
        }

        obs_output_release(main_output);
    }

    void stop_all()
    {
        stopping_ = true;
        ++retry_epoch_;
        retry_infos_.clear();

        for (const std::unique_ptr<DestinationRuntime> &runtime : runtimes_) {
            if (runtime->output) {
                disconnect_output_callbacks(runtime.get());
                if (obs_output_active(runtime->output)) {
                    obs_output_stop(runtime->output);
                }
                obs_output_release(runtime->output);
                runtime->output = nullptr;
            }

            if (runtime->service) {
                obs_service_release(runtime->service);
                runtime->service = nullptr;
            }

            set_runtime_status(runtime->destination, RuntimeState::Stopped);
        }

        runtimes_.clear();
    }

    void handle_runtime_deactivated(const Destination &dst)
    {
        if (stopping_) {
            return;
        }

        cleanup_runtime_for_destination(destination_id(dst));
        schedule_retry(dst, "Output deactivated");
    }

    void retry_destination_by_id(const std::string &id, uint64_t retry_epoch)
    {
        if (stopping_ || retry_epoch != retry_epoch_) {
            return;
        }

        const Destination *candidate = find_enabled_destination_by_id(id);
        if (!candidate) {
            return;
        }

        obs_output_t *main_output = obs_frontend_get_streaming_output();
        if (!main_output) {
            schedule_retry(*candidate, "Main output unavailable during retry");
            return;
        }

        obs_encoder_t *video_encoder = obs_output_get_video_encoder(main_output);
        obs_encoder_t *audio_encoder = obs_output_get_audio_encoder(main_output, 0);
        if (!video_encoder || !audio_encoder) {
            obs_output_release(main_output);
            schedule_retry(*candidate, "Main encoders unavailable during retry");
            return;
        }

        cleanup_runtime_for_destination(id);
        set_runtime_status(*candidate, RuntimeState::Starting);
        const bool started = start_single_destination(*candidate, video_encoder, audio_encoder);
        obs_output_release(main_output);

        if (!started) {
            schedule_retry(*candidate, "Retry start failed");
        }
    }

private:
    struct DestinationRuntime {
        Destination destination;
        obs_service_t *service = nullptr;
        obs_output_t *output = nullptr;
    };

    struct RetryInfo {
        int attempts = 0;
    };

    std::vector<std::unique_ptr<DestinationRuntime>> runtimes_;
    std::unordered_map<std::string, RetryInfo> retry_infos_;
    bool stopping_ = false;
    uint64_t retry_epoch_ = 0;

    static constexpr int kMaxRetryAttempts = 3;
    static constexpr int kBaseRetryDelayMs = 2000;

    static void on_output_started(void *param, calldata_t *)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        set_runtime_status(runtime->destination, RuntimeState::Live);
    }

    static void on_output_stopped(void *param, calldata_t *)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        set_runtime_status(runtime->destination, RuntimeState::Stopped);
    }

    static void on_output_reconnect(void *param, calldata_t *)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        set_runtime_status(runtime->destination, RuntimeState::Starting, "Reconnecting");
    }

    static void on_output_reconnect_success(void *param, calldata_t *)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        set_runtime_status(runtime->destination, RuntimeState::Live);
    }

    static void on_output_deactivate(void *param, calldata_t *)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        set_runtime_status(runtime->destination, RuntimeState::Failed, "Output deactivated");
        if (g_multistream_manager) {
            g_multistream_manager->handle_runtime_deactivated(runtime->destination);
        }
    }

    const Destination *find_enabled_destination_by_id(const std::string &id) const
    {
        for (const Destination &dst : g_destinations) {
            if (!dst.enabled) {
                continue;
            }
            if (destination_id(dst) == id) {
                return &dst;
            }
        }
        return nullptr;
    }

    void cleanup_runtime_for_destination(const std::string &id)
    {
        for (auto it = runtimes_.begin(); it != runtimes_.end(); ++it) {
            DestinationRuntime *runtime = it->get();
            if (destination_id(runtime->destination) != id) {
                continue;
            }

            if (runtime->output) {
                disconnect_output_callbacks(runtime);
                if (obs_output_active(runtime->output)) {
                    obs_output_stop(runtime->output);
                }
                obs_output_release(runtime->output);
                runtime->output = nullptr;
            }

            if (runtime->service) {
                obs_service_release(runtime->service);
                runtime->service = nullptr;
            }

            runtimes_.erase(it);
            return;
        }
    }

    void schedule_retry(const Destination &dst, const std::string &reason)
    {
        if (stopping_ || !g_dock) {
            return;
        }

        const std::string id = destination_id(dst);
        const Destination *candidate = find_enabled_destination_by_id(id);
        if (!candidate) {
            return;
        }

        RetryInfo &retry = retry_infos_[id];
        if (retry.attempts >= kMaxRetryAttempts) {
            set_runtime_status(*candidate, RuntimeState::Failed, "Retry limit reached");
            return;
        }

        ++retry.attempts;
        const int delay_ms = kBaseRetryDelayMs * retry.attempts;
        set_runtime_status(
            *candidate,
            RuntimeState::Starting,
            "Retry " + std::to_string(retry.attempts) + "/" + std::to_string(kMaxRetryAttempts) +
                " in " + std::to_string(delay_ms / 1000) + "s: " + reason);

        const uint64_t scheduled_epoch = retry_epoch_;
        QTimer::singleShot(delay_ms, g_dock, [id, scheduled_epoch]() {
            if (!g_multistream_manager) {
                return;
            }
            g_multistream_manager->retry_destination_by_id(id, scheduled_epoch);
        });
    }

    static void connect_output_callbacks(DestinationRuntime *runtime)
    {
        if (!runtime || !runtime->output) {
            return;
        }

        signal_handler_t *output_signal_handler = obs_output_get_signal_handler(runtime->output);
        if (!output_signal_handler) {
            return;
        }

        signal_handler_connect(output_signal_handler, "start", on_output_started, runtime);
        signal_handler_connect(output_signal_handler, "stop", on_output_stopped, runtime);
        signal_handler_connect(output_signal_handler, "reconnect", on_output_reconnect, runtime);
        signal_handler_connect(output_signal_handler, "reconnect_success", on_output_reconnect_success, runtime);
        signal_handler_connect(output_signal_handler, "deactivate", on_output_deactivate, runtime);
    }

    static void disconnect_output_callbacks(DestinationRuntime *runtime)
    {
        if (!runtime || !runtime->output) {
            return;
        }

        signal_handler_t *output_signal_handler = obs_output_get_signal_handler(runtime->output);
        if (!output_signal_handler) {
            return;
        }

        signal_handler_disconnect(output_signal_handler, "start", on_output_started, runtime);
        signal_handler_disconnect(output_signal_handler, "stop", on_output_stopped, runtime);
        signal_handler_disconnect(output_signal_handler, "reconnect", on_output_reconnect, runtime);
        signal_handler_disconnect(output_signal_handler, "reconnect_success", on_output_reconnect_success, runtime);
        signal_handler_disconnect(output_signal_handler, "deactivate", on_output_deactivate, runtime);
    }

    static std::string make_safe_name(const std::string &prefix, const Destination &dst)
    {
        const std::string platform = dst.platform.empty() ? "unknown" : dst.platform;
        return prefix + "_" + platform;
    }

    bool start_single_destination(const Destination &dst, obs_encoder_t *video_encoder, obs_encoder_t *audio_encoder)
    {
        const PlatformKind platform_kind = detect_platform_kind(dst.platform);
        obs_data_t *service_settings = obs_data_create();
        obs_data_set_string(service_settings, "server", dst.server.c_str());
        obs_data_set_string(service_settings, "key", dst.stream_key.c_str());

        const std::string service_name = make_safe_name("multistream_service", dst);
        obs_service_t *service = obs_service_create("rtmp_custom", service_name.c_str(), service_settings, nullptr);
        obs_data_release(service_settings);

        if (!service) {
            blog(LOG_ERROR, "[obs-multistream-plugin] Failed to create service for platform=%s", dst.platform.c_str());
            return false;
        }

        const std::string output_name = make_safe_name("multistream_output", dst);
        obs_output_t *output = obs_output_create("rtmp_output", output_name.c_str(), nullptr, nullptr);
        if (!output) {
            blog(LOG_ERROR, "[obs-multistream-plugin] Failed to create output for platform=%s", dst.platform.c_str());
            obs_service_release(service);
            return false;
        }

        obs_output_set_service(output, service);
        obs_output_set_video_encoder(output, video_encoder);
        obs_output_set_audio_encoder(output, audio_encoder, 0);

        if (!obs_output_start(output)) {
            blog(LOG_ERROR, "[obs-multistream-plugin] Failed to start output for platform=%s", dst.platform.c_str());
            set_runtime_status(dst, RuntimeState::Failed, "Failed to start output");
            obs_output_release(output);
            obs_service_release(service);
            return false;
        }

        auto runtime = std::make_unique<DestinationRuntime>();
        runtime->destination = dst;
        runtime->service = service;
        runtime->output = output;
        connect_output_callbacks(runtime.get());
        runtimes_.push_back(std::move(runtime));
        retry_infos_[destination_id(dst)].attempts = 0;

        blog(LOG_INFO,
            "[obs-multistream-plugin] Started secondary stream for platform=%s (kind=%s, protocol=%s)",
            dst.platform.c_str(),
            platform_kind_name(platform_kind),
            dst.protocol.c_str());
        return true;
    }
};

void on_frontend_event(enum obs_frontend_event event, void *)
{
    if (!g_multistream_manager) {
        return;
    }

    switch (event) {
    case OBS_FRONTEND_EVENT_STREAMING_STARTED:
        g_multistream_manager->start_for_all_enabled(g_destinations);
        break;
    case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
    case OBS_FRONTEND_EVENT_EXIT:
        g_multistream_manager->stop_all();
        break;
    default:
        break;
    }
}

void load_destinations()
{
    g_destinations.clear();
    g_runtime_statuses.clear();

    obs_data_t *config = obs_data_create_from_json_file_safe("obs-multistream-plugin.json", "bak");
    if (!config) {
        blog(LOG_INFO, "[obs-multistream-plugin] No existing destination config found");
        return;
    }

    obs_data_array_t *items = obs_data_get_array(config, "destinations");
    const size_t count = obs_data_array_count(items);
    for (size_t i = 0; i < count; ++i) {
        obs_data_t *item = obs_data_array_item(items, i);
        Destination dst;
        dst.platform = obs_data_get_string(item, "platform");
        dst.server = obs_data_get_string(item, "server");
        dst.stream_key = obs_data_get_string(item, "stream_key");
        const char *protocol = obs_data_get_string(item, "protocol");
        if (protocol && protocol[0] != '\0') {
            dst.protocol = protocol;
        }
        dst.requires_vertical = obs_data_get_bool(item, "requires_vertical");
        dst.notes = obs_data_get_string(item, "notes");
        dst.enabled = obs_data_has_user_value(item, "enabled") ? obs_data_get_bool(item, "enabled") : true;

        normalize_destination(dst);

        if (dst.platform.empty() || dst.server.empty() || dst.stream_key.empty()) {
            blog(LOG_WARNING, "[obs-multistream-plugin] Skipping incomplete destination at index=%zu", i);
            obs_data_release(item);
            continue;
        }

        if (has_duplicate_destination(dst)) {
            blog(LOG_WARNING, "[obs-multistream-plugin] Skipping duplicate destination from config for platform=%s", dst.platform.c_str());
            obs_data_release(item);
            continue;
        }

        g_runtime_statuses[destination_id(dst)] = RuntimeStatus{};
        g_destinations.push_back(std::move(dst));
        obs_data_release(item);
    }

    obs_data_array_release(items);
    obs_data_release(config);
    blog(LOG_INFO, "[obs-multistream-plugin] Loaded %zu destination(s)", g_destinations.size());
}

void save_destinations()
{
    obs_data_t *config = obs_data_create();
    obs_data_array_t *items = obs_data_array_create();

    for (const Destination &dst : g_destinations) {
        obs_data_t *item = obs_data_create();
        obs_data_set_string(item, "platform", dst.platform.c_str());
        obs_data_set_string(item, "server", dst.server.c_str());
        obs_data_set_string(item, "stream_key", dst.stream_key.c_str());
        obs_data_set_string(item, "protocol", dst.protocol.c_str());
        obs_data_set_bool(item, "requires_vertical", dst.requires_vertical);
        obs_data_set_bool(item, "enabled", dst.enabled);
        obs_data_set_string(item, "notes", dst.notes.c_str());
        obs_data_array_push_back(items, item);
        obs_data_release(item);
    }

    obs_data_set_array(config, "destinations", items);
    obs_data_save_json_safe(config, "obs-multistream-plugin.json", "tmp", "bak");

    obs_data_array_release(items);
    obs_data_release(config);
    blog(LOG_INFO, "[obs-multistream-plugin] Saved %zu destination(s)", g_destinations.size());
}

void create_dock()
{
    auto *main_window = reinterpret_cast<QMainWindow *>(obs_frontend_get_main_window());
    if (!main_window) {
        blog(LOG_WARNING, "[obs-multistream-plugin] Could not get OBS main window");
        return;
    }

    auto *container = new QWidget(main_window);
    auto *layout = new QVBoxLayout(container);
    auto *hint = new QLabel("Milestone 2 scaffold: destination UI and persistence are initialized.", container);
    hint->setWordWrap(true);

    auto *form_widget = new QWidget(container);
    auto *form = new QFormLayout(form_widget);
    auto *platform = new QLineEdit(form_widget);
    auto *server = new QLineEdit(form_widget);
    auto *stream_key = new QLineEdit(form_widget);
    stream_key->setEchoMode(QLineEdit::Password);

    form->addRow("Platform", platform);
    form->addRow("Server", server);
    form->addRow("Stream key", stream_key);

    auto *add_button = new QPushButton("Add destination", container);
    auto *start_button = new QPushButton("Start multistream", container);
    auto *stop_button = new QPushButton("Stop multistream", container);
    auto *toggle_button = new QPushButton("Toggle enabled", container);
    auto *remove_button = new QPushButton("Remove selected", container);

    g_destinations_table = new QTableWidget(container);
    g_destinations_table->setColumnCount(7);
    g_destinations_table->setHorizontalHeaderLabels({"Enabled", "Platform", "Server", "Protocol", "Vertical", "Status", "Last error"});
    g_destinations_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    g_destinations_table->setSelectionMode(QAbstractItemView::SingleSelection);
    g_destinations_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    g_destinations_table->verticalHeader()->setVisible(false);
    g_destinations_table->horizontalHeader()->setStretchLastSection(true);

    QObject::connect(add_button, &QPushButton::clicked, [platform, server, stream_key]() {
        Destination dst;
        dst.platform = platform->text().toStdString();
        dst.server = server->text().toStdString();
        dst.stream_key = stream_key->text().toStdString();

        normalize_destination(dst);
        if (!validate_destination(dst)) {
            return;
        }

        g_destinations.push_back(std::move(dst));
        g_runtime_statuses[destination_id(g_destinations.back())] = RuntimeStatus{};
        save_destinations();
        refresh_destinations_table();

        platform->clear();
        server->clear();
        stream_key->clear();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination added");
    });

    QObject::connect(start_button, &QPushButton::clicked, []() {
        if (!g_multistream_manager) {
            return;
        }

        g_multistream_manager->start_for_all_enabled(g_destinations);
    });

    QObject::connect(stop_button, &QPushButton::clicked, []() {
        if (!g_multistream_manager) {
            return;
        }

        g_multistream_manager->stop_all();
    });

    QObject::connect(toggle_button, &QPushButton::clicked, []() {
        if (!g_destinations_table) {
            return;
        }

        const int row = g_destinations_table->currentRow();
        if (row < 0 || row >= static_cast<int>(g_destinations.size())) {
            blog(LOG_WARNING, "[obs-multistream-plugin] No destination selected to toggle");
            return;
        }

        Destination &dst = g_destinations[static_cast<size_t>(row)];
        dst.enabled = !dst.enabled;
        save_destinations();
        refresh_destinations_table();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination toggled for platform=%s", dst.platform.c_str());
    });

    QObject::connect(remove_button, &QPushButton::clicked, []() {
        if (!g_destinations_table) {
            return;
        }

        const int row = g_destinations_table->currentRow();
        if (row < 0 || row >= static_cast<int>(g_destinations.size())) {
            blog(LOG_WARNING, "[obs-multistream-plugin] No destination selected to remove");
            return;
        }

        const std::string platform = g_destinations[static_cast<size_t>(row)].platform;
        g_runtime_statuses.erase(destination_id(g_destinations[static_cast<size_t>(row)]));
        g_destinations.erase(g_destinations.begin() + row);
        save_destinations();
        refresh_destinations_table();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination removed for platform=%s", platform.c_str());
    });

    auto *actions_layout = new QHBoxLayout();
    actions_layout->addWidget(start_button);
    actions_layout->addWidget(stop_button);

    auto *manage_layout = new QHBoxLayout();
    manage_layout->addWidget(toggle_button);
    manage_layout->addWidget(remove_button);

    layout->addWidget(hint);
    layout->addWidget(form_widget);
    layout->addWidget(add_button);
    layout->addWidget(g_destinations_table);
    layout->addLayout(actions_layout);
    layout->addLayout(manage_layout);
    layout->addStretch();

    refresh_destinations_table();

    g_dock = new QDockWidget("Multistream Destinations", main_window);
    g_dock->setObjectName("obs_multistream_destinations_dock");
    g_dock->setWidget(container);

    main_window->addDockWidget(Qt::RightDockWidgetArea, g_dock);
}

void destroy_dock()
{
    if (!g_dock) {
        return;
    }

    g_dock->hide();
    g_dock->deleteLater();
    g_destinations_table = nullptr;
    g_dock = nullptr;
}
} // namespace

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multistream-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS plugin for multistreaming to multiple RTMP platforms with shared encoders.";
}

bool obs_module_load(void)
{
    load_destinations();
    create_dock();
    g_multistream_manager = std::make_unique<MultistreamManager>();
    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    blog(LOG_INFO, "[obs-multistream-plugin] Module loaded");
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    if (g_multistream_manager) {
        g_multistream_manager->stop_all();
    }
    g_multistream_manager.reset();
    save_destinations();
    destroy_dock();
    blog(LOG_INFO, "[obs-multistream-plugin] Module unloaded");
}
