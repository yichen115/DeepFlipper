"""用 BreezeBlue 云端 TTS（鲸鱼娘说话的声音）生成 RVC 训练数据集。

    venv\\Scripts\\python make_voice_dataset.py --voice clever
    venv\\Scripts\\python make_voice_dataset.py --voice dreamy --env-file ..\\server\\main\\xiaozhi-server\\data\\server.env

* API Key 读环境变量 BREEZE_API_KEY（或 --env-file 指定的 KEY=VALUE 文件），不会打印出来；
* 免费套餐同一时间只能合成一句，所以严格逐句请求，429 时按 Retry-After 等待重试；
* 每个中文字约 0.2 额度：开始前先估算并询问确认；
* 输出 dataset/<voice>/0001.wav …（默认 40 kHz 单声道 16 位，RVC 训练用），已存在的跳过，可随时中断续跑。
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BASE_URL = "https://api.breeze.blue/v1"
VOICES = {"clever": "voc_u3nxkd5578wc", "dreamy": "voc_8yud7hz7c9q2"}
CREDIT_PER_CHAR = 0.2
CHARS_PER_SECOND = 4.3      # 普通话朗读语速估计
SOURCE_RATE = 24000


def load_sentences(path):
    with open(path, "r", encoding="utf-8") as f:
        return [line.strip() for line in f if line.strip() and not line.lstrip().startswith("#")]


def read_key(env_file):
    key = os.environ.get("BREEZE_API_KEY", "").strip()
    if not key and env_file:
        with open(env_file, "r", encoding="utf-8") as f:
            for line in f:
                name, _, value = line.strip().partition("=")
                if name.strip() == "BREEZE_API_KEY":
                    key = value.strip().strip('"').strip("'")
    return key


def synthesize(key, voice_id, text, retries=5):
    """返回 24 kHz s16le 单声道 PCM。"""
    url = f"{BASE_URL}/text-to-speech/{urllib.parse.quote(voice_id)}/stream?" + urllib.parse.urlencode(
        {"output_format": "pcm", "enable_logging": "false"})
    body = json.dumps({"text": text, "language_code": "zh"}, ensure_ascii=False).encode("utf-8")
    for attempt in range(retries + 1):
        req = urllib.request.Request(url, data=body, method="POST", headers={
            "xi-api-key": key, "content-type": "application/json", "accept": "audio/pcm"})
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                pcm = resp.read()
            return pcm[: len(pcm) & ~1]
        except urllib.error.HTTPError as error:
            if error.code == 429 and attempt < retries:   # 免费套餐：上一句还没合成完
                wait = error.headers.get("Retry-After") if error.headers else None
                try:
                    wait = float(wait)
                except (TypeError, ValueError):
                    wait = 2.0 * (attempt + 1)
                time.sleep(min(30.0, max(1.0, wait)))
                continue
            hints = {401: "API Key 无效", 402: "额度已用完", 403: "无权使用该音色", 404: "音色不存在"}
            raise SystemExit(f"BreezeBlue HTTP {error.code} {hints.get(error.code, '')}") from None
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            if attempt < retries:
                time.sleep(2.0 * (attempt + 1))
                continue
            raise SystemExit(f"网络错误：{type(error).__name__}") from None
    raise SystemExit("重试次数用完")


def save_wav(pcm, path, rate):
    """24 kHz PCM → 目标采样率 WAV（ffmpeg 重采样，开头结尾各留 0.1 秒静音）。"""
    tmp = path + ".part.wav"
    cmd = ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-f", "s16le", "-ar", str(SOURCE_RATE),
           "-ac", "1", "-i", "pipe:0", "-af", "adelay=100,apad=pad_dur=0.1", "-ar", str(rate),
           "-ac", "1", "-c:a", "pcm_s16le", tmp]
    subprocess.run(cmd, input=pcm, check=True)
    os.replace(tmp, path)


def main():
    parser = argparse.ArgumentParser(description="用 BreezeBlue 声音生成 RVC 训练集")
    parser.add_argument("--voice", default="clever", help="clever / dreamy 或 voc_ 开头的音色 ID")
    parser.add_argument("--sentences", default=os.path.join(HERE, "dataset_sentences.txt"))
    parser.add_argument("--out", default=None, help="输出目录，默认 dataset/<voice>")
    parser.add_argument("--rate", type=int, default=40000, choices=(32000, 40000, 48000))
    parser.add_argument("--limit", type=int, default=0, help="只生成前 N 句（试听用）")
    parser.add_argument("--env-file", default=None, help="从 KEY=VALUE 文件读取 BREEZE_API_KEY")
    parser.add_argument("--yes", action="store_true", help="不询问，直接开始")
    args = parser.parse_args()

    voice_id = VOICES.get(args.voice, args.voice)
    if not voice_id.startswith("voc_"):
        raise SystemExit("--voice 应为 clever、dreamy 或 voc_ 开头的音色 ID")
    if not shutil.which("ffmpeg"):
        raise SystemExit("没有找到 ffmpeg：winget install Gyan.FFmpeg 后重开终端")
    out_dir = args.out or os.path.join(HERE, "dataset", args.voice)
    os.makedirs(out_dir, exist_ok=True)
    sentences = load_sentences(args.sentences)
    if args.limit:
        sentences = sentences[: args.limit]
    todo = [(i, s) for i, s in enumerate(sentences, 1) if not os.path.exists(os.path.join(out_dir, f"{i:04d}.wav"))]
    chars = sum(len(s) for _, s in todo)
    spoken = sum(len(re.sub(r"[\W_]", "", s)) for _, s in todo)
    print(f"音色 {args.voice}（{voice_id}），共 {len(sentences)} 句，待生成 {len(todo)} 句、{chars} 字")
    print(f"预计消耗约 {chars * CREDIT_PER_CHAR:.0f} 额度（每字约 {CREDIT_PER_CHAR}），"
          f"音频约 {spoken / CHARS_PER_SECOND / 60:.1f} 分钟，输出 {out_dir}（{args.rate} Hz）")
    if not todo:
        print("全部已生成。")
        return
    key = read_key(args.env_file)
    if not key:
        raise SystemExit("没有 API Key：先设置环境变量 BREEZE_API_KEY，或用 --env-file 指向 server.env")
    if not args.yes and input("确认开始？[y/N] ").strip().lower() not in ("y", "yes", "是"):
        print("已取消。")
        return

    total_audio = 0.0
    with open(os.path.join(out_dir, "metadata.csv"), "a", encoding="utf-8") as meta:
        for n, (i, text) in enumerate(todo, 1):
            started = time.monotonic()
            pcm = synthesize(key, voice_id, text)
            if len(pcm) < SOURCE_RATE // 5:
                print(f"  [{i:04d}] 没有返回音频，跳过：{text}")
                continue
            save_wav(pcm, os.path.join(out_dir, f"{i:04d}.wav"), args.rate)
            seconds = len(pcm) / 2 / SOURCE_RATE
            total_audio += seconds
            meta.write(f"{i:04d}.wav|{text}\n")
            meta.flush()
            print(f"  [{n}/{len(todo)}] {i:04d}.wav {seconds:.1f}s（{time.monotonic() - started:.1f}s）{text[:24]}")
    print(f"完成：本次新增音频 {total_audio / 60:.1f} 分钟。下一步见 README：用 Applio 训练。")


if __name__ == "__main__":
    sys.exit(main())
