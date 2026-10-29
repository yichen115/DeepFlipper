param([switch]$Package)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$firmwareRoot = Join-Path $projectRoot 'firmware/flipper'
Push-Location $projectRoot
try {
    python scripts/fetch_dependencies.py
    if($LASTEXITCODE -ne 0) { throw 'Flipper dependency download/check failed' }
    python scripts/sync_protocol.py
    if($LASTEXITCODE -ne 0) { throw 'Protocol synchronization failed' }
    python scripts/export_flipper_assets.py
    if($LASTEXITCODE -ne 0) { throw 'Asset export failed' }
    Push-Location $firmwareRoot
    try {
        $env:FBT_NO_SYNC = '1'
        $target = if($Package) { 'updater_package' } else { 'firmware_all' }
        & .\fbt.cmd $target DEBUG=0 -j8
        if($LASTEXITCODE -ne 0) { throw "Flipper build failed: $target" }
        $release = Join-Path $projectRoot 'build/release/flipper'
        New-Item -ItemType Directory -Force $release | Out-Null
        foreach($extension in @('bin','dfu','elf')) {
            Copy-Item -LiteralPath "build/f7-firmware-C/firmware.$extension" -Destination $release
        }
        Copy-Item -LiteralPath 'applications/services/deepflipper/resources/deepflipper' -Destination $release -Recurse -Force
        if($Package) {
            Copy-Item -LiteralPath 'dist/f7-C/f7-update-local' -Destination $release -Recurse -Force
            Get-ChildItem -Path 'dist/f7-C' -Filter '*.tgz' -File -ErrorAction SilentlyContinue |
                Copy-Item -Destination $release
        }
        Write-Output "Build complete. Review output in $release; no device flashed."
    } finally { Pop-Location }
} finally { Pop-Location }
