#pragma once

#include "config_io.h"
#include "destination_rules.h"
#include "dock_ui.h"

#include <obs-frontend-api.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class QDockWidget;
class QTableWidget;
class MultistreamManager;

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

extern std::vector<Destination> g_destinations;
extern QDockWidget *g_dock;
extern QTableWidget *g_destinations_table;
extern bool g_is_refreshing_table;
extern std::unordered_map<std::string, RuntimeStatus> g_runtime_statuses;
extern std::mutex g_runtime_status_mutex;
extern std::unique_ptr<MultistreamManager> g_multistream_manager;

std::string destination_id(const Destination &dst);
const char *runtime_state_name(RuntimeState state);

void set_runtime_status(const Destination &dst, RuntimeState state, const std::string &last_error = "");
void set_runtime_retry_status(const Destination &dst, int attempt, int max_attempts, const std::string &retry_message);
void set_runtime_terminal_failure(const Destination &dst, const std::string &terminal_reason);
std::string last_runtime_error_for_destination(const Destination &dst);

void request_table_refresh();

bool validate_and_log_destination(const Destination &dst, int skip_duplicate_index = -1);

void on_frontend_event(enum obs_frontend_event event, void *priv);
