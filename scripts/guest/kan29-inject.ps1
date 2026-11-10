# kan29-inject.ps1 — duplicate the console user's token and start a program in that session.
# Runs as SYSTEM (scheduled task) because CreateProcessAsUser needs SeAssignPrimaryToken
# Privilege and SeIncreaseQuotaPrivilege, which an elevated administrator does not hold.
# Recipe: .pi/notes/interactive-sessions.md, "yoink that token" (decision policy step 2).
param(
    [Parameter(Mandatory = $true)][string]$Target,
    [string]$SessionUser = 'sky',
    [string]$ScriptFile = ''
)

$ErrorActionPreference = 'Stop'
$log = Join-Path $env:SystemRoot 'Temp\kan29-inject.log'
function Log([string]$m) {
    Add-Content -LiteralPath $log -Value ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m)
}
Set-Content -LiteralPath $log -Value ("[{0}] start target={1} user={2}" -f (Get-Date -Format 'HH:mm:ss'), $Target, $SessionUser)

$source = @'
using System;
using System.Runtime.InteropServices;

public static class Yoink
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
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
    public struct LUID { public uint LowPart; public int HighPart; }

    [StructLayout(LayoutKind.Sequential)]
    public struct LUID_AND_ATTRIBUTES { public LUID Luid; public uint Attributes; }

    [StructLayout(LayoutKind.Sequential)]
    public struct TOKEN_PRIVILEGES { public uint PrivilegeCount; public LUID_AND_ATTRIBUTES Privileges; }

    [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] public static extern int GetLastError();
    [DllImport("advapi32.dll", SetLastError = true)] public static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] public static extern bool DuplicateTokenEx(IntPtr existing, uint access, IntPtr attrs, int level, int type, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)] public static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TOKEN_PRIVILEGES newState, int bufferLength, IntPtr previousState, IntPtr returnLength);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)] public static extern bool LookupPrivilegeValue(string system, string name, out LUID luid);
    [DllImport("userenv.dll", SetLastError = true)] public static extern bool CreateEnvironmentBlock(out IntPtr env, IntPtr token, bool inherit);
    [DllImport("userenv.dll", SetLastError = true)] public static extern bool DestroyEnvironmentBlock(IntPtr env);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool CreateProcessAsUser(IntPtr token, string app, string cmdline, IntPtr pa, IntPtr ta,
        bool inherit, uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    const uint TOKEN_ASSIGN_PRIMARY = 0x0001;
    const uint TOKEN_DUPLICATE = 0x0002;
    const uint TOKEN_QUERY = 0x0008;
    const uint TOKEN_ADJUST_PRIVILEGES = 0x0020;
    const uint TOKEN_ALL_ACCESS = 0x000F01FF;
    const uint SE_PRIVILEGE_ENABLED = 0x00000002;
    const uint PROCESS_QUERY_INFORMATION = 0x0400;
    const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
    const int SecurityImpersonation = 2;
    const int TokenPrimary = 1;

    public static string EnablePrivileges()
    {
        IntPtr token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, out token))
            return "OpenProcessToken(self) failed: " + GetLastError();
        string names = "SeAssignPrimaryTokenPrivilege,SeIncreaseQuotaPrivilege,SeDebugPrivilege";
        foreach (string name in names.Split(','))
        {
            LUID luid;
            if (!LookupPrivilegeValue(null, name, out luid))
                return "LookupPrivilegeValue(" + name + ") failed: " + GetLastError();
            TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
            tp.PrivilegeCount = 1;
            tp.Privileges.Luid = luid;
            tp.Privileges.Attributes = SE_PRIVILEGE_ENABLED;
            if (!AdjustTokenPrivileges(token, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero))
                return "AdjustTokenPrivileges(" + name + ") failed: " + GetLastError();
            if (GetLastError() != 0)
                return "AdjustTokenPrivileges(" + name + ") last error: " + GetLastError();
        }
        return null;
    }

    public static int Spawn(int sourcePid, string app, string cmdLine, string desktop, out string error)
    {
        error = null;
        IntPtr proc = OpenProcess(PROCESS_QUERY_INFORMATION, false, sourcePid);
        if (proc == IntPtr.Zero) { error = "OpenProcess failed: " + GetLastError(); return 0; }
        IntPtr token;
        if (!OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY, out token))
        { error = "OpenProcessToken failed: " + GetLastError(); return 0; }
        IntPtr primary;
        if (!DuplicateTokenEx(token, TOKEN_ALL_ACCESS, IntPtr.Zero, SecurityImpersonation, TokenPrimary, out primary))
        { error = "DuplicateTokenEx failed: " + GetLastError(); return 0; }

        IntPtr env = IntPtr.Zero;
        CreateEnvironmentBlock(out env, primary, false);
        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(si);
        si.lpDesktop = desktop;
        PROCESS_INFORMATION pi;
        if (!CreateProcessAsUser(primary, app, cmdLine, IntPtr.Zero, IntPtr.Zero, false,
                                CREATE_UNICODE_ENVIRONMENT, env, null, ref si, out pi))
        { error = "CreateProcessAsUser failed: " + GetLastError(); return 0; }
        if (env != IntPtr.Zero) DestroyEnvironmentBlock(env);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return pi.dwProcessId;
    }
}
'@

Add-Type -TypeDefinition $source -Language CSharp

$problem = [Yoink]::EnablePrivileges()
if ($problem) { Log "privilege: $problem"; exit 1 }
Log 'privileges enabled (SeAssignPrimaryToken, SeIncreaseQuota, SeDebug)'

$explorer = Get-Process -Name explorer -IncludeUserName -ErrorAction Stop |
    Where-Object { $_.UserName -like "*\$SessionUser" } |
    Sort-Object StartTime -Descending |
    Select-Object -First 1

if (-not $explorer) { Log "no explorer.exe for $SessionUser"; exit 1 }
Log ("source pid={0} session={1} user={2}" -f $explorer.Id, $explorer.SessionId, $explorer.UserName)

# `out` parameters bind as [ref]; do not name it $error (PowerShell's automatic variable).
# Build the child command line here, so the caller never has to get quoting through
# powershell -> cmd -> schtasks (which mangles it).
if ($ScriptFile) {
    # Set lpApplicationName as well as the command line: leaving it null makes CreateProcess
    # resolve the first token itself, which failed with ERROR_PATH_NOT_FOUND (3) from SYSTEM.
    $CommandLine = '"' + $Target + '" -NoProfile -ExecutionPolicy Bypass -File "' + $ScriptFile + '"'
    $SpawnApp = $Target
} else {
    $CommandLine = $null
    $SpawnApp = $Target
}
Log ("child command line: " + $CommandLine)
$spawnError = ''
try {
    $child = [Yoink]::Spawn($explorer.Id, $SpawnApp, $CommandLine, 'winsta0\default', [ref]$spawnError)
} catch {
    Log ("spawn threw: " + $_.Exception.Message)
    exit 1
}
if ($child -eq 0) { Log "spawn failed: $spawnError"; exit 1 }

Start-Sleep -Milliseconds 800
$info = Get-Process -Id $child -ErrorAction SilentlyContinue
Log ("child pid={0} session={1} alive={2}" -f $child, $(if ($info) { $info.SessionId } else { 'gone' }), [bool]$info)
if ($info) { Log ("child user={0} integrity-check=see-marker" -f $info.UserName) }
