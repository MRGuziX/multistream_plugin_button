#include "multistream_manager.h"

#include "plugin_state.h"

#include <obs-module.h>

#include <QDockWidget>
#include <QTimer>

#include <string>

namespace {

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

} // namespace

void MultistreamManager::request_cleanup_by_id(const std::string &id)
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

void MultistreamManager::on_output_started(void *param, calldata_t *)
{
    auto *runtime = static_cast<DestinationRuntime *>(param);
    set_runtime_status(runtime->destination, RuntimeState::Live);
    blog(LOG_INFO,
         "[obs-multistream-plugin] Output is live for platform=%s",
         runtime->destination.platform.c_str());
}

void MultistreamManager::on_output_stopped(void *param, calldata_t *params)
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

void MultistreamManager::on_output_reconnect(void *param, calldata_t *params)
{
    auto *runtime = static_cast<DestinationRuntime *>(param);
    const std::string reason = callback_reason(params, "Reconnecting");
    set_runtime_status(runtime->destination, RuntimeState::Starting, reason);
    blog(LOG_INFO,
         "[obs-multistream-plugin] Reconnect signaled for platform=%s: %s",
         runtime->destination.platform.c_str(),
         reason.c_str());
}

void MultistreamManager::on_output_reconnect_success(void *param, calldata_t *)
{
    auto *runtime = static_cast<DestinationRuntime *>(param);
    set_runtime_status(runtime->destination, RuntimeState::Live);
    blog(LOG_INFO,
         "[obs-multistream-plugin] Reconnect succeeded for platform=%s",
         runtime->destination.platform.c_str());
}

void MultistreamManager::on_output_deactivate(void *param, calldata_t *params)
{
    auto *runtime = static_cast<DestinationRuntime *>(param);
    if (!runtime->output) {
        return;
    }

    /* libobs ends data capture (and signals "deactivate") while RTMP outputs reconnect after a drop.
     * Treating that as a hard failure schedules our own cleanup/retry and fights OBS's reconnect,
     * which can loop (e.g. Twitch secondary while YouTube primary is live). */
    if (obs_output_reconnecting(runtime->output)) {
        blog(LOG_INFO,
             "[obs-multistream-plugin] Ignoring deactivate for platform=%s (output reconnect in progress)",
             runtime->destination.platform.c_str());
        return;
    }

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
        } else if (g_dock) {
            QTimer::singleShot(0, g_dock, [destination]() {
                if (g_multistream_manager) {
                    g_multistream_manager->handle_runtime_deactivated(destination);
                }
            });
        }
    }
}

void MultistreamManager::connect_output_callbacks(DestinationRuntime *runtime)
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

void MultistreamManager::disconnect_output_callbacks(DestinationRuntime *runtime)
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
