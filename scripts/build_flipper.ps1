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
    python scripts/make_pet_assets.py
    if($LASTEXITCODE -ne 0) { throw 'Pet asset generation failed' }
    Push-Location $firmwareRoot
    try {
        $env:FBT_NO_SYNC = '1'
        & .\fbt.cmd fap_deepflipper_tools DEBUG=0 -j8
        if($LASTEXITCODE -ne 0) { throw 'Capture adapter FAP build failed' }
        $adapterResources='applications/services/deepflipper/resources/apps/Tools'
        New-Item -ItemType Directory -Force $adapterResources | Out-Null
        Copy-Item -LiteralPath 'build/f7-firmware-C/.extapps/deepflipper_tools.fap' -Destination $adapterResources
        & python ../../scripts/export_app_catalog.py --manifest applications/examples/deepflipper_tools/ai_app.json --fap build/f7-firmware-C/.extapps/deepflipper_tools.fap --output-dir applications/services/deepflipper/resources/deepflipper/apps
        if($LASTEXITCODE -ne 0) { throw 'App catalog registration export failed' }
        & .\fbt.cmd fap_whale_pet DEBUG=0 -j8
        if($LASTEXITCODE -ne 0) { throw 'Pet FAP build failed' }
        New-Item -ItemType Directory -Force 'applications/services/deepflipper/resources/apps/Games' | Out-Null
        Copy-Item -LiteralPath 'build/f7-firmware-C/.extapps/whale_pet.fap' -Destination 'applications/services/deepflipper/resources/apps/Games'
        & python ../../scripts/export_app_catalog.py --manifest applications/examples/whale_pet/ai_app.json --fap build/f7-firmware-C/.extapps/whale_pet.fap --output-dir applications/services/deepflipper/resources/deepflipper/apps
        if($LASTEXITCODE -ne 0) { throw 'Pet registration export failed' }
        $target = if($Package) { 'updater_package' } else { 'firmware_all' }
        & .\fbt.cmd $target DEBUG=0 -j8
        if($LASTEXITCODE -ne 0) { throw "Flipper build failed: $target" }
        $release = Join-Path $projectRoot 'build/release/flipper'
        New-Item -ItemType Directory -Force $release | Out-Null
        foreach($extension in @('bin','dfu','elf')) {
            Copy-Item -LiteralPath "build/f7-firmware-C/firmware.$extension" -Destination $release
        }
        Copy-Item -LiteralPath 'applications/services/deepflipper/resources/deepflipper' -Destination $release -Recurse -Force
        New-Item -ItemType Directory -Force (Join-Path $release 'apps/Tools') | Out-Null
        Copy-Item -LiteralPath 'build/f7-firmware-C/.extapps/deepflipper_tools.fap' -Destination (Join-Path $release 'apps/Tools')
        New-Item -ItemType Directory -Force (Join-Path $release 'apps/Games') | Out-Null
        Copy-Item -LiteralPath 'build/f7-firmware-C/.extapps/whale_pet.fap' -Destination (Join-Path $release 'apps/Games')
        & python ../../scripts/package_pet_preview.py --release-dir $release
        if($LASTEXITCODE -ne 0) { throw 'Pet SD preview export failed' }
        if($Package) {
            Copy-Item -LiteralPath 'dist/f7-C/f7-update-local' -Destination $release -Recurse -Force
            Get-ChildItem -Path 'dist/f7-C' -Filter '*.tgz' -File -ErrorAction SilentlyContinue |
                Copy-Item -Destination $release
        }
        Write-Output "Build complete. Review output in $release; no device flashed."
    } finally { Pop-Location }
} finally { Pop-Location }
