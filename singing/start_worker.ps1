# Start the DeepFlipper singing worker (http://0.0.0.0:8790).
#   powershell -ExecutionPolicy Bypass -File singing\start_worker.ps1
#   -InstallAutostart  register a Scheduled Task that starts the worker at logon (hidden window)
#   -RemoveAutostart   remove that task
#   -Background        start now in a hidden window (no scheduled task)
param([switch]$InstallAutostart, [switch]$RemoveAutostart, [switch]$Background)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$localAudioTools = Join-Path (Split-Path -Parent $here) '.tools\ffmpeg'
if (Test-Path -LiteralPath $localAudioTools) { $env:Path = $localAudioTools + ';' + $env:Path }
$env:PYTHONUTF8 = '1'
$py = Join-Path $here 'venv\Scripts\python.exe'
$taskName = 'DeepFlipperSingingWorker'

if ($RemoveAutostart) {
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "Removed scheduled task $taskName"
    return
}
if (-not (Test-Path $py)) { throw 'venv missing: run singing\setup_windows.ps1 first' }

if ($InstallAutostart) {
    # Hidden PowerShell console: child processes (ffmpeg) inherit it, so no windows pop up.
    $log = Join-Path $here 'worker.log'
    $cmd = "& '$py' worker.py --log '$log'"
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -WorkingDirectory $here -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -Command `"$cmd`""
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Description 'DeepFlipper RVC singing worker' -Force | Out-Null
    Write-Host "Registered $taskName (starts at logon, log: $log). Start now: Start-ScheduledTask $taskName"
    return
}

if ($Background) {
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath + '"'
    Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -WorkingDirectory $here -WindowStyle Hidden | Out-Null
    Write-Host 'Singing worker starting in background. Log: singing\worker.log'
    return
}

Set-Location $here
& $py worker.py --log (Join-Path $here 'worker.log')
