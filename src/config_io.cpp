#include "config_io.h"
#include "plugin_state.h"

#include "stream_key_storage.h"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <algorithm>
#include <string>

void sync_default_destination_from_obs()
{
    Destination default_dst;
    default_dst.is_default = true;
    default_dst.enabled = true;
    default_dst.platform = "OBS default";

    obs_service_t *primary_service = obs_frontend_get_streaming_service();
    if (primary_service) {
        obs_data_t *primary_settings = obs_service_get_settings(primary_service);
        if (primary_settings) {
            const char *server = obs_data_get_string(primary_settings, "server");
            default_dst.server = server ? server : "";

            const char *key = obs_data_get_string(primary_settings, "key");
            if (!key || key[0] == '\0') {
                key = obs_data_get_string(primary_settings, "stream_key");
            }
            default_dst.stream_key = key ? key : "";

            const char *service_name = obs_service_get_type(primary_service);
            if (service_name && std::string(service_name).find("rtmp_common") != std::string::npos) {
                const char *named = obs_data_get_string(primary_settings, "service");
                if (named && named[0] != '\0') {
                    default_dst.platform = named;
                }
            }
            obs_data_release(primary_settings);
        }
    }

    normalize_destination(default_dst);

    auto it = std::find_if(g_destinations.begin(), g_destinations.end(),
                           [](const Destination &d) { return d.is_default; });

    const bool has_valid = !default_dst.server.empty() && !default_dst.stream_key.empty();

    if (!has_valid) {
        if (it != g_destinations.end()) {
            {
                std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
                g_runtime_statuses.erase(destination_id(*it));
            }
            g_destinations.erase(it);
            refresh_destinations_table();
        }
        return;
    }

    if (it == g_destinations.end()) {
        {
            std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
            g_runtime_statuses[destination_id(default_dst)] = RuntimeStatus{};
        }
        g_destinations.insert(g_destinations.begin(), std::move(default_dst));
    } else {
        const std::string old_id = destination_id(*it);
        const std::string new_id = destination_id(default_dst);
        if (old_id != new_id) {
            std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
            auto status_it = g_runtime_statuses.find(old_id);
            RuntimeStatus status = status_it != g_runtime_statuses.end() ? status_it->second : RuntimeStatus{};
            if (status_it != g_runtime_statuses.end()) {
                g_runtime_statuses.erase(status_it);
            }
            g_runtime_statuses[new_id] = status;
        }
        *it = std::move(default_dst);
        if (it != g_destinations.begin()) {
            std::rotate(g_destinations.begin(), it, it + 1);
        }
    }

    refresh_destinations_table();
}

void load_destinations()
{
    load_destinations_from_file("obs-multistream-plugin.json");
}

void load_destinations_from_file(const char *filename)
{
    g_destinations.clear();
    {
        std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
        g_runtime_statuses.clear();
    }

    obs_data_t *config = obs_data_create_from_json_file_safe(filename, "bak");
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
        const std::string key_raw = obs_data_get_string(item, "stream_key");
        const std::string key_enc = obs_data_get_string(item, "stream_key_encoding");
        if (key_enc.empty()) {
            dst.stream_key = key_raw;
        } else if (!stream_key_unprotect_load(key_raw, key_enc, &dst.stream_key)) {
            blog(LOG_WARNING,
                 "[obs-multistream-plugin] Could not decrypt stream key for platform=%s; skipping entry",
                 dst.platform.c_str());
            obs_data_release(item);
            continue;
        }
        const char *protocol = obs_data_get_string(item, "protocol");
        if (protocol && protocol[0] != '\0') {
            dst.protocol = protocol;
        }
        dst.requires_vertical = obs_data_get_bool(item, "requires_vertical");
        dst.notes = obs_data_get_string(item, "notes");
        dst.enabled = obs_data_has_user_value(item, "enabled") ? obs_data_get_bool(item, "enabled") : true;
        dst.video_encoder_id = obs_data_get_string(item, "video_encoder_id");
        if (obs_data_has_user_value(item, "video_bitrate_kbps")) {
            dst.video_bitrate_kbps = static_cast<int>(obs_data_get_int(item, "video_bitrate_kbps"));
        }
        if (obs_data_has_user_value(item, "audio_bitrate_kbps")) {
            dst.audio_bitrate_kbps = static_cast<int>(obs_data_get_int(item, "audio_bitrate_kbps"));
        }

        normalize_destination(dst);

        if (obs_data_get_bool(item, "is_default")) {
            obs_data_release(item);
            continue;
        }

        if (dst.platform.empty() || dst.server.empty() || dst.stream_key.empty()) {
            blog(LOG_WARNING, "[obs-multistream-plugin] Skipping incomplete destination at index=%zu", i);
            obs_data_release(item);
            continue;
        }

        if (has_duplicate_destination(g_destinations, dst)) {
            blog(LOG_WARNING, "[obs-multistream-plugin] Skipping duplicate destination from config for platform=%s",
                 dst.platform.c_str());
            obs_data_release(item);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_runtime_status_mutex);
            g_runtime_statuses[destination_id(dst)] = RuntimeStatus{};
        }
        g_destinations.push_back(std::move(dst));
        obs_data_release(item);
    }

    obs_data_array_release(items);
    obs_data_release(config);
    blog(LOG_INFO, "[obs-multistream-plugin] Loaded %zu destination(s)", g_destinations.size());
}

void save_destinations()
{
    save_destinations_to_file("obs-multistream-plugin.json");
}

void save_destinations_to_file(const char *filename)
{
    obs_data_t *config = obs_data_create();
    obs_data_array_t *items = obs_data_array_create();

    for (const Destination &dst : g_destinations) {
        if (dst.is_default) {
            continue;
        }
        obs_data_t *item = obs_data_create();
        obs_data_set_string(item, "platform", dst.platform.c_str());
        obs_data_set_string(item, "server", dst.server.c_str());
        std::string key_blob;
        std::string key_enc;
        if (stream_key_protect_for_save(dst.stream_key, &key_blob, &key_enc)) {
            obs_data_set_string(item, "stream_key", key_blob.c_str());
            obs_data_set_string(item, "stream_key_encoding", key_enc.c_str());
        } else {
            obs_data_set_string(item, "stream_key", dst.stream_key.c_str());
            obs_data_set_string(item, "stream_key_encoding", "");
            blog(LOG_WARNING,
                 "[obs-multistream-plugin] Stream key not encrypted for platform=%s (OS/build lacks protection)",
                 dst.platform.c_str());
        }
        obs_data_set_string(item, "protocol", dst.protocol.c_str());
        obs_data_set_bool(item, "requires_vertical", dst.requires_vertical);
        obs_data_set_bool(item, "enabled", dst.enabled);
        obs_data_set_string(item, "notes", dst.notes.c_str());
        if (!dst.video_encoder_id.empty()) {
            obs_data_set_string(item, "video_encoder_id", dst.video_encoder_id.c_str());
        }
        if (dst.video_bitrate_kbps > 0) {
            obs_data_set_int(item, "video_bitrate_kbps", static_cast<long long>(dst.video_bitrate_kbps));
        }
        if (dst.audio_bitrate_kbps > 0) {
            obs_data_set_int(item, "audio_bitrate_kbps", static_cast<long long>(dst.audio_bitrate_kbps));
        }
        obs_data_array_push_back(items, item);
        obs_data_release(item);
    }

    obs_data_set_array(config, "destinations", items);
    obs_data_save_json_safe(config, filename, "tmp", "bak");

    obs_data_array_release(items);
    obs_data_release(config);
    blog(LOG_INFO, "[obs-multistream-plugin] Saved %zu destination(s)", g_destinations.size());
}
