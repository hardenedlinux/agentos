#include "agentos/obs_bus.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace agentos {

[[maybe_unused]] static long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

void ObsBus::emit(const LogRecord& record) {
    // TODO Phase 3: route to configured sinks (stdout, file, OTLP)
    auto level = record.level;
    if      (level == LogLevel::DEBUG) spdlog::debug("[{}] {}", record.subsystem, record.message);
    else if (level == LogLevel::INFO)  spdlog::info ("[{}] {}", record.subsystem, record.message);
    else if (level == LogLevel::WARN)  spdlog::warn ("[{}] {}", record.subsystem, record.message);
    else                               spdlog::error("[{}] {}", record.subsystem, record.message);
}

void ObsBus::task_submitted(const TaskId& task_id, const std::string& goal) {
    spdlog::info("[obs] task_submitted id={} goal='{}'", task_id, goal);
}

void ObsBus::task_completed(const TaskResult& result) {
    if (result.success)
        spdlog::info("[obs] task_completed id={} OK", result.task_id);
    else
        spdlog::warn("[obs] task_completed id={} FAILED: {}", result.task_id, result.error);
}

void ObsBus::step_started(const TaskId& task_id, const std::string& step_id,
                           const std::string& command) {
    spdlog::info("[obs] step_started task={} step={} cmd={}", task_id, step_id, command);
}

void ObsBus::step_completed(const TaskId& task_id, const std::string& step_id, bool success) {
    spdlog::info("[obs] step_completed task={} step={} ok={}", task_id, step_id, success);
}

void ObsBus::client_connected(const ClientId& id, ClientType type) {
    spdlog::info("[obs] client_connected id={} type={}", id,
        type == ClientType::Adviser ? "adviser" : "worker");
}

void ObsBus::client_disconnected(const ClientId& id) {
    spdlog::info("[obs] client_disconnected id={}", id);
}

} // namespace agentos
