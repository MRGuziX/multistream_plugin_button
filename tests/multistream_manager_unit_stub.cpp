// Test-only stub: satisfies the linker for std::unique_ptr<MultistreamManager> and ODR without
// linking the real multistream_manager*.cpp (no Qt in unit-tests).
#include "multistream_manager.h"

#include "plugin_state.h"

#include <string>

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

void MultistreamManager::cleanup_runtime_for_destination(const std::string &) {}

bool MultistreamManager::release_runtime_resources(DestinationRuntime *)
{
    return true;
}

void MultistreamManager::erase_runtime_by_id(const std::string &) {}

void MultistreamManager::schedule_retry(const Destination &, const std::string &) {}

std::string MultistreamManager::make_safe_name(const std::string &prefix, const Destination &dst)
{
    const std::string platform = dst.platform.empty() ? "unknown" : dst.platform;
    const size_t id_hash = std::hash<std::string>{}(destination_id(dst));
    return prefix + "_" + platform + "_" + std::to_string(id_hash);
}

bool MultistreamManager::start_single_destination(const Destination &)
{
    return false;
}

void MultistreamManager::start_for_all_enabled(const std::vector<Destination> &) {}

void MultistreamManager::stop_all() {}

void MultistreamManager::handle_runtime_deactivated(const Destination &) {}

void MultistreamManager::retry_destination_by_id(const std::string &, uint64_t) {}

void MultistreamManager::request_cleanup_by_id(const std::string &id)
{
    if (g_multistream_manager) {
        g_multistream_manager->cleanup_runtime_for_destination(id);
    }
}

void MultistreamManager::on_output_started(void *, calldata_t *) {}

void MultistreamManager::on_output_stopped(void *, calldata_t *) {}

void MultistreamManager::on_output_reconnect(void *, calldata_t *) {}

void MultistreamManager::on_output_reconnect_success(void *, calldata_t *) {}

void MultistreamManager::on_output_deactivate(void *, calldata_t *) {}

void MultistreamManager::connect_output_callbacks(DestinationRuntime *) {}

void MultistreamManager::disconnect_output_callbacks(DestinationRuntime *) {}
