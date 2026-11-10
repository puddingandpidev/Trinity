// ========================================== { START OF FILE } ========================================== //
// Host-side checks for task deserialisation and result serialisation.
//
// Both are platform-neutral (rapidjson over the parsed model), so they compile and run on the build
// host instead of the Windows target. Together with task_validator_test.cpp this covers the
// "task deserialisation, serialisation and error handling" part of the agent test requirement
// (spec 12.1.4.1).
//
// Build and run via: cmake --build Agent/out/build/<preset> --target check-units
// (or `ctest` in the build directory after that target has produced the binaries).
#include <cstdio>
#include <string>

#include "agent/models/checkin_models.h"
#include "agent/task/task_queue_manager.h"

namespace {

    int g_failures = 0;
    int g_checks = 0;

    void ExpectTrue(bool condition, const char* label) {
        ++g_checks;
        if (condition) {
            return;
        }
        ++g_failures;
        std::printf("FAIL  %s\n", label);
    }

    void ExpectInt(int actual, int expected, const char* label) {
        ++g_checks;
        if (actual == expected) {
            return;
        }
        ++g_failures;
        std::printf("FAIL  %s\n        expected %d\n        actual   %d\n", label, expected, actual);
    }

    void ExpectContains(const std::string& haystack, const std::string& needle, const char* label) {
        ++g_checks;
        if (haystack.find(needle) != std::string::npos) {
            return;
        }
        ++g_failures;
        std::printf("FAIL  %s\n        missing \"%s\" in: %s\n", label, needle.c_str(), haystack.c_str());
    }

    /// The TeamServer serialises TaskRequest with PascalCase keys.
    void TestPascalCaseTask() {
        agent::task::TaskQueueManager manager;
        manager.QueueTask(R"({"TaskID":7,"TaskType":8,"TaskCode":0,"Status":4,"Command":{"Command":"whoami"}})");
        const agent::models::CheckinResponse task = manager.GetNextTask();
        ExpectInt(task.id, 7, "pascal: TaskID");
        ExpectInt(task.type, 8, "pascal: TaskType");
        ExpectTrue(task.has_command, "pascal: has_command");
        ExpectTrue(task.run_command == "whoami", "pascal: shell Command");
    }

    /// Older payloads and the legacy lower-case keys must still parse.
    void TestLegacyLowerCaseTask() {
        agent::task::TaskQueueManager manager;
        manager.QueueTask(R"({"id":3,"type":5,"code":7,"command":{"PID":4412}})");
        const agent::models::CheckinResponse task = manager.GetNextTask();
        ExpectInt(task.id, 3, "legacy: id");
        ExpectInt(task.type, 5, "legacy: type");
        ExpectInt(task.code, 7, "legacy: code");
        ExpectInt(task.pid, 4412, "legacy: PID");
        ExpectTrue(task.has_command, "legacy: has_command");
    }

    /// A task that never parsed must be reported, not dropped: has_command stays false.
    void TestUnparsableTask() {
        agent::task::TaskQueueManager manager;
        manager.QueueTask("not json");
        const agent::models::CheckinResponse task = manager.GetNextTask();
        ExpectInt(task.id, 0, "unparsable: no id");
        ExpectTrue(!task.has_command, "unparsable: has_command stays false");
    }

    /// A task with no Command member has nothing to validate, so has_command is false too.
    void TestMissingCommand() {
        agent::task::TaskQueueManager manager;
        manager.QueueTask(R"({"TaskID":9,"TaskType":8,"TaskCode":0})");
        const agent::models::CheckinResponse task = manager.GetNextTask();
        ExpectTrue(!task.has_command, "no Command: has_command false");
    }

    /// NET_ASM carries a base64 Assembly blob plus its argument vector in Command.Arguments.
    void TestNetAsmTask() {
        agent::task::TaskQueueManager manager;
        manager.QueueTask(
            R"({"TaskID":12,"TaskType":10,"TaskCode":0,"Command":{"Assembly":"TVqQAAMAAAAEAAAA","Arguments":"--x y"}})");
        const agent::models::CheckinResponse task = manager.GetNextTask();
        ExpectInt(task.id, 12, "netasm: TaskID");
        ExpectInt(task.type, 10, "netasm: TaskType");
        ExpectTrue(task.assembly == "TVqQAAMAAAAEAAAA", "netasm: Assembly");
        ExpectTrue(task.command.arguments == "--x y", "netasm: Arguments");
    }

    /// Status=2 carries Data.Output; Status=1 carries Data.ErrorCode/Data.Message.
    void TestResultSerialisation() {
        agent::models::ResultRequest success;
        success.task_id = 5;
        success.task_type = 8;
        success.task_code = 0;
        success.is_successful = true;
        success.output = "whoami output";
        const std::string success_json = success.ToJson();
        ExpectContains(success_json, R"("Status":2)", "result: success Status");
        ExpectContains(success_json, R"("Output":"whoami output")", "result: success Output");

        agent::models::ResultRequest failure;
        failure.task_id = 6;
        failure.task_type = 8;
        failure.task_code = 0;
        failure.is_successful = false;
        failure.output = "missing Path";
        const std::string failure_json = failure.ToJson();
        ExpectContains(failure_json, R"("Status":1)", "result: error Status");
        ExpectContains(failure_json, "\"ErrorCode\":1", "result: ErrorCode");
        ExpectContains(failure_json, R"("Message":"missing Path")", "result: error Message");
    }

}  // namespace

int main() {
    TestPascalCaseTask();
    TestLegacyLowerCaseTask();
    TestUnparsableTask();
    TestMissingCommand();
    TestNetAsmTask();
    TestResultSerialisation();

    if (g_failures == 0) {
        std::printf("all %d parsing checks passed\n", g_checks);
        return 0;
    }
    std::printf("%d of %d parsing checks failed\n", g_failures, g_checks);
    return 1;
}
// ========================================== { END OF FILE } ========================================== //
