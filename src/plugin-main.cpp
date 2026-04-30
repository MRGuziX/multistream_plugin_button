#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/bmem.h>

#include <QTimer>

#include "multistream_manager.h"
#include "plugin_state.h"
#include "stream_key_storage.h"

void on_frontend_event(enum obs_frontend_event event, void *)
{
    switch (event) {
    case OBS_FRONTEND_EVENT_FINISHED_LOADING:
    case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
    case OBS_FRONTEND_EVENT_PROFILE_RENAMED:
        sync_default_destination_from_obs();
        return;
    default:
        break;
    }

    if (!g_multistream_manager) {
        return;
    }

    switch (event) {
    case OBS_FRONTEND_EVENT_STREAMING_STARTING:
        sync_default_destination_from_obs();
        break;
    case OBS_FRONTEND_EVENT_STREAMING_STARTED: {
        for (const Destination &dst : g_destinations) {
            if (dst.is_default) {
                set_runtime_status(dst, RuntimeState::Live);
            }
        }
        if (g_dock) {
            QTimer::singleShot(0, g_dock, []() {
                if (g_multistream_manager) {
                    g_multistream_manager->start_for_all_enabled(g_destinations);
                }
            });
        } else if (g_multistream_manager) {
            g_multistream_manager->start_for_all_enabled(g_destinations);
        }
        break;
    }
    case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
    case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
    case OBS_FRONTEND_EVENT_EXIT:
        for (const Destination &dst : g_destinations) {
            if (dst.is_default) {
                set_runtime_status(dst, RuntimeState::Stopped);
            }
        }
        g_multistream_manager->stop_all();
        break;
    default:
        break;
    }
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multistream-plugin", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "OBS plugin for multistreaming to multiple RTMP platforms with shared encoders.";
}

bool obs_module_load(void)
{
#if defined(STREAM_KEY_USE_OPENSSL)
    char *master_path = obs_module_config_path("stream_key_master.bin");
    if (master_path) {
        stream_key_set_master_key_file_path(master_path);
        bfree(master_path);
    }
#endif
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
