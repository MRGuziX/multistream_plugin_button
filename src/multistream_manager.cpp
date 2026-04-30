#include "multistream_manager.h"

#include "multistream_raii.h"
#include "plugin_state.h"

#include <obs-module.h>

#include <QTimer>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

using obs_multistream_detail::ObsEncoderHolder;
using obs_multistream_detail::ObsOutputHolder;
using obs_multistream_detail::ObsServiceHolder;

const Destination *MultistreamManager::find_enabled_destination_by_id(const std::string &id) const
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

void MultistreamManager::cleanup_runtime_for_destination(const std::string &id)
{
    for (const std::unique_ptr<DestinationRuntime> &runtime_ptr : runtimes_) {
        DestinationRuntime *runtime = runtime_ptr.get();
        if (destination_id(runtime->destination) != id) {
            continue;
        }

        release_runtime_resources(runtime);
        return;
    }
}

bool MultistreamManager::release_runtime_resources(DestinationRuntime *runtime)
{
    if (!runtime) {
        return true;
    }

    if (runtime->output) {
        if (obs_output_active(runtime->output)) {
            obs_output_stop(runtime->output);
            return false;
        }
        disconnect_output_callbacks(runtime);
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

    if (runtime->audio_encoder) {
        obs_encoder_release(runtime->audio_encoder);
        runtime->audio_encoder = nullptr;
    }

    return true;
}

void MultistreamManager::erase_runtime_by_id(const std::string &id)
{
    runtimes_.erase(
        std::remove_if(runtimes_.begin(), runtimes_.end(),
                       [&id](const std::unique_ptr<DestinationRuntime> &r) {
                           return r && destination_id(r->destination) == id;
                       }),
        runtimes_.end());
}

void MultistreamManager::schedule_retry(const Destination &dst, const std::string &reason)
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

std::string MultistreamManager::make_safe_name(const std::string &prefix, const Destination &dst)
{
    const std::string platform = dst.platform.empty() ? "unknown" : dst.platform;
    const size_t id_hash = std::hash<std::string>{}(destination_id(dst));
    return prefix + "_" + platform + "_" + std::to_string(id_hash);
}

static void apply_video_bitrate(obs_data_t *settings, int kbps)
{
    if (kbps > 0) {
        obs_data_set_int(settings, "bitrate", kbps);
    }
}

static void apply_audio_bitrate(obs_data_t *settings, int kbps)
{
    if (kbps > 0) {
        obs_data_set_int(settings, "bitrate", kbps);
    }
}

obs_encoder_t *MultistreamManager::create_owned_video_encoder(const Destination &dst, bool vertical)
{
    const std::string encoder_name =
        make_safe_name(vertical ? "multistream_vertical_video_encoder" : "multistream_video_encoder", dst);

    const char *primary_id = dst.video_encoder_id.empty() ? "obs_x264" : dst.video_encoder_id.c_str();

    auto try_create = [&](const char *id) -> obs_encoder_t * {
        obs_data_t *video_settings = obs_data_create();
        apply_video_bitrate(video_settings, dst.video_bitrate_kbps);
        obs_encoder_t *enc = obs_video_encoder_create(id, encoder_name.c_str(), video_settings, nullptr);
        obs_data_release(video_settings);
        return enc;
    };

    obs_encoder_t *encoder = try_create(primary_id);
    if (!encoder && strcmp(primary_id, "obs_x264") != 0) {
        blog(LOG_WARNING,
             "[obs-multistream-plugin] Video encoder '%s' unavailable; falling back to obs_x264 for platform=%s",
             primary_id, dst.platform.c_str());
        encoder = try_create("obs_x264");
    }

    if (!encoder) {
        return nullptr;
    }

    obs_encoder_set_video(encoder, obs_get_video());
    if (vertical) {
        obs_encoder_set_scaled_size(encoder, kVerticalWidth, kVerticalHeight);
    }
    return encoder;
}

obs_encoder_t *MultistreamManager::create_owned_audio_encoder(const Destination &dst)
{
    const std::string encoder_name = make_safe_name("multistream_audio_encoder", dst);
    obs_data_t *audio_settings = obs_data_create();
    apply_audio_bitrate(audio_settings, dst.audio_bitrate_kbps);
    obs_encoder_t *encoder =
        obs_audio_encoder_create("ffmpeg_aac", encoder_name.c_str(), audio_settings, 0, nullptr);
    obs_data_release(audio_settings);

    if (!encoder) {
        return nullptr;
    }
    obs_encoder_set_audio(encoder, obs_get_audio());
    return encoder;
}

bool MultistreamManager::start_single_destination(const Destination &dst)
{
    const bool vertical = dst.requires_vertical;
    const char *pipeline_name = vertical ? "vertical" : "shared";
    const std::string id = destination_id(dst);

    ObsServiceHolder service_holder;
    obs_data_t *service_settings = obs_data_create();
    obs_data_set_string(service_settings, "server", dst.server.c_str());
    obs_data_set_string(service_settings, "key", dst.stream_key.c_str());
    const std::string service_name =
        make_safe_name(vertical ? "multistream_vertical_service" : "multistream_service", dst);
    service_holder.p =
        obs_service_create("rtmp_custom", service_name.c_str(), service_settings, nullptr);
    obs_data_release(service_settings);
    if (!service_holder.p) {
        blog(LOG_ERROR, "[obs-multistream-plugin] Failed to create service for platform=%s",
             dst.platform.c_str());
        return false;
    }

    ObsOutputHolder output_holder;
    const std::string output_name =
        make_safe_name(vertical ? "multistream_vertical_output" : "multistream_output", dst);
    output_holder.p = obs_output_create("rtmp_output", output_name.c_str(), nullptr, nullptr);
    if (!output_holder.p) {
        blog(LOG_ERROR, "[obs-multistream-plugin] Failed to create output for platform=%s",
             dst.platform.c_str());
        return false;
    }

    ObsEncoderHolder video_holder;
    ObsEncoderHolder audio_holder;
    video_holder.p = create_owned_video_encoder(dst, vertical);
    audio_holder.p = create_owned_audio_encoder(dst);
    if (!video_holder.p || !audio_holder.p) {
        blog(LOG_ERROR, "[obs-multistream-plugin] Failed to create encoders for platform=%s",
             dst.platform.c_str());
        return false;
    }

    DestinationRuntime *runtime = nullptr;
    for (const std::unique_ptr<DestinationRuntime> &existing : runtimes_) {
        if (existing && destination_id(existing->destination) == id) {
            runtime = existing.get();
            break;
        }
    }
    if (!runtime) {
        auto new_runtime = std::make_unique<DestinationRuntime>();
        new_runtime->destination = dst;
        runtime = new_runtime.get();
        runtimes_.push_back(std::move(new_runtime));
    }
    runtime->destination = dst;
    runtime->service = service_holder.release();
    runtime->output = output_holder.release();
    runtime->video_encoder = video_holder.release();
    runtime->audio_encoder = audio_holder.release();

    connect_output_callbacks(runtime);

    obs_output_set_service(runtime->output, runtime->service);
    obs_output_set_video_encoder(runtime->output, runtime->video_encoder);
    obs_output_set_audio_encoder(runtime->output, runtime->audio_encoder, 0);

    if (!obs_output_start(runtime->output)) {
        const char *last_error = obs_output_get_last_error(runtime->output);
        const std::string reason =
            (last_error && last_error[0] != '\0') ? std::string(last_error) : "Failed to start output";
        blog(LOG_ERROR,
             "[obs-multistream-plugin] Failed to start %s output for platform=%s: %s",
             pipeline_name, dst.platform.c_str(), reason.c_str());
        set_runtime_status(dst, RuntimeState::Failed, reason);
        disconnect_output_callbacks(runtime);
        release_runtime_resources(runtime);
        erase_runtime_by_id(id);
        return false;
    }

    retry_infos_[id].attempts = 0;

    const PlatformKind platform_kind = detect_platform_kind(dst.platform);
    blog(LOG_INFO,
         "[obs-multistream-plugin] Started %s secondary stream for platform=%s (kind=%s, protocol=%s)",
         pipeline_name, dst.platform.c_str(),
         platform_kind_name(platform_kind), dst.protocol.c_str());
    return true;
}

void MultistreamManager::start_for_all_enabled(const std::vector<Destination> &destinations)
{
    stopping_ = false;
    stop_all();
    stopping_ = false;

    bool started_any = false;
    for (const Destination &dst : destinations) {
        if (!dst.enabled) {
            continue;
        }
        if (dst.is_default) {
            continue;
        }

        set_runtime_status(dst, RuntimeState::Starting);
        const bool started = start_single_destination(dst);
        if (!started) {
            const std::string reason = last_runtime_error_for_destination(dst).empty()
                ? "Failed to start output"
                : last_runtime_error_for_destination(dst);
            schedule_retry(dst, reason);
        }
        started_any = started_any || started;
    }

    if (!started_any) {
        blog(LOG_INFO, "[obs-multistream-plugin] No secondary multistream destination started");
    }
}

void MultistreamManager::stop_all()
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
}

void MultistreamManager::handle_runtime_deactivated(const Destination &dst)
{
    if (stopping_) {
        return;
    }

    cleanup_runtime_for_destination(destination_id(dst));
    schedule_retry(dst, "Output deactivated");
}

void MultistreamManager::retry_destination_by_id(const std::string &id, uint64_t retry_epoch)
{
    if (stopping_ || retry_epoch != retry_epoch_) {
        return;
    }

    const Destination *candidate = find_enabled_destination_by_id(id);
    if (!candidate || candidate->is_default) {
        return;
    }

    cleanup_runtime_for_destination(id);
    set_runtime_status(*candidate, RuntimeState::Starting);
    const bool started = start_single_destination(*candidate);

    if (!started) {
        const std::string reason = last_runtime_error_for_destination(*candidate).empty()
            ? "Retry start failed"
            : last_runtime_error_for_destination(*candidate);
        schedule_retry(*candidate, reason);
    }
}
