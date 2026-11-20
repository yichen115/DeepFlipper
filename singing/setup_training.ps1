# Install Applio 3.7.1 for headless training in its own environment.
param([string]$Python = '', [string]$Applio = '', [string]$TorchWheel = '')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $Applio) { $Applio = Join-Path $projectRoot '.tools\Applio' }
if (-not $Python) { $Python = Join-Path $PSScriptRoot 'venv\Scripts\python.exe' }
if (-not (Test-Path -LiteralPath $Python)) { throw 'Run singing\setup_windows.ps1 first.' }
& $Python -c "import sys; assert sys.version_info[:2] == (3, 11), 'Headless training requires Python 3.11'"
if ($LASTEXITCODE -ne 0) { throw 'Use a Python 3.11 environment for training.' }
if (-not (Test-Path -LiteralPath (Join-Path $Applio 'rvc\train\train.py'))) {
    & git clone --depth 1 --branch 3.7.1 https://github.com/IAHispano/Applio.git $Applio
    if ($LASTEXITCODE -ne 0) { throw 'Applio source download failed.' }
}
& $Python -m pip install uv
if ($LASTEXITCODE -ne 0) { throw 'uv install failed.' }
$trainPy = Join-Path $Applio '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $trainPy)) {
    & $Python -m uv venv (Join-Path $Applio '.venv') --python $Python
    if ($LASTEXITCODE -ne 0) { throw 'Training environment creation failed.' }
}
if ($TorchWheel) {
    & $Python -m uv pip install --python $trainPy $TorchWheel
    if ($LASTEXITCODE -ne 0) { throw 'Local PyTorch install failed.' }
}
& $Python -m uv pip install --python $trainPy torch==2.11.0 torchaudio==2.11.0 --index-url https://download.pytorch.org/whl/cu128
if ($LASTEXITCODE -ne 0) { throw 'CUDA PyTorch install failed.' }
& $Python -m uv pip install --python $trainPy -r (Join-Path $PSScriptRoot 'requirements-training.txt')
if ($LASTEXITCODE -ne 0) { throw 'Training dependencies install failed.' }
& $trainPy -c "import torch, librosa, faiss, transformers; assert torch.cuda.is_available(), 'CUDA unavailable'; print('Training GPU:', torch.cuda.get_device_name(0))"
if ($LASTEXITCODE -ne 0) { throw 'Training GPU check failed.' }
Write-Host 'Ready. Run singing\venv\Scripts\python singing\train_voice.py --epochs 300'
