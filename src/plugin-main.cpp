#include <obs-module.h>
#include <obs-frontend-api.h>

#include "destination_rules.h"

#include <functional>
#include <memory>
#include <QDockWidget>
#include <QComboBox>
#include <QColor>
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

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr const char *kPluginUiVersion = "v1.0.0b";

std::vector<Destination> g_destinations;
QDockWidget *g_dock = nullptr;
QTableWidget *g_destinations_table = nullptr;
bool g_is_refreshing_table = false;

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
    int retry_attempt = 0;
    int retry_max = 0;
    std::string terminal_reason;
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

    if (state == RuntimeState::Live || state == RuntimeState::Stopped || state == RuntimeState::Idle) {
        status.retry_attempt = 0;
        status.retry_max = 0;
        status.terminal_reason.clear();
    }

    request_table_refresh();
}

void set_runtime_retry_status(const Destination &dst, int attempt, int max_attempts, const std::string &retry_message)
{
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
    const auto it = g_runtime_statuses.find(destination_id(dst));
    if (it == g_runtime_statuses.end()) {
        return "";
    }
    return it->second.last_error;
}

std::string status_text_for_table(const RuntimeStatus &status, bool enabled)
{
    if (!enabled) {
        return "Disabled";
    }

    std::string value = runtime_state_name(status.state);
    if (status.retry_max > 0 && (status.state == RuntimeState::Starting || status.state == RuntimeState::Failed)) {
        value += " (retry " + std::to_string(status.retry_attempt) + "/" + std::to_string(status.retry_max) + ")";
    }
    return value;
}

QColor status_color_for_table(const RuntimeStatus &status, bool enabled)
{
    if (!enabled) {
        return QColor(160, 160, 160);
    }

    switch (status.state) {
    case RuntimeState::Live:
        return QColor(0, 170, 0);
    case RuntimeState::Failed:
        return QColor(200, 40, 40);
    case RuntimeState::Starting:
        return QColor(210, 140, 0);
    case RuntimeState::Stopped:
    case RuntimeState::Idle:
    default:
        return QColor(200, 200, 200);
    }
}

std::string error_text_for_table(const RuntimeStatus &status)
{
    if (!status.terminal_reason.empty()) {
        if (status.last_error.empty() || status.last_error == status.terminal_reason) {
            return status.terminal_reason;
        }
        return status.last_error + " | Terminal: " + status.terminal_reason;
    }
    return status.last_error;
}

std::string callback_reason(calldata_t *params, const std::string &fallback)
{
    if (!params) {
        return fallback;
    }

    const char *last_error = calldata_string(params, "last_error");
    if (last_error && last_error[0] != '\0') {
        return std::string(last_error);
    }

    const char *message = calldata_string(params, "message");
    if (message && message[0] != '\0') {
        return std::string(message);
    }

    const char *reason = calldata_string(params, "reason");
    if (reason && reason[0] != '\0') {
        return std::string(reason);
    }

    const long long code = calldata_int(params, "code");
    if (code != 0) {
        return fallback + " (code " + std::to_string(code) + ")";
    }

    return fallback;
}

bool validate_destination(const Destination &dst, int skip_duplicate_index = -1)
{
    const DestinationValidationResult result =
        validate_destination(g_destinations, dst, skip_duplicate_index);
    if (result.ok) {
        obs_service_t *primary_service = obs_frontend_get_streaming_service();
        if (primary_service) {
            obs_data_t *primary_settings = obs_service_get_settings(primary_service);

            Destination primary_dst;
            if (primary_settings) {
                primary_dst.platform = "OBS Primary";
                primary_dst.server = obs_data_get_string(primary_settings, "server");

                const char *primary_key = obs_data_get_string(primary_settings, "key");
                if (!primary_key || primary_key[0] == '\0') {
                    primary_key = obs_data_get_string(primary_settings, "stream_key");
                }
                primary_dst.stream_key = primary_key ? primary_key : "";

                normalize_destination(primary_dst);
                obs_data_release(primary_settings);
            }

            obs_service_release(primary_service);

            if (!primary_dst.server.empty() && !primary_dst.stream_key.empty() &&
                primary_dst.server == dst.server && primary_dst.stream_key == dst.stream_key) {
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

void refresh_destinations_table()
{
    if (!g_destinations_table) {
        return;
    }

    g_is_refreshing_table = true;
    g_destinations_table->setRowCount(static_cast<int>(g_destinations.size()));
    for (int row = 0; row < static_cast<int>(g_destinations.size()); ++row) {
        const Destination &dst = g_destinations[static_cast<size_t>(row)];
        const auto it = g_runtime_statuses.find(destination_id(dst));
        const RuntimeStatus runtime = it == g_runtime_statuses.end() ? RuntimeStatus{} : it->second;

        auto *enabled_item = new QTableWidgetItem();
        auto *platform_item = new QTableWidgetItem(QString::fromStdString(dst.platform));
        auto *server_item = new QTableWidgetItem(QString::fromStdString(dst.server));
        auto *protocol_item = new QTableWidgetItem(QString::fromStdString(dst.protocol));
        auto *vertical_item = new QTableWidgetItem(dst.requires_vertical ? "Yes" : "No");
        auto *status_item = new QTableWidgetItem(QString::fromStdString(status_text_for_table(runtime, dst.enabled)));
        auto *error_item = new QTableWidgetItem(QString::fromStdString(error_text_for_table(runtime)));

        enabled_item->setFlags((enabled_item->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
        enabled_item->setCheckState(dst.enabled ? Qt::Checked : Qt::Unchecked);
        platform_item->setFlags(platform_item->flags() & ~Qt::ItemIsEditable);
        server_item->setFlags(server_item->flags() & ~Qt::ItemIsEditable);
        protocol_item->setFlags(protocol_item->flags() & ~Qt::ItemIsEditable);
        vertical_item->setFlags(vertical_item->flags() & ~Qt::ItemIsEditable);
        status_item->setFlags(status_item->flags() & ~Qt::ItemIsEditable);
        error_item->setFlags(error_item->flags() & ~Qt::ItemIsEditable);
        status_item->setForeground(status_color_for_table(runtime, dst.enabled));

        g_destinations_table->setItem(row, 0, enabled_item);
        g_destinations_table->setItem(row, 1, platform_item);
        g_destinations_table->setItem(row, 2, server_item);
        g_destinations_table->setItem(row, 3, protocol_item);
        g_destinations_table->setItem(row, 4, vertical_item);
        g_destinations_table->setItem(row, 5, status_item);
        g_destinations_table->setItem(row, 6, error_item);
    }

    g_destinations_table->resizeColumnsToContents();
    g_is_refreshing_table = false;
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
                const std::string reason = last_runtime_error_for_destination(dst).empty()
                    ? "Failed to start output"
                    : last_runtime_error_for_destination(dst);
                schedule_retry(dst, reason);
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
                if (obs_output_active(runtime->output)) {
                    obs_output_stop(runtime->output);
                } else {
                    release_runtime_resources(runtime.get());
                }
            }

            set_runtime_status(runtime->destination, RuntimeState::Stopped);
        }

        runtimes_.erase(
            std::remove_if(
                runtimes_.begin(),
                runtimes_.end(),
                [](const std::unique_ptr<DestinationRuntime> &runtime) {
                    return !runtime || !runtime->output;
                }),
            runtimes_.end());
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
            const std::string reason = last_runtime_error_for_destination(*candidate).empty()
                ? "Retry start failed"
                : last_runtime_error_for_destination(*candidate);
            schedule_retry(*candidate, reason);
        }
    }

private:
    struct DestinationRuntime {
        Destination destination;
        obs_service_t *service = nullptr;
        obs_output_t *output = nullptr;
        obs_encoder_t *video_encoder = nullptr;
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
    static constexpr uint32_t kVerticalWidth = 720;
    static constexpr uint32_t kVerticalHeight = 1280;

    static void request_cleanup_by_id(const std::string &id)
    {
        if (!g_multistream_manager) {
            return;
        }

        if (g_dock) {
            QTimer::singleShot(0, g_dock, [id]() {
                if (!g_multistream_manager) {
                    return;
                }
                g_multistream_manager->cleanup_runtime_for_destination(id);
            });
            return;
        }

        g_multistream_manager->cleanup_runtime_for_destination(id);
    }

    static void on_output_started(void *param, calldata_t *)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        set_runtime_status(runtime->destination, RuntimeState::Live);
        blog(LOG_INFO,
            "[obs-multistream-plugin] Output is live for platform=%s",
            runtime->destination.platform.c_str());
    }

    static void on_output_stopped(void *param, calldata_t *params)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        const Destination destination = runtime->destination;
        const std::string id = destination_id(destination);
        const long long code = params ? calldata_int(params, "code") : 0;
        if (code == 0) {
            set_runtime_status(destination, RuntimeState::Stopped);
            blog(LOG_INFO,
                "[obs-multistream-plugin] Output stopped for platform=%s",
                destination.platform.c_str());
            request_cleanup_by_id(id);
            return;
        }

        const std::string reason = callback_reason(params, "Output stopped with error");
        set_runtime_status(destination, RuntimeState::Failed, reason);
        blog(LOG_WARNING,
            "[obs-multistream-plugin] Output stop error for platform=%s: %s",
            destination.platform.c_str(),
            reason.c_str());

        if (g_multistream_manager && g_multistream_manager->stopping_) {
            request_cleanup_by_id(id);
        }
    }

    static void on_output_reconnect(void *param, calldata_t *params)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        const std::string reason = callback_reason(params, "Reconnecting");
        set_runtime_status(runtime->destination, RuntimeState::Starting, reason);
        blog(LOG_INFO,
            "[obs-multistream-plugin] Reconnect signaled for platform=%s: %s",
            runtime->destination.platform.c_str(),
            reason.c_str());
    }

    static void on_output_reconnect_success(void *param, calldata_t *)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        set_runtime_status(runtime->destination, RuntimeState::Live);
        blog(LOG_INFO,
            "[obs-multistream-plugin] Reconnect succeeded for platform=%s",
            runtime->destination.platform.c_str());
    }

    static void on_output_deactivate(void *param, calldata_t *params)
    {
        auto *runtime = static_cast<DestinationRuntime *>(param);
        const Destination destination = runtime->destination;
        const std::string id = destination_id(destination);
        const std::string reason = callback_reason(params, "Output deactivated");
        set_runtime_status(destination, RuntimeState::Failed, reason);
        blog(LOG_WARNING,
            "[obs-multistream-plugin] Output deactivated for platform=%s: %s",
            destination.platform.c_str(),
            reason.c_str());
        if (g_multistream_manager) {
            if (g_multistream_manager->stopping_) {
                request_cleanup_by_id(id);
            } else {
                g_multistream_manager->handle_runtime_deactivated(destination);
            }
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

            release_runtime_resources(runtime);

            runtimes_.erase(it);
            return;
        }
    }

    static void release_runtime_resources(DestinationRuntime *runtime)
    {
        if (!runtime) {
            return;
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

        if (runtime->video_encoder) {
            obs_encoder_release(runtime->video_encoder);
            runtime->video_encoder = nullptr;
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
            const std::string terminal_reason =
                "Retry limit reached (" + std::to_string(kMaxRetryAttempts) + "/" +
                std::to_string(kMaxRetryAttempts) + "): " + reason;
            set_runtime_terminal_failure(*candidate, terminal_reason);
            blog(LOG_ERROR,
                "[obs-multistream-plugin] Terminal failure for platform=%s: %s",
                candidate->platform.c_str(),
                terminal_reason.c_str());
            return;
        }

        ++retry.attempts;
        const int delay_ms = kBaseRetryDelayMs * retry.attempts;
        const std::string retry_message =
            "Retry " + std::to_string(retry.attempts) + "/" + std::to_string(kMaxRetryAttempts) + " in " +
            std::to_string(delay_ms / 1000) + "s: " + reason;
        set_runtime_retry_status(*candidate, retry.attempts, kMaxRetryAttempts, retry_message);
        blog(LOG_WARNING,
            "[obs-multistream-plugin] %s for platform=%s",
            retry_message.c_str(),
            candidate->platform.c_str());

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
        const size_t id_hash = std::hash<std::string>{}(destination_id(dst));
        return prefix + "_" + platform + "_" + std::to_string(id_hash);
    }

    static obs_encoder_t *create_vertical_video_encoder(const Destination &dst, obs_encoder_t *shared_video_encoder)
    {
        if (!shared_video_encoder) {
            return nullptr;
        }

        const char *video_encoder_id = obs_encoder_get_id(shared_video_encoder);
        if (!video_encoder_id || video_encoder_id[0] == '\0') {
            video_encoder_id = "obs_x264";
        }

        obs_data_t *video_settings = obs_data_create();
        const std::string encoder_name = make_safe_name("multistream_vertical_video_encoder", dst);
        obs_encoder_t *vertical_encoder =
            obs_video_encoder_create(video_encoder_id, encoder_name.c_str(), video_settings, nullptr);
        obs_data_release(video_settings);

        if (!vertical_encoder) {
            return nullptr;
        }

        obs_encoder_set_video(vertical_encoder, obs_get_video());
        obs_encoder_set_scaled_size(vertical_encoder, kVerticalWidth, kVerticalHeight);
        return vertical_encoder;
    }

    bool start_runtime(
        const Destination &dst,
        obs_service_t *service,
        obs_output_t *output,
        obs_encoder_t *video_encoder,
        obs_encoder_t *audio_encoder,
        obs_encoder_t *owned_video_encoder,
        const char *pipeline_name)
    {
        auto runtime = std::make_unique<DestinationRuntime>();
        runtime->destination = dst;
        runtime->service = service;
        runtime->output = output;
        runtime->video_encoder = owned_video_encoder;

        connect_output_callbacks(runtime.get());

        obs_output_set_service(output, service);
        obs_output_set_video_encoder(output, video_encoder);
        obs_output_set_audio_encoder(output, audio_encoder, 0);

        if (!obs_output_start(output)) {
            const char *last_error = obs_output_get_last_error(output);
            const std::string reason = (last_error && last_error[0] != '\0')
                ? std::string(last_error)
                : "Failed to start output";
            blog(LOG_ERROR,
                "[obs-multistream-plugin] Failed to start %s output for platform=%s: %s",
                pipeline_name,
                dst.platform.c_str(),
                reason.c_str());
            set_runtime_status(dst, RuntimeState::Failed, reason);
            disconnect_output_callbacks(runtime.get());
            if (owned_video_encoder) {
                obs_encoder_release(owned_video_encoder);
            }
            obs_output_release(output);
            obs_service_release(service);
            return false;
        }

        runtimes_.push_back(std::move(runtime));
        retry_infos_[destination_id(dst)].attempts = 0;

        const PlatformKind platform_kind = detect_platform_kind(dst.platform);
        blog(LOG_INFO,
            "[obs-multistream-plugin] Started %s secondary stream for platform=%s (kind=%s, protocol=%s)",
            pipeline_name,
            dst.platform.c_str(),
            platform_kind_name(platform_kind),
            dst.protocol.c_str());
        return true;
    }

    bool start_shared_destination(const Destination &dst, obs_encoder_t *video_encoder, obs_encoder_t *audio_encoder)
    {
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

        return start_runtime(dst, service, output, video_encoder, audio_encoder, nullptr, "shared");
    }

    bool start_vertical_destination(const Destination &dst, obs_encoder_t *shared_video_encoder, obs_encoder_t *audio_encoder)
    {
        obs_data_t *service_settings = obs_data_create();
        obs_data_set_string(service_settings, "server", dst.server.c_str());
        obs_data_set_string(service_settings, "key", dst.stream_key.c_str());

        const std::string service_name = make_safe_name("multistream_vertical_service", dst);
        obs_service_t *service = obs_service_create("rtmp_custom", service_name.c_str(), service_settings, nullptr);
        obs_data_release(service_settings);

        if (!service) {
            blog(LOG_ERROR,
                "[obs-multistream-plugin] Failed to create vertical service for platform=%s",
                dst.platform.c_str());
            return false;
        }

        const std::string output_name = make_safe_name("multistream_vertical_output", dst);
        obs_output_t *output = obs_output_create("rtmp_output", output_name.c_str(), nullptr, nullptr);
        if (!output) {
            blog(LOG_ERROR,
                "[obs-multistream-plugin] Failed to create vertical output for platform=%s",
                dst.platform.c_str());
            obs_service_release(service);
            return false;
        }

        obs_encoder_t *vertical_video_encoder = create_vertical_video_encoder(dst, shared_video_encoder);
        if (!vertical_video_encoder) {
            blog(LOG_ERROR,
                "[obs-multistream-plugin] Failed to create vertical video encoder for platform=%s",
                dst.platform.c_str());
            obs_output_release(output);
            obs_service_release(service);
            return false;
        }

        return start_runtime(dst, service, output, vertical_video_encoder, audio_encoder, vertical_video_encoder, "vertical");
    }

    bool start_single_destination(const Destination &dst, obs_encoder_t *video_encoder, obs_encoder_t *audio_encoder)
    {
        if (dst.requires_vertical) {
            return start_vertical_destination(dst, video_encoder, audio_encoder);
        }
        return start_shared_destination(dst, video_encoder, audio_encoder);
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

        if (has_duplicate_destination(g_destinations, dst)) {
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

    auto *header_layout = new QHBoxLayout();
    header_layout->addStretch(1);
    auto *version_label = new QLabel(QString::fromUtf8(kPluginUiVersion), container);
    version_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    header_layout->addWidget(version_label);
    layout->addLayout(header_layout);

    auto *form_widget = new QWidget(container);
    auto *form = new QFormLayout(form_widget);
    auto *platform = new QComboBox(form_widget);
    platform->addItem("YouTube");
    platform->addItem("Twitch");
    platform->addItem("Kick");
    auto *server = new QLineEdit(form_widget);
    server->setReadOnly(true);
    auto *stream_key = new QLineEdit(form_widget);
    stream_key->setEchoMode(QLineEdit::Password);

    form->addRow("Platform", platform);
    form->addRow("Server", server);
    form->addRow("Stream key", stream_key);

    auto *add_button = new QPushButton("Add destination", container);
    auto *edit_button = new QPushButton("Edit selected", container);
    auto *remove_button = new QPushButton("Remove selected", container);

    g_destinations_table = new QTableWidget(container);
    g_destinations_table->setColumnCount(7);
    g_destinations_table->setHorizontalHeaderLabels({"Enabled", "Platform", "Server", "Protocol", "Vertical", "Status", "Last error"});
    g_destinations_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    g_destinations_table->setSelectionMode(QAbstractItemView::SingleSelection);
    g_destinations_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    g_destinations_table->verticalHeader()->setVisible(false);
    g_destinations_table->horizontalHeader()->setStretchLastSection(true);

    auto update_server_for_platform = [platform, server]() {
        const PlatformKind kind = detect_platform_kind(platform->currentText().toStdString());
        server->setText(QString::fromStdString(default_server_for_platform(kind)));
    };
    update_server_for_platform();

    QObject::connect(platform, &QComboBox::currentTextChanged, [update_server_for_platform](const QString &) {
        update_server_for_platform();
    });

    QObject::connect(add_button, &QPushButton::clicked, [platform, server, stream_key, update_server_for_platform]() {
        Destination dst;
        dst.platform = platform->currentText().toStdString();
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

        stream_key->clear();
        update_server_for_platform();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination added");
    });

    QObject::connect(edit_button, &QPushButton::clicked, [platform, server, stream_key, update_server_for_platform]() {
        if (!g_destinations_table) {
            return;
        }

        const int row = g_destinations_table->currentRow();
        if (row < 0 || row >= static_cast<int>(g_destinations.size())) {
            blog(LOG_WARNING, "[obs-multistream-plugin] No destination selected to edit");
            return;
        }

        Destination updated = g_destinations[static_cast<size_t>(row)];
        updated.platform = platform->currentText().toStdString();
        updated.server = server->text().toStdString();
        updated.stream_key = stream_key->text().toStdString();

        normalize_destination(updated);
        if (!validate_destination(updated, row)) {
            return;
        }

        Destination &existing = g_destinations[static_cast<size_t>(row)];
        const std::string old_id = destination_id(existing);
        const std::string new_id = destination_id(updated);

        RuntimeStatus status;
        const auto status_it = g_runtime_statuses.find(old_id);
        if (status_it != g_runtime_statuses.end()) {
            status = status_it->second;
            g_runtime_statuses.erase(status_it);
        }

        existing.platform = updated.platform;
        existing.server = updated.server;
        existing.stream_key = updated.stream_key;
        existing.protocol = updated.protocol;
        existing.requires_vertical = updated.requires_vertical;

        g_runtime_statuses[new_id] = status;
        save_destinations();
        refresh_destinations_table();
        update_server_for_platform();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination edited for platform=%s", existing.platform.c_str());
    });

    QObject::connect(g_destinations_table, &QTableWidget::itemSelectionChanged, [platform, server, stream_key]() {
        if (!g_destinations_table) {
            return;
        }

        const int row = g_destinations_table->currentRow();
        if (row < 0 || row >= static_cast<int>(g_destinations.size())) {
            return;
        }

        const Destination &selected = g_destinations[static_cast<size_t>(row)];
        const int index = platform->findText(QString::fromStdString(selected.platform));
        if (index >= 0) {
            platform->setCurrentIndex(index);
        }
        server->setText(QString::fromStdString(selected.server));
        stream_key->setText(QString::fromStdString(selected.stream_key));
    });

    QObject::connect(g_destinations_table, &QTableWidget::itemChanged, [](QTableWidgetItem *item) {
        if (!item || !g_destinations_table || g_is_refreshing_table || item->column() != 0) {
            return;
        }

        const int row = item->row();
        if (row < 0 || row >= static_cast<int>(g_destinations.size())) {
            return;
        }

        Destination &dst = g_destinations[static_cast<size_t>(row)];
        const bool enabled = item->checkState() == Qt::Checked;
        if (dst.enabled == enabled) {
            return;
        }

        dst.enabled = enabled;
        save_destinations();
        refresh_destinations_table();
        blog(LOG_INFO, "[obs-multistream-plugin] Destination %s for platform=%s", enabled ? "enabled" : "disabled",
             dst.platform.c_str());
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

    auto *manage_layout = new QHBoxLayout();
    manage_layout->addWidget(edit_button);
    manage_layout->addWidget(remove_button);

    layout->addWidget(form_widget);
    layout->addWidget(add_button);
    layout->addWidget(g_destinations_table);
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
