#include "multistream_manager.h"

#include "config_io.h"
#include "destination_rules.h"
#include "multistream_raii.h"
#include "plugin_state.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QDockWidget>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

// Two encoder type IDs are shareable when they produce the same codec on
// the same hardware backend.  OBS can register multiple implementations of
// the same encoder (e.g. "ffmpeg_nvenc" vs "jim_nvenc") — they have
// different type IDs but identical display names and codec output, so
// sharing one instance across outputs that selected either variant is safe.
static bool encoders_shareable(const char *id_a, const char *id_b)
{
    if (!id_a || !id_b)
        return false;
    if (strcmp(id_a, id_b) == 0)
        return true;
    const char *codec_a = obs_get_encoder_codec(id_a);
    const char *codec_b = obs_get_encoder_codec(id_b);
    if (!codec_a || !codec_b || strcmp(codec_a, codec_b) != 0)
        return false;
    const char *disp_a = obs_encoder_get_display_name(id_a);
    const char *disp_b = obs_encoder_get_display_name(id_b);
    if (!disp_a || !disp_b)
        return false;
    return strcmp(disp_a, disp_b) == 0;
}

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

    runtime->sharing_encoder = false;
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

    obs_output_t *main_out = obs_frontend_get_streaming_output();
    if (!main_out) {
        blog(LOG_ERROR, "[obs-multistream-plugin] Main streaming output not available for platform=%s",
             dst.platform.c_str());
        return false;
    }
    obs_encoder_t *main_venc = obs_output_get_video_encoder(main_out);
    obs_encoder_t *main_aenc = obs_output_get_audio_encoder(main_out, 0);
    obs_output_release(main_out);
    if (!main_venc || !main_aenc) {
        blog(LOG_ERROR, "[obs-multistream-plugin] Main stream encoders not available for platform=%s",
             dst.platform.c_str());
        return false;
    }

    const char *main_enc_id = obs_encoder_get_id(main_venc);
    const std::string wanted_enc = dst.video_encoder_id.empty() ? (main_enc_id ? main_enc_id : "obs_x264")
                                                                : dst.video_encoder_id;

    blog(LOG_INFO,
         "[obs-multistream-plugin] Encoder lookup for platform=%s: wanted='%s', main='%s', dst_config='%s'",
         dst.platform.c_str(), wanted_enc.c_str(),
         main_enc_id ? main_enc_id : "(null)",
         dst.video_encoder_id.c_str());

    obs_encoder_t *venc = nullptr;
    obs_encoder_t *aenc = nullptr;
    bool sharing = false;

    // Search every running encoder (main + all secondaries) for a
    // shareable match.  Sharing via obs_encoder_get_ref() lets OBS route
    // the same encoded data to multiple RTMP outputs without spawning
    // extra hardware-encoder sessions (same mechanism as obs-multi-rtmp).
    struct EncoderSource {
        obs_encoder_t *video;
        obs_encoder_t *audio;
        bool is_vertical;
        const char *label;
    };

    std::vector<EncoderSource> candidates;
    if (main_venc && main_aenc && main_enc_id)
        candidates.push_back({main_venc, main_aenc, false, "main"});
    for (const std::unique_ptr<DestinationRuntime> &rt : runtimes_) {
        if (!rt || !rt->video_encoder || !rt->audio_encoder)
            continue;
        candidates.push_back({rt->video_encoder, rt->audio_encoder,
                              rt->destination.requires_vertical,
                              rt->destination.platform.c_str()});
    }

    for (const EncoderSource &src : candidates) {
        if (src.is_vertical != vertical)
            continue;
        const char *enc_id = obs_encoder_get_id(src.video);
        const bool shareable = encoders_shareable(wanted_enc.c_str(), enc_id);
        blog(LOG_INFO,
             "[obs-multistream-plugin]   candidate from %s: id='%s', shareable=%s",
             src.label, enc_id ? enc_id : "(null)", shareable ? "YES" : "NO");
        if (!shareable)
            continue;
        venc = obs_encoder_get_ref(src.video);
        aenc = obs_encoder_get_ref(src.audio);
        if (venc && aenc) {
            sharing = true;
            blog(LOG_INFO,
                 "[obs-multistream-plugin] Sharing encoder '%s' from %s stream for platform=%s",
                 enc_id, src.label, dst.platform.c_str());
            break;
        }
        if (venc) { obs_encoder_release(venc); venc = nullptr; }
        if (aenc) { obs_encoder_release(aenc); aenc = nullptr; }
    }

    // No shareable encoder found — create a dedicated instance with
    // streaming-safe defaults.  Hardware encoders like NVENC need explicit
    // keyint_sec and rate_control; their built-in defaults produce output
    // that ingest servers (especially Twitch) reject.
    if (!sharing) {
        blog(LOG_INFO,
             "[obs-multistream-plugin] No shareable encoder found for platform=%s, creating new '%s' instance",
             dst.platform.c_str(), wanted_enc.c_str());
        const std::string vname = make_safe_name("multistream_video", dst);

        obs_data_t *vs = obs_data_create();
        int bitrate = dst.video_bitrate_kbps;
        if (bitrate <= 0) {
            config_t *profile = obs_frontend_get_profile_config();
            if (profile) {
                const char *mode = config_get_string(profile, "Output", "Mode");
                if (mode && strcmp(mode, "Advanced") == 0)
                    bitrate = static_cast<int>(config_get_int(profile, "AdvOut", "VBitrate"));
                else
                    bitrate = static_cast<int>(config_get_int(profile, "SimpleOutput", "VBitrate"));
            }
        }
        if (bitrate > 0)
            obs_data_set_int(vs, "bitrate", bitrate);
        obs_data_set_int(vs, "keyint_sec", 2);
        obs_data_set_string(vs, "rate_control", "CBR");

        venc = obs_video_encoder_create(wanted_enc.c_str(), vname.c_str(), vs, nullptr);
        obs_data_release(vs);
        if (!venc && wanted_enc != "obs_x264") {
            blog(LOG_WARNING, "[obs-multistream-plugin] Encoder '%s' unavailable, falling back to x264 for platform=%s",
                 wanted_enc.c_str(), dst.platform.c_str());
            obs_data_t *vs2 = obs_data_create();
            if (bitrate > 0) obs_data_set_int(vs2, "bitrate", bitrate);
            obs_data_set_int(vs2, "keyint_sec", 2);
            obs_data_set_string(vs2, "rate_control", "CBR");
            venc = obs_video_encoder_create("obs_x264", vname.c_str(), vs2, nullptr);
            obs_data_release(vs2);
        }
        if (venc) {
            obs_encoder_set_video(venc, obs_get_video());
            if (vertical) obs_encoder_set_scaled_size(venc, kVerticalWidth, kVerticalHeight);
        }

        const std::string aname = make_safe_name("multistream_audio", dst);
        obs_data_t *as = obs_data_create();
        if (dst.audio_bitrate_kbps > 0) obs_data_set_int(as, "bitrate", dst.audio_bitrate_kbps);
        aenc = obs_audio_encoder_create("ffmpeg_aac", aname.c_str(), as, 0, nullptr);
        obs_data_release(as);
        if (aenc) obs_encoder_set_audio(aenc, obs_get_audio());
    }

    if (!venc || !aenc) {
        if (venc) obs_encoder_release(venc);
        if (aenc) obs_encoder_release(aenc);
        blog(LOG_ERROR, "[obs-multistream-plugin] Failed to create encoders for platform=%s", dst.platform.c_str());
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
    runtime->video_encoder = venc;
    runtime->audio_encoder = aenc;
    runtime->sharing_encoder = sharing;

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
