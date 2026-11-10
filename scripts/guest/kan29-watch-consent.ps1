# kan29-watch.ps1 — record every consent.exe sighting while the escalate test runs.
# Runs as SYSTEM via a scheduled task so it is independent of the SSH session, and samples
# fast enough (200 ms) to catch a prompt that a person would have had to click.
param([int]$Seconds = 180)

$log = Join-Path $env:SystemDrive 'Users\pi-user\kan29-consent.log'
Set-Content -LiteralPath $log -Value ("[{0}] watcher start, {1}s" -f (Get-Date -Format 'HH:mm:ss.fff'), $Seconds)

$deadline = (Get-Date).AddSeconds($Seconds)
$seen = 0
while ((Get-Date) -lt $deadline) {
    $consent = Get-Process -Name consent -ErrorAction SilentlyContinue
    if ($consent) {
        foreach ($c in $consent) {
            Add-Content -LiteralPath $log -Value ("[{0}] CONSENT pid={1} session={2}" -f (Get-Date -Format 'HH:mm:ss.fff'), $c.Id, $c.SessionId)
            $seen++
        }
    }
    Start-Sleep -Milliseconds 200
}
Add-Content -LiteralPath $log -Value ("[{0}] watcher end, sightings={1}" -f (Get-Date -Format 'HH:mm:ss.fff'), $seen)
