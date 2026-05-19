#ifndef AGENTOS_TESTS_MOCKS_HPP
#define AGENTOS_TESTS_MOCKS_HPP

#include <gmock/gmock.h>
#include "agentos/dispatcher.h"
#include "agentos/registry.h"
#include "agentos/verifier.h"
#include "agentos/scheduler.h"
#include "agentos/orchestrator.h"
#include "agentos/obs_bus.h"
#include "agentos/types.h"

namespace agentos {

class MockDispatcher : public Dispatcher {
public:
    MockDispatcher() : Dispatcher("/tmp/test_socket") {}
    MOCK_METHOD(bool, bind, (), (override));
    MOCK_METHOD(bool, listen, (), (override));
    MOCK_METHOD(std::string, create_task_push, (const std::string& task_id), (override));
    MOCK_METHOD(void, close_task_push, (const std::string& task_id), (override));
    MOCK_METHOD(void, send_task, (const std::string& task_id, const std::string& task_json), (override));
    MOCK_METHOD(std::string, receive_result, (), (override));
    MOCK_METHOD(void, broadcast_event, (const std::string& event_json), (override));
    MOCK_METHOD(void, send_request, (const ClientId& client_id, const std::string& method,
                                     const std::string& params_json,
                                     std::function<void(const std::string&, const std::string&)> callback), (override));
    MOCK_METHOD(void, stop, (), (override));
};

class MockRegistry : public Registry {
public:
    MOCK_METHOD(void, register_adviser, (const RegisteredAdviser& agent), (override));
    MOCK_METHOD(void, register_worker, (const RegisteredExecutor& worker), (override));
    MOCK_METHOD(void, remove, (const ClientId& id), (override));
    MOCK_METHOD(std::optional<RegisteredAdviser>, find_adviser, (const std::string& domain), (const, override));
    MOCK_METHOD(std::optional<RegisteredExecutor>, find_worker_for_command, (const std::string& command), (const, override));
    MOCK_METHOD(std::optional<CommandSchema>, get_command_schema, (const std::string& command), (const, override));
    MOCK_METHOD(std::vector<CommandSchema>, all_command_schemas, (), (const, override));
    MOCK_METHOD(size_t, adviser_count, (), (const, override));
    MOCK_METHOD(size_t, worker_count, (), (const, override));
};

class MockVerifier : public Verifier {
public:
    MockVerifier(const Registry& registry) : Verifier(registry) {}
    MOCK_METHOD(VerifyResult, verify, (const Plan& plan), (const, override));
};

class MockScheduler : public Scheduler {
public:
    MockScheduler(const Registry& registry, Dispatcher& dispatcher) : Scheduler(registry, dispatcher) {}
    MOCK_METHOD(TaskResult, run, (const Plan& plan), (override));
};

class MockObsBus : public ObsBus {
public:
    MOCK_METHOD(void, emit, (const LogRecord& record), (override));
    MOCK_METHOD(void, task_submitted, (const TaskId& task_id, const std::string& goal), (override));
    MOCK_METHOD(void, task_completed, (const TaskResult& result), (override));
    MOCK_METHOD(void, step_started, (const TaskId& task_id, const std::string& step_id, const std::string& command), (override));
    MOCK_METHOD(void, step_completed, (const TaskId& task_id, const std::string& step_id, bool success), (override));
    MOCK_METHOD(void, client_connected, (const ClientId& id, ClientType type), (override));
    MOCK_METHOD(void, client_disconnected, (const ClientId& id), (override));
};

} // namespace agentos

#endif // AGENTOS_TESTS_MOCKS_HPP
