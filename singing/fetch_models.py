"""下载唱歌服务需要的公共模型（setup_windows.ps1 会调用；可重复运行，已存在的跳过）。

    venv\\Scripts\\python fetch_models.py            # 国内网络可先设置 $env:HF_ENDPOINT="https://hf-mirror.com"

* models/rmvpe.pt          RMVPE 音高模型（lj1995/VoiceConversionWebUI）
* models/hubert_base/      ContentVec 特征模型（和 Applio 训练使用相同权重）
* models/separator/…       人声分离模型（config.yaml 的 separator.model，从 GitHub 下载）
"""
import os
import sys
import json
import time
import urllib.request

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))


def hf_url(repo, name):
    base = os.environ.get("HF_ENDPOINT", "https://huggingface.co").rstrip("/")
    return f"{base}/{repo}/resolve/main/{name}"


def download(url, dst):
    if os.path.isfile(dst) and (os.path.getsize(dst) > 1_000_000 or dst.endswith('.json')):
        if dst.endswith('.json'):
            with open(dst, encoding='utf-8') as stream:
                json.load(stream)
        print(f"已存在：{dst}")
        return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".part"
    print(f"下载 {url}")
    for attempt in range(6):
        offset = os.path.getsize(tmp) if os.path.isfile(tmp) else 0
        req = urllib.request.Request(url, headers={'Range': f'bytes={offset}-'} if offset else {})
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                append = resp.status == 206 and offset > 0
                if append and not resp.headers.get('Content-Range', '').startswith(f'bytes {offset}-'):
                    raise ValueError('服务器返回了错误的续传范围')
                expected = int(resp.headers.get('Content-Length') or 0)
                received = 0
                with open(tmp, 'ab' if append else 'wb') as out:
                    while block := resp.read(1 << 20):
                        out.write(block)
                        received += len(block)
                if expected and received != expected:
                    raise OSError('模型下载不完整')
            if dst.endswith('.json'):
                with open(tmp, encoding='utf-8') as stream:
                    json.load(stream)
            os.replace(tmp, dst)
            print(f'完成：{os.path.basename(dst)}', flush=True)
            return
        except (OSError, ValueError) as error:
            if (isinstance(error, ValueError) or getattr(error, 'code', None) == 416) and os.path.isfile(tmp):
                os.remove(tmp)
            if attempt == 5:
                raise
            print(f'下载重试 {attempt + 1}/5：{os.path.basename(dst)}（{type(error).__name__}）', flush=True)
            time.sleep(2 * (attempt + 1))


def path(value):
    return value if os.path.isabs(value) else os.path.join(HERE, value)


def main():
    with open(os.path.join(HERE, "config.yaml"), "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f) or {}
    rvc, sep = cfg.get("rvc", {}), cfg.get("separator", {})

    download(hf_url("lj1995/VoiceConversionWebUI", "rmvpe.pt"), path(rvc.get("rmvpe", "models/rmvpe.pt")))

    hubert = rvc.get("hubert")
    if hubert:
        print("下载 ContentVec（与 Applio 训练保持一致）…")
        for name in ("config.json", "pytorch_model.bin"):
            download(hf_url("IAHispano/Applio", "Resources/embedders/contentvec/" + name),
                     os.path.join(path(hubert), name))

    from audio_separator.separator import Separator
    model_dir = path(sep.get("model_dir", "models/separator"))
    print(f"下载人声分离模型 {sep.get('model')} …")
    Separator(model_file_dir=model_dir, output_dir=model_dir).download_model_and_data(sep["model"])
    print("模型准备完成。")


if __name__ == "__main__":
    sys.exit(main())
