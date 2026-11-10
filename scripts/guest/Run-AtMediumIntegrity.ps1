# ========================================== { START OF FILE } ========================================== #
# scripts/guest/Run-AtMediumIntegrity.ps1
#
# Launches a process with a Medium-integrity token so the Trinity Agent can be tested from
# a *non-elevated* context even when the SSH session itself is elevated.
#
# Why this is needed: OpenSSH on Windows runs the shell in session 0 with a full/elevated
# token for members of Administrators, so the escalate (UAC bypass) command has nothing to
# bypass. A real UAC-filtered token cannot be synthesised here: CreateRestrictedToken with
# LUA_TOKEN makes the child die with STATUS_DLL_INIT_FAILED (0xC0000142) in this session.
#
# Instead this duplicates the current token and lowers TokenIntegrityLevel to Medium
# (S-1-16-8192), then starts the target with CreateProcessAsUser. The result is a
# non-elevated (Medium IL) process owned by an administrator - the same starting point a
# UAC bypass expects. Note: the Administrators group stays enabled (a true UAC token has it
# deny-only), which does not affect what FodHelper auto-elevation does.
#
# Reference: https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-settokeninformation
#            (TokenIntegrityLevel), and
#            https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessasuser
#
# Usage: powershell -File Run-AtMediumIntegrity.ps1 -FilePath <exe> [-Arguments <args>]
# Prints: "started pid=<pid>" (and "warning: ..." when the child failed to start)
# ========================================== { END OF FILE } ========================================== #

param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [string]$Arguments = ""
)

$ErrorActionPreference = "Stop"

if (-not ("MediumLauncher" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class MediumLauncher
{
    [StructLayout(LayoutKind.Sequential)]
    public struct STARTUPINFO
    {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION
    {
        public IntPtr hProcess, hThread;
        public int dwProcessId, dwThreadId;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SID_AND_ATTRIBUTES
    {
        public IntPtr Sid;
        public uint Attributes;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct TOKEN_MANDATORY_LABEL
    {
        public SID_AND_ATTRIBUTES Label;
    }

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool DuplicateTokenEx(
        IntPtr ExistingToken, uint DesiredAccess, IntPtr TokenAttributes,
        int ImpersonationLevel, int TokenType, out IntPtr NewToken);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ConvertStringSidToSidW(string StringSid, out IntPtr Sid);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool SetTokenInformation(
        IntPtr TokenHandle, int TokenInformationClass,
        ref TOKEN_MANDATORY_LABEL TokenInformation, int TokenInformationLength);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool CreateProcessAsUser(
        IntPtr hToken, string lpApplicationName, string lpCommandLine,
        IntPtr lpProcessAttributes, IntPtr lpThreadAttributes,
        bool bInheritHandles, uint dwCreationFlags, IntPtr lpEnvironment,
        string lpCurrentDirectory, ref STARTUPINFO lpStartupInfo,
        out PROCESS_INFORMATION lpProcessInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr hProcess, out uint lpExitCode);

    const uint TOKEN_ASSIGN_PRIMARY = 0x0001;
    const uint TOKEN_DUPLICATE = 0x0002;
    const uint TOKEN_QUERY = 0x0008;
    const uint MAXIMUM_ALLOWED = 0x02000000;
    const int TokenIntegrityLevel = 25;
    const uint SE_GROUP_INTEGRITY = 0x00000020;
    const uint CREATE_NO_WINDOW = 0x08000000;

    public static int Launch(string commandLine, string integritySid, out string error)
    {
        error = "";
        IntPtr token = IntPtr.Zero, dup = IntPtr.Zero, sid = IntPtr.Zero;

        if (!OpenProcessToken(
                System.Diagnostics.Process.GetCurrentProcess().Handle,
                TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY,
                out token))
        {
            error = "OpenProcessToken failed: " + Marshal.GetLastWin32Error();
            return 0;
        }

        try
        {
            if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, IntPtr.Zero, 2 /* SecurityImpersonation */,
                                  1 /* TokenPrimary */, out dup))
            {
                error = "DuplicateTokenEx failed: " + Marshal.GetLastWin32Error();
                return 0;
            }

            if (!ConvertStringSidToSidW(integritySid, out sid))
            {
                error = "ConvertStringSidToSidW failed: " + Marshal.GetLastWin32Error();
                return 0;
            }

            TOKEN_MANDATORY_LABEL label = new TOKEN_MANDATORY_LABEL();
            label.Label.Sid = sid;
            label.Label.Attributes = SE_GROUP_INTEGRITY;

            int labelSize = Marshal.SizeOf(typeof(TOKEN_MANDATORY_LABEL)) + 8; // + SID sub-authorities
            if (!SetTokenInformation(dup, TokenIntegrityLevel, ref label, labelSize))
            {
                error = "SetTokenInformation failed: " + Marshal.GetLastWin32Error();
                return 0;
            }

            STARTUPINFO si = new STARTUPINFO();
            si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            PROCESS_INFORMATION pi;

            if (!CreateProcessAsUser(dup, null, commandLine, IntPtr.Zero, IntPtr.Zero, false,
                                     CREATE_NO_WINDOW, IntPtr.Zero, null, ref si, out pi))
            {
                error = "CreateProcessAsUser failed: " + Marshal.GetLastWin32Error();
                return 0;
            }

            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return pi.dwProcessId;
        }
        finally
        {
            if (sid != IntPtr.Zero) CloseHandle(sid);
            if (dup != IntPtr.Zero) CloseHandle(dup);
            if (token != IntPtr.Zero) CloseHandle(token);
        }
    }
}
"@
}

$cmd = "`"$FilePath`""
if ($Arguments) { $cmd = "$cmd $Arguments" }

$err = ""
$pidValue = [MediumLauncher]::Launch($cmd, "S-1-16-8192", [ref]$err)
if ($pidValue -eq 0) {
    Write-Output "error: $err"
    exit 1
}

Write-Output "started pid=$pidValue"
if ($err) { Write-Output "warning: $err" }
