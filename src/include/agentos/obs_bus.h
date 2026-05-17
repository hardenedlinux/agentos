#pragma once
/**
 * agentos/obs_bus.h
 *
 * ObsBus — structured observability aggregator.
 * Collects logs, metrics, and task lifecycle events from all subsystems.
 */

#include <string>
#include "agentos/types.h"

namespace agentos {

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

struct LogRecord {
    LogLevel    level;
    std::string subsystem;   // "dispatcher" | "registry" | "verifier" | "scheduler" | "orchestrator"
    std::string client_id;   // empty if not client-specific
    std::string task_id;     // empty if not task-specific
    std::string message;
    long long   timestamp_ms;
};

class ObsBus {
public:
    void emit(const LogRecord& record);
    void task_submitted(const TaskId& task_id, const std::string& goal);
    void task_completed(const TaskResult& result);
    void step_started(const TaskId& task_id, const std::string& step_id, const std::string& command);
    void step_completed(const TaskId& task_id, const std::string& step_id, bool success);
    void client_connected(const ClientId& id, ClientType type);
    void client_disconnected(const ClientId& id);
};

} // namespace agentos
