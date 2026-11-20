# DeepFlipper singing worker setup (Windows + NVIDIA GPU, e.g. RTX 4060 8 GB).
#   powershell -ExecutionPolicy Bypass -File singing\setup_windows.ps1
#   options: -HfMirror (use https://hf-mirror.com)  -SkipModels  -Python "C:\path\python.exe"
# Creates singing\venv, installs PyTorch (CUDA 12.8) + audio-separator + infer-rvc-python,
# downloads public models, creates the shared token and checks the GPU.
param(
    [string]$Python = '',
    [switch]$HfMirror,
    [switch]$SkipModels
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$projectRoot = Split-Path -Parent $here
$venv = Join-Path $here 'venv'
$py = Join-Path $venv 'Scripts\python.exe'

function Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }

Step 'GPU driver'
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if (-not $smi) { throw 'nvidia-smi not found: install the NVIDIA driver first (570 or newer for CUDA 12.8).' }
$gpu = (& nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader | Select-Object -First 1)
Write-Host "GPU: $gpu"
$driver = [double](($gpu -split ',')[1].Trim().Split('.')[0])
if ($driver -lt 570) { Write-Warning "Driver $driver is older than 570; CUDA 12.8 wheels need 570+. Update the NVIDIA driver." }

Step 'ffmpeg'
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
    if (Get-Command winget -ErrorAction SilentlyContinue) {
        Write-Host 'Installing ffmpeg with winget (Gyan.FFmpeg)...'
        winget install --id Gyan.FFmpeg -e --accept-source-agreements --accept-package-agreements
        $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
    }
    if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
        throw 'ffmpeg not found. Install it (winget install Gyan.FFmpeg  or  choco install ffmpeg), reopen PowerShell, run again.'
    }
}
Write-Host (& ffmpeg -hide_banner -version | Select-Object -First 1)

Step 'Python venv'
if (-not (Test-Path $py)) {
    if (-not $Python -and (Get-Command py -ErrorAction SilentlyContinue)) {
        $ErrorActionPreference = 'Continue'   # py prints to stderr when a version is missing
        foreach ($v in @('3.11')) {
            & py "-$v" -c "import sys" *> $null
            if ($LASTEXITCODE -eq 0) { $Python = "py -$v"; break }
        }
        $ErrorActionPreference = 'Stop'
    }
    if (-not $Python -and (Get-Command python -ErrorAction SilentlyContinue)) {
        $candidate = & python -c "import sys; print(sys.executable if sys.version_info[:2] == (3, 11) else '')"
        if ($LASTEXITCODE -eq 0 -and $candidate) { $Python = $candidate.Trim() }
    }
    if (-not $Python) { throw 'Python 3.11 not found: winget install Python.Python.3.11' }
    Write-Host "Creating venv with $Python"
    if ($Python.StartsWith('py ')) { & py $Python.Substring(3) -m venv $venv } else { & $Python -m venv $venv }
    if ($LASTEXITCODE -ne 0) { throw 'venv creation failed' }
}
& $py -m pip install --upgrade "pip>=24" wheel
if ($LASTEXITCODE -ne 0) { throw 'pip upgrade failed' }

Step 'Python GPU runtime'
& $py -c "import sys; assert sys.version_info[:2] == (3, 11), 'Python 3.11 required'"
if ($LASTEXITCODE -ne 0) { throw 'Create the singing environment with Python 3.11.' }
& $py -m pip install uv
if ($LASTEXITCODE -ne 0) { throw 'uv install failed' }
# Older Conda bundles an old VC runtime that prevents c10.dll from loading.
# Switch only this project's venv to standalone Python; installed audio packages stay in place.
$needsRuntime = & $py -c "import sys; from pathlib import Path; print(int(sys.version_info[:3] < (3, 11, 9) or (Path(sys.base_prefix) / 'conda-meta').is_dir()))"
if ($needsRuntime.Trim() -eq '1') {
    $runtimeDir = Join-Path $projectRoot '.tools\python'
    $previousInstallDir = $env:UV_PYTHON_INSTALL_DIR
    try {
        $env:UV_PYTHON_INSTALL_DIR = $runtimeDir
        & $py -m uv python install 3.11 --install-dir $runtimeDir
        if ($LASTEXITCODE -ne 0) { throw 'Standalone Python download failed.' }
        $managedPy = (& $py -m uv python find --managed-python 3.11).Trim()
        if ($LASTEXITCODE -ne 0) { throw 'Standalone Python not found.' }
        $switchRuntime = @'
import re, shutil, sys
from pathlib import Path
cfg = Path(sys.argv[1]) / 'pyvenv.cfg'
backup = cfg.with_name('pyvenv.cfg.before-runtime-switch')
if not backup.exists(): shutil.copy2(cfg, backup)
text = cfg.read_text()
text = re.sub(r'^home\s*=.*$', lambda m: 'home = ' + sys.base_prefix, text, flags=re.M)
text = re.sub(r'^executable\s*=.*$', lambda m: 'executable = ' + sys.executable, text, flags=re.M)
text = re.sub(r'^(version|version_info)\s*=.*$', lambda m: m[1] + ' = ' + '.'.join(map(str, sys.version_info[:3])), text, flags=re.M)
cfg.write_text(text)
'@
        & $managedPy -c $switchRuntime $venv
        if ($LASTEXITCODE -ne 0) { throw 'Local runtime switch failed.' }
    } finally { $env:UV_PYTHON_INSTALL_DIR = $previousInstallDir }
}

Step 'PyTorch 2.11 + CUDA 12.8'
& $py -m pip install torch==2.11.0 torchvision==0.26.0 torchaudio==2.11.0 --index-url https://download.pytorch.org/whl/cu128
if ($LASTEXITCODE -ne 0) { throw 'PyTorch install failed' }

Step 'audio-separator + infer-rvc-python'
& $py -m uv pip install --python $py --override (Join-Path $here 'dependency-overrides.txt') -r (Join-Path $here 'requirements.txt')
if ($LASTEXITCODE -ne 0) { throw 'requirements install failed' }
& $py -c "import torch, numpy, pyworld, faiss, audio_separator, infer_rvc_python; print('torch', torch.__version__, 'cuda', torch.cuda.is_available(), 'numpy', numpy.__version__)"
if ($LASTEXITCODE -ne 0) { throw 'import check failed (see error above)' }

if (-not $SkipModels) {
    Step 'Public models (rmvpe, hubert, BS-Roformer)'
    if ($HfMirror) { $env:HF_ENDPOINT = 'https://hf-mirror.com' }
    Push-Location $here
    try {
        & $py fetch_models.py
        if ($LASTEXITCODE -ne 0) { throw 'model download failed (try -HfMirror, or rerun later)' }
    } finally { Pop-Location }
}

Step 'Shared token'
foreach ($d in @('songs_in', 'models', 'cache')) { New-Item -ItemType Directory -Force (Join-Path $here $d) | Out-Null }
$tokenFile = Join-Path $here 'token.txt'
if (-not (Test-Path $tokenFile)) {
    $bytes = New-Object byte[] 24
    [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
    $token = [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')
    [IO.File]::WriteAllText($tokenFile, $token)
    Write-Host "Created $tokenFile"
}
$token = (Get-Content $tokenFile -Raw).Trim()
$serverEnv = Join-Path $projectRoot 'server\main\xiaozhi-server\data\server.env'
if (Test-Path $serverEnv) {
    $hasToken = Select-String -Path $serverEnv -Pattern '^\s*SINGING_TOKEN\s*=' -Quiet
    if (-not $hasToken) {
        Add-Content -Path $serverEnv -Value "`nSINGING_TOKEN=$token"
        Write-Host 'Added SINGING_TOKEN to server\main\xiaozhi-server\data\server.env'
        Write-Host 'Recreate the voice server: docker compose -f server/compose.local.yaml up -d'
    } else {
        Write-Host 'server.env already has SINGING_TOKEN (make sure it equals singing\token.txt)'
    }
} else {
    Write-Warning "Not found: $serverEnv. Add SINGING_TOKEN=<content of singing\token.txt> to the voice server env."
}

Step 'Check'
& $py (Join-Path $here 'worker.py') --check
Write-Host "`nDone. Next: put your RVC model at singing\models\whale.pth (+ whale.index), songs in singing\songs_in,"
Write-Host 'then start the worker:  powershell -ExecutionPolicy Bypass -File singing\start_worker.ps1'
