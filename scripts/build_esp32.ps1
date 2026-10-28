param(
    [string]$EimPath = '',
    [string]$Idf = 'v6.1'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if(-not $EimPath) {
    $workspaceEim = Join-Path (Split-Path -Parent $projectRoot) 'eim-cli.exe'
    if(Test-Path -LiteralPath $workspaceEim) { $EimPath = $workspaceEim }
    else {
        $command = Get-Command eim -ErrorAction SilentlyContinue
        if($command) { $EimPath = $command.Source }
    }
}
if(-not $EimPath -or -not (Test-Path -LiteralPath $EimPath)) {
    throw 'Pass -EimPath pointing to the installed EIM CLI executable.'
}
$previousUtf8 = $env:PYTHONUTF8
Push-Location (Join-Path $projectRoot 'firmware/esp32')
try {
    $env:PYTHONUTF8 = '1'
    $nonce = [Guid]::NewGuid().ToString('N')
    $entry = "python ../../scripts/esp32_build_entry.py --result ../../build/esp32-build-result.json --nonce $nonce"
    & $EimPath --do-not-track true run $entry $Idf
    if($LASTEXITCODE -ne 0) { throw 'ESP32 build failed in the selected EIM environment.' }
    $resultPath = Join-Path $projectRoot 'build/esp32-build-result.json'
    if(-not (Test-Path -LiteralPath $resultPath)) { throw 'EIM did not produce a build result.' }
    $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
    if($result.nonce -ne $nonce -or $result.state -ne 'complete' -or $result.exit_code -ne 0) {
        throw 'The build inside EIM failed; refusing to export stale firmware.'
    }
    $mergedHash = (Get-FileHash -LiteralPath 'build/merged-binary.bin' -Algorithm SHA256).Hash
    if($mergedHash.ToLowerInvariant() -ne $result.merged_sha256) { throw 'Build result does not match firmware.' }
    $release = Join-Path $projectRoot 'build/release/esp32'
    New-Item -ItemType Directory -Force $release | Out-Null
    foreach($name in @('xiaozhi.bin','xiaozhi.elf','merged-binary.bin','flasher_args.json')) {
        $file = Join-Path 'build' $name
        if(-not (Test-Path -LiteralPath $file)) { throw "Expected build output missing: $file" }
        Copy-Item -LiteralPath $file -Destination $release
    }
    $buildRoot = (Resolve-Path -LiteralPath 'build').Path
    $flash = Get-Content -LiteralPath 'build/flasher_args.json' -Raw | ConvertFrom-Json
    foreach($entry in $flash.flash_files.PSObject.Properties) {
        $relative = [string]$entry.Value
        if([System.IO.Path]::IsPathRooted($relative)) { throw 'Nonportable flash file path in manifest' }
        $source = [System.IO.Path]::GetFullPath((Join-Path $buildRoot $relative))
        if(-not $source.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar,
                                  [System.StringComparison]::OrdinalIgnoreCase)) {
            throw 'Flash file is outside the selected build directory'
        }
        $destination = Join-Path $release $relative
        New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination
    }
    Write-Output "ESP32 build exported to $release; no hardware flashed."
} finally {
    $env:PYTHONUTF8 = $previousUtf8
    Pop-Location
}
