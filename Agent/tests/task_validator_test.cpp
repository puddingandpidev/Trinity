// ========================================== { START OF FILE } ========================================== //
// Host-side unit checks for the task command validator.
//
// `agent::task::MissingCommandField` is deliberately platform-neutral, so it can be compiled with
// the build host's compiler instead of the Windows target. That keeps the "a command missing a
// required field must name the field" contract testable without a guest.
//
// Build and run via: cmake --build Agent/out/build/<preset> --target check-units
#include <cstdio>
#include <string>
#include <vector>

#include "agent/models/task_types.h"
#include "agent/task/task_validator.h"

namespace {

    namespace models = agent::models;
    using CheckinResponse = models::CheckinResponse;
    using models::TaskCode;
    using models::TaskType;

    int g_failures = 0;
    int g_checks = 0;

    void ExpectEqual(const std::string& actual, const std::string& expected, const char* label) {
        ++g_checks;
        if (actual == expected) {
            return;
        }
        ++g_failures;
        std::printf("FAIL  %s\n        expected \"%s\"\n        actual   \"%s\"\n", label, expected.c_str(),
                    actual.c_str());
    }

    /// An upload task carrying both fields its type requires.
    CheckinResponse CompleteUpload() {
        CheckinResponse task;
        task.id = 1;
        task.type = static_cast<int>(TaskType::Upload);
        task.has_command = true;
        task.file_path = "C:\\Users\\pi-user\\probe.txt";
        task.transfer_id = "3";
        return task;
    }

    void TestUpload() {
        ExpectEqual(agent::task::MissingCommandField(CompleteUpload()), "", "upload: complete command passes");

        CheckinResponse no_transfer = CompleteUpload();
        no_transfer.transfer_id.clear();
        ExpectEqual(agent::task::MissingCommandField(no_transfer), "TransferID", "upload: dropped TransferID is named");

        CheckinResponse no_path = CompleteUpload();
        no_path.file_path.clear();
        ExpectEqual(agent::task::MissingCommandField(no_path), "Path", "upload: dropped Path is named");

        CheckinResponse download = CompleteUpload();
        download.type = static_cast<int>(TaskType::Download);
        ExpectEqual(agent::task::MissingCommandField(download), "", "download: complete command passes");
    }

    void TestShellAndPowershell() {
        CheckinResponse shell;
        shell.type = static_cast<int>(TaskType::Shell);
        shell.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(shell), "Command", "shell: missing Command is named");
        shell.run_command = "whoami";
        ExpectEqual(agent::task::MissingCommandField(shell), "", "shell: complete command passes");

        CheckinResponse powershell;
        powershell.type = static_cast<int>(TaskType::Powershell);
        powershell.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(powershell), "Commandlet",
                    "powershell: missing Commandlet is named");
        powershell.command.commandlet = "Get-Date";
        ExpectEqual(agent::task::MissingCommandField(powershell), "", "powershell: complete command passes");
    }

    void TestEscalate() {
        CheckinResponse escalate;
        escalate.type = static_cast<int>(TaskType::Escalate);
        escalate.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(escalate), "RegKey", "escalate: missing RegKey is named");
        escalate.reg_key = "fodhelper";
        ExpectEqual(agent::task::MissingCommandField(escalate), "TargetBinary",
                    "escalate: missing TargetBinary is named");
        escalate.target_binary = "C:\\Windows\\System32\\cmd.exe";
        ExpectEqual(agent::task::MissingCommandField(escalate), "", "escalate: complete command passes");
    }

    void TestToken() {
        CheckinResponse token;
        token.type = static_cast<int>(TaskType::Token);
        token.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(token), "Operation", "token: missing Operation is named");

        token.operation = "rev2self";
        ExpectEqual(agent::task::MissingCommandField(token), "", "token: rev2self needs no further fields");

        token.operation = "steal";
        ExpectEqual(agent::task::MissingCommandField(token), "PID", "token: steal without PID is named");
        token.pid = 4412;
        ExpectEqual(agent::task::MissingCommandField(token), "", "token: steal with PID passes");

        token.operation = "make";
        ExpectEqual(agent::task::MissingCommandField(token), "Username", "token: make without credentials is named");
        token.username = "pi-admin";
        token.password = "secret";
        ExpectEqual(agent::task::MissingCommandField(token), "", "token: make with credentials passes");
    }

    void TestKeyloggerAndProcess() {
        CheckinResponse keylogger;
        keylogger.type = static_cast<int>(TaskType::Keylogger);
        keylogger.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(keylogger), "Operation", "keylogger: missing Operation is named");
        keylogger.operation = "start";
        ExpectEqual(agent::task::MissingCommandField(keylogger), "", "keylogger: start passes");

        CheckinResponse runu;
        runu.type = static_cast<int>(TaskType::Process);
        runu.code = static_cast<int>(TaskCode::RunU);
        runu.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(runu), "PID", "runu: missing PID is named");
        runu.pid = 4412;
        ExpectEqual(agent::task::MissingCommandField(runu), "Command", "runu: missing Command is named");
        runu.run_command = "C:\\Windows\\System32\\cmd.exe";
        ExpectEqual(agent::task::MissingCommandField(runu), "", "runu: complete command passes");

        // KILL_PROCESS names the target PID; without it there is nothing to terminate.
        CheckinResponse kill;
        kill.type = static_cast<int>(TaskType::Process);
        kill.code = static_cast<int>(TaskCode::KillProcess);
        kill.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(kill), "PID", "kill: missing PID is named");
        kill.pid = 4412;
        ExpectEqual(agent::task::MissingCommandField(kill), "", "kill: complete command passes");

        // An unsupported PROCESS code is the dispatcher's business, not a missing field.
        CheckinResponse unknown_code = runu;
        unknown_code.code = static_cast<int>(TaskCode::RunAs);
        ExpectEqual(agent::task::MissingCommandField(unknown_code), "",
                    "process: unsupported code is left to the dispatcher");
    }

    void TestUnparseableCommand() {
        CheckinResponse no_command;
        no_command.type = static_cast<int>(TaskType::Shell);
        no_command.has_command = false;
        ExpectEqual(agent::task::MissingCommandField(no_command), "Command",
                    "no command payload: reported rather than dropped");
    }

    void TestDescribeTaskType() {
        ExpectEqual(agent::task::DescribeTaskType(static_cast<int>(TaskType::Upload), 0), "UPLOAD", "describe: upload");
        ExpectEqual(
            agent::task::DescribeTaskType(static_cast<int>(TaskType::Process), static_cast<int>(TaskCode::RunU)),
            "PROCESS/RUN_U", "describe: runu");
        ExpectEqual(
            agent::task::DescribeTaskType(static_cast<int>(TaskType::Process), static_cast<int>(TaskCode::KillProcess)),
            "PROCESS/KILL_PROCESS", "describe: kill process");
        ExpectEqual(agent::task::DescribeTaskType(99, 0), "type 99", "describe: unknown type");
    }

    void TestWinTask() {
        // WIN_TASK drops the payload at Path and registers it under TaskName; Username is optional
        // (empty means the running user) and is not a required field.
        CheckinResponse wintask;
        wintask.type = static_cast<int>(TaskType::WinTask);
        wintask.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(wintask), "Path", "wintask: missing Path is named");
        wintask.file_path = "C:\\Users\\pi-user\\e2e_persist.exe";
        ExpectEqual(agent::task::MissingCommandField(wintask), "TaskName", "wintask: missing TaskName is named");
        wintask.task_name = "TrinityE2E";
        ExpectEqual(agent::task::MissingCommandField(wintask), "", "wintask: complete command passes");

        ExpectEqual(agent::task::DescribeTaskType(static_cast<int>(TaskType::WinTask), 0), "WIN_TASK",
                    "describe: wintask");
        ExpectEqual(agent::task::DescribeTaskType(static_cast<int>(TaskType::WinTask), 99), "WIN_TASK",
                    "describe: wintask ignores the code");
    }

    void TestNetAsm() {
        // NET_ASM decodes Command.Assembly (base64) into the CLR; the argument vector is optional, so
        // Assembly is the only required wire key.
        CheckinResponse netasm;
        netasm.type = static_cast<int>(TaskType::NetAsm);
        netasm.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(netasm), "Assembly", "netasm: missing Assembly is named");
        netasm.assembly = "TVqQAAMAAAAEAAAA";
        ExpectEqual(agent::task::MissingCommandField(netasm), "", "netasm: complete command passes");

        ExpectEqual(agent::task::DescribeTaskType(static_cast<int>(TaskType::NetAsm), 0), "NET_ASM",
                    "describe: netasm");
    }

    void TestGetUidAndKill() {
        // GET_UID and KILL carry no command fields: an empty but present command must pass, and the
        // dispatcher still reports the wire name so a missing Command member is attributable.
        CheckinResponse getuid;
        getuid.type = static_cast<int>(TaskType::GetUid);
        getuid.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(getuid), "", "getuid: no required fields");

        CheckinResponse kill;
        kill.type = static_cast<int>(TaskType::Kill);
        kill.has_command = true;
        ExpectEqual(agent::task::MissingCommandField(kill), "", "kill: no required fields");

        ExpectEqual(agent::task::DescribeTaskType(static_cast<int>(TaskType::GetUid), 0), "GET_UID",
                    "describe: getuid");
        ExpectEqual(agent::task::DescribeTaskType(static_cast<int>(TaskType::Kill), 0), "KILL", "describe: kill");
    }

}  // namespace

int main() {
    TestUpload();
    TestShellAndPowershell();
    TestEscalate();
    TestToken();
    TestKeyloggerAndProcess();
    TestUnparseableCommand();
    TestDescribeTaskType();
    TestWinTask();
    TestNetAsm();
    TestGetUidAndKill();

    if (g_failures == 0) {
        std::printf("all %d validator checks passed\n", g_checks);
        return 0;
    }
    std::printf("%d of %d validator checks failed\n", g_failures, g_checks);
    return 1;
}
// ========================================== { END OF FILE } ========================================== //
