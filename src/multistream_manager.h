#pragma once

#include "destination_rules.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <obs.h>

class MultistreamManager {
public:
    void start_for_all_enabled(const std::vector<Destination> &destinations);
    void stop_all();
    void handle_runtime_deactivated(const Destination &dst);
    void retry_destination_by_id(const std::string &id, uint64_t retry_epoch);

private:
    struct DestinationRuntime {
        Destination destination;
        obs_service_t *service = nullptr;
        obs_output_t *output = nullptr;
        obs_encoder_t *video_encoder = nullptr;
        obs_encoder_t *audio_encoder = nullptr;
        bool sharing_encoder = false;
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

    static void request_cleanup_by_id(const std::string &id);
    static void on_output_started(void *param, calldata_t *);
    static void on_output_stopped(void *param, calldata_t *params);
    static void on_output_reconnect(void *param, calldata_t *params);
    static void on_output_reconnect_success(void *param, calldata_t *);
    static void on_output_deactivate(void *param, calldata_t *params);

    const Destination *find_enabled_destination_by_id(const std::string &id) const;
    void cleanup_runtime_for_destination(const std::string &id);
    static bool release_runtime_resources(DestinationRuntime *runtime);
    void erase_runtime_by_id(const std::string &id);
    void schedule_retry(const Destination &dst, const std::string &reason);
    static void connect_output_callbacks(DestinationRuntime *runtime);
    static void disconnect_output_callbacks(DestinationRuntime *runtime);
    static std::string make_safe_name(const std::string &prefix, const Destination &dst);

    bool start_single_destination(const Destination &dst);
};
