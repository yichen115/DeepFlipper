"""Applio 3.7.1 无界面训练：预处理、提取、训练、索引、部署。

先运行 make_voice_dataset.py 和 setup_windows.ps1。Applio 源码默认在
../.tools/Applio，使用其独立 .venv，避免训练与推理依赖互相覆盖。
中断后重跑相同命令会从训练检查点继续；--prepare-only 只处理数据。
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import wave

HERE = Path(__file__).resolve().parent


def exported_models(directory, name):
    result = []
    for path in directory.glob(f"{name}_*e_*s.pth"):
        match = re.fullmatch(re.escape(name) + r"_(\d+)e_(\d+)s\.pth", path.name)
        if match and path.stat().st_size > 1_000_000:
            result.append((int(match[1]), int(match[2]), path))
    return sorted(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--applio", type=Path, default=HERE.parent / ".tools" / "Applio")
    parser.add_argument("--python", type=Path, help="Applio 独立环境的 Python")
    parser.add_argument("--dataset", type=Path, default=HERE / "dataset" / "clever")
    parser.add_argument("--name", default="whale")
    parser.add_argument("--epochs", type=int, default=300)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--cpu-cores", type=int, default=4)
    parser.add_argument("--cache-gpu", action="store_true", help="把训练片段缓存在显存，加快后续轮次；显存不足时去掉")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--skip-prepare", action="store_true", help="数据未变、只继续训练时使用")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_-]+", args.name):
        parser.error("模型名称只接受英文字母、数字、下划线和短横线")
    if min(args.epochs, args.batch_size, args.cpu_cores) < 1:
        parser.error("轮数、批大小和进程数必须大于零")
    applio = args.applio.resolve()
    python = (args.python or applio / ".venv" / "Scripts" / "python.exe").resolve()
    dataset = args.dataset.resolve()
    if not python.is_file() or not (applio / "rvc/train/train.py").is_file():
        raise SystemExit("未找到 Applio 及其独立环境，见 README 的自动训练安装步骤")
    wavs = sorted(dataset.glob("*.wav"))
    seconds = 0
    for path in wavs:
        with wave.open(str(path)) as audio:
            if audio.getnchannels() != 1 or audio.getframerate() != 40000:
                raise SystemExit(f"训练素材需要 40 kHz 单声道：{path.name}")
            seconds += audio.getnframes() / audio.getframerate()
    if seconds < 60:
        raise SystemExit("训练素材不足一分钟，请先完成数据集生成")
    logs = HERE / "training"
    experiment = logs / args.name
    experiment.mkdir(parents=True, exist_ok=True)
    profile = logs / "applio-config.json"
    if not profile.exists():
        profile.write_text(json.dumps({"precision": "fp16", "discord_presence": False}), encoding="utf-8")
    env = dict(os.environ, APPLIO_LOGS_DIR=str(logs), APPLIO_CONFIG_FILE=str(profile),
               PYTHONUTF8="1", PYTHONUNBUFFERED="1", OMP_NUM_THREADS=str(args.cpu_cores))
    env["PYTHONPATH"] = str(applio) + os.pathsep + env.get("PYTHONPATH", "")
    local_ffmpeg = HERE.parent / ".tools" / "ffmpeg"
    env["PATH"] = str(local_ffmpeg) + os.pathsep + env.get("PATH", "")
    # Share precisely the ContentVec/RMVPE weights used by the inference worker.
    for source, target in [
        (HERE / "models/rmvpe.pt", applio / "rvc/models/predictors/rmvpe.pt"),
        *[(HERE / "models/hubert_base" / f, applio / "rvc/models/embedders/contentvec" / f)
          for f in ("config.json", "pytorch_model.bin")],
    ]:
        if not source.is_file():
            raise SystemExit(f"公共模型未下载：{source}")
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists():
            try:
                os.link(source, target)
            except OSError:
                shutil.copy2(source, target)

    status_file = experiment / "deepflipper-status.json"

    def status(stage, **extra):
        data = {"stage": stage, "dataset": str(dataset), "clips": len(wavs),
                "minutes": round(seconds / 60, 2), "epochs": args.epochs,
                "updated": time.strftime("%Y-%m-%d %H:%M:%S"), **extra}
        temporary = status_file.with_suffix(".tmp")
        temporary.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(status_file)

    def run(stage, script, *arguments):
        status(stage)
        print(f"\n=== {stage} ===", flush=True)
        command = [str(python), str(applio / script), *map(str, arguments)]
        with (experiment / "deepflipper.log").open("a", encoding="utf-8") as log:
            log.write(f"\n=== {time.strftime('%Y-%m-%d %H:%M:%S')} {stage} ===\n")
            with subprocess.Popen(command, cwd=applio, env=env, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                  errors="replace", bufsize=1) as process:
                for line in process.stdout:
                    log.write(line)
                    log.flush()
                    if "%|" not in line or "100%|" in line:
                        print(line, end="", flush=True)
                if process.wait():
                    raise subprocess.CalledProcessError(process.returncode, command)

    try:
        if not args.skip_prepare:
            run("preprocess", "rvc/train/preprocess/preprocess.py", experiment, dataset,
                40000, args.cpu_cores, "Automatic", False, 3.0, 0.3, "none")
            run("extract", "rvc/train/extract/extract.py", experiment, "rmvpe",
                args.cpu_cores, "0", 40000, "contentvec", "None", 2)
        filelist = experiment / "filelist.txt"
        if not filelist.exists() or not filelist.read_text(encoding="utf-8").strip():
            raise RuntimeError("特征提取没有产生可训练的样本")
        for audio in (experiment / "sliced_audios").glob("*.wav"):
            required = [experiment / "extracted" / (audio.stem + ".npy"),
                        experiment / "f0" / (audio.name + ".npy"),
                        experiment / "f0_voiced" / (audio.name + ".npy")]
            if not all(path.is_file() and path.stat().st_size > 0 for path in required):
                raise RuntimeError(f"训练片段特征不完整：{audio.name}，请重新提取特征")
        if args.prepare_only:
            status("prepared")
            return
        run("pretrained", "rvc/lib/tools/prerequisites_download.py",
            "--vocoder", "HiFi-GAN", "--sample-rate", 40000)
        pretrained = applio / "rvc/models/pretraineds/hifi-gan"
        models = exported_models(experiment, args.name)
        if not models or models[-1][0] < args.epochs:
            run("train", "rvc/train/train.py", args.name, 25, args.epochs,
                pretrained / "f0G40k.pth", pretrained / "f0D40k.pth", "0", args.batch_size,
                40000, True, True, args.cache_gpu, False, "HiFi-GAN", False)
        models = exported_models(experiment, args.name)
        if not models or models[-1][0] < args.epochs:
            raise RuntimeError("训练未完成，未部署不完整的模型；请查看训练日志")
        run("index", "rvc/train/process/extract_index.py", experiment, "Auto")
        indices = sorted(experiment.glob("*.index"), key=lambda p: p.stat().st_mtime)
        if not indices or indices[-1].stat().st_size == 0:
            raise RuntimeError("没有生成检索索引")
        for source, name in [(models[-1][2], "whale.pth"), (indices[-1], "whale.index")]:
            target = HERE / "models" / name
            temporary = target.with_suffix(target.suffix + ".tmp")
            shutil.copy2(source, temporary)
            temporary.replace(target)
        status("done", model=str(models[-1][2]), index=str(indices[-1]))
        print("\n训练完成，已部署 singing/models/whale.pth 和 whale.index。", flush=True)
    except Exception as error:
        status("failed", error=str(error))
        raise


if __name__ == "__main__":
    main()
