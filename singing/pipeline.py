"""翻唱渲染流水线：截取片段 → 人声分离 → 自动音高 → RVC 变声 → ffmpeg 混音。

参数参考 MIT 许可的 aureth-labs/dsh-aurabot-qq（src/song/studio.js）与
Dhao007/dsh-singing-studio：120 s 内片段、从 30% 处开始、BS-Roformer 分离、
rmvpe、index_rate 0.5、protect 0.33、伴奏 0.8、淡入 1.5 s / 淡出 3 s、按八度移调。

重依赖（torch、audio-separator、infer-rvc-python）都在函数里延迟导入，
这样 worker 的排队/缓存逻辑可以在没有 GPU 的环境里测试。
"""
import gc
import json
import logging
import math
import os
import re
import shutil
import subprocess
import time

log = logging.getLogger("singing.pipeline")


class RenderError(Exception):
    """给用户看的失败原因（中文短句）。"""


def _run(cmd, timeout=600):
    proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=timeout)
    if proc.returncode != 0:
        tail = proc.stderr.decode("utf-8", "replace")[-600:]
        raise RenderError(f"ffmpeg 失败：{tail.strip().splitlines()[-1] if tail.strip() else proc.returncode}")
    return proc


def probe_duration(path):
    """歌曲时长（秒）。优先 ffprobe，没有就解析 ffmpeg -i 的输出。"""
    if shutil.which("ffprobe"):
        out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                              "-of", "default=nw=1:nk=1", path], stdin=subprocess.DEVNULL, capture_output=True,
                             timeout=60)
        try:
            return float(out.stdout.decode().strip())
        except ValueError:
            pass
    out = subprocess.run(["ffmpeg", "-hide_banner", "-nostdin", "-i", path], stdin=subprocess.DEVNULL,
                         capture_output=True, timeout=60)
    m = re.search(r"Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)", out.stderr.decode("utf-8", "replace"))
    if not m:
        raise RenderError("读不出歌曲时长，文件可能损坏")
    h, mi, s = m.groups()
    return int(h) * 3600 + int(mi) * 60 + float(s)


def clip_window(duration, clip_seconds, start_ratio):
    """返回 (start, length)。clip_seconds<=0 或歌曲更短时取整首。"""
    if clip_seconds <= 0 or duration <= clip_seconds:
        return 0.0, duration
    start = max(0.0, min(round(duration * start_ratio), math.floor(duration - clip_seconds)))
    return float(start), float(clip_seconds)


def octave_shift(source_hz, target_hz, max_shift=12):
    """按整八度把原唱音高移到目标附近（伴奏不用变调，仍然同调）。"""
    if not source_hz or source_hz <= 0 or not target_hz:
        return 0
    shift = 12 * round(math.log2(target_hz / source_hz))
    return int(max(-max_shift, min(max_shift, shift)))


def fit_shift(low_hz, median_hz, high_hz, target_hz=330.0, min_hz=200.0, max_hz=480.0, max_shift=12):
    """选一个移调（半音），让整首歌的音域落在鲸鱼娘练过的范围里。

    音色模型只用说话素材训练，基频大多在 200–480 Hz；唱到 600 Hz 以上就会发虚、发哑。
    所以不只看中位音高，还要看高音（第 95 百分位）和低音（第 10 百分位）：
    高音超出上限扣分最重，低音低于下限其次，中位离 target 越远越扣分；
    不是整八度要给伴奏变调（音色会变一点），也小扣一点，能整八度就整八度。
    """
    if not median_hz or median_hz <= 0:
        return 0
    low_hz = low_hz if low_hz and low_hz > 0 else median_hz
    high_hz = high_hz if high_hz and high_hz > 0 else median_hz
    best, best_cost = 0, None
    for shift in range(-max_shift, max_shift + 1):
        ratio = 2 ** (shift / 12)
        over = max(0.0, 12 * math.log2(high_hz * ratio / max_hz))
        under = max(0.0, 12 * math.log2(min_hz / (low_hz * ratio)))
        centre = abs(12 * math.log2(median_hz * ratio / target_hz))
        cost = 3.0 * over + 1.5 * under + 0.35 * centre + (0 if shift % 12 == 0 else 1.0)
        if best_cost is None or cost < best_cost - 1e-9:
            best, best_cost = shift, cost
    return best


def atempo_chain(ratio):
    """atempo 单级只接受 0.5–2.0。"""
    parts = []
    while ratio > 2.0:
        parts.append("atempo=2.0")
        ratio /= 2.0
    while ratio < 0.5:
        parts.append("atempo=0.5")
        ratio /= 0.5
    parts.append(f"atempo={ratio:.6f}")
    return ",".join(parts)


def mix_filter(length, mix, inst_semitones=0, sample_rate_in=44100):
    """ffmpeg filter_complex：人声 + 伴奏 → 响度 → 淡入淡出 → 24 kHz 单声道。"""
    inst = ""
    if inst_semitones:
        # 非整八度的手动移调：伴奏也移相同的余数半音，保持同调。
        # 顺序必须是 asetrate → atempo → aresample（先 aresample 再 atempo 在 ffmpeg 4.4 上会卡住）
        r = 2 ** (inst_semitones / 12)
        inst = f"asetrate={int(sample_rate_in * r)},{atempo_chain(1 / r)},aresample={sample_rate_in},"
    chain = [f"[0:a]aresample={sample_rate_in},volume={mix['vocal_volume']}[v]",
             f"[1:a]{inst}volume={mix['instrumental_volume']}[i]"]
    post = "[v][i]amix=inputs=2:duration=longest:normalize=0"
    if mix.get("loudnorm", True):
        post += ",loudnorm=I=-16:TP=-1.5:LRA=11"
    else:
        post += ",alimiter=limit=0.95"
    fade_in, fade_out = float(mix.get("fade_in", 1.5)), float(mix.get("fade_out", 3.0))
    if fade_in > 0:
        post += f",afade=t=in:d={fade_in}"
    if fade_out > 0 and length > fade_out + fade_in:
        post += f",afade=t=out:st={length - fade_out:.2f}:d={fade_out}"
    post += f",aresample={int(mix.get('sample_rate', 24000))}[out]"
    return ";".join(chain + [post])


class Pipeline:
    """真实 GPU 流水线。render() 在 worker 的后台线程里调用，一次只跑一首。"""

    def __init__(self, config, base_dir):
        self.cfg = config
        self.base = base_dir
        self._gpu = None
        local_tools = os.path.abspath(os.path.join(base_dir, "..", ".tools", "ffmpeg"))
        if not shutil.which("ffmpeg") and os.path.isfile(os.path.join(local_tools, "ffmpeg.exe" if os.name == "nt" else "ffmpeg")):
            os.environ["PATH"] = local_tools + os.pathsep + os.environ.get("PATH", "")

    def path(self, value):
        if not value:
            return ""
        return value if os.path.isabs(value) else os.path.join(self.base, value)

    # ------------------------------------------------------------ 状态
    def gpu_info(self):
        if self._gpu is None:
            try:
                import torch
                if torch.cuda.is_available():
                    free, total = torch.cuda.mem_get_info()
                    self._gpu = {"cuda": True, "name": torch.cuda.get_device_name(0),
                                 "vram_gb": round(total / 2**30, 1), "torch": torch.__version__}
                else:
                    self._gpu = {"cuda": False, "name": "CPU（未检测到 CUDA）", "torch": torch.__version__}
            except Exception as error:  # torch 未安装
                self._gpu = {"cuda": False, "name": f"torch 不可用：{type(error).__name__}"}
        return self._gpu

    def model_status(self):
        rvc = self.cfg["rvc"]
        model = self.path(rvc.get("model"))
        index = self.path(rvc.get("index"))
        return {"model": os.path.basename(model), "model_ready": os.path.isfile(model),
                "index": os.path.basename(index) if index else "", "index_ready": bool(index) and os.path.isfile(index)}

    def model_id(self):
        """缓存键的一部分：换了 .pth / .index 就重新渲染。"""
        parts = []
        for key in ("model", "index"):
            p = self.path(self.cfg["rvc"].get(key))
            if p and os.path.isfile(p):
                st = os.stat(p)
                parts.append(f"{os.path.basename(p)}:{st.st_size}:{int(st.st_mtime)}")
        rvc = self.cfg["rvc"]
        parts.append(f"{rvc.get('index_rate')}:{rvc.get('protect')}:{rvc.get('rms_mix_rate')}:{rvc.get('target_f0_hz')}")
        # 音域适配方式改了，自动移调的结果会不同
        mode = str(rvc.get("pitch_mode", "fit")).lower()
        if mode != "octave":
            parts.append(f"fit:{rvc.get('min_f0_hz', 200)}:{rvc.get('max_f0_hz', 480)}")
        return "|".join(parts)

    # ------------------------------------------------------------ 流程
    def render(self, src, out_wav, pitch, clip_seconds, start_ratio, workdir, progress):
        """pitch: 'auto' 或整数半音。返回元数据（实际移调、片段位置、各阶段耗时）。"""
        if not self.model_status()["model_ready"]:
            raise RenderError("还没有 RVC 音色模型：把 .pth 放到 singing/models/ 并在 config.yaml 里填路径")
        timings = {}
        os.makedirs(workdir, exist_ok=True)

        def stage(name, fraction, fn, *args):
            progress(name, fraction)
            t0 = time.monotonic()
            result = fn(*args)
            timings[name] = round(time.monotonic() - t0, 1)
            log.info("阶段 %s 用时 %.1fs", name, timings[name])
            return result

        duration = probe_duration(src)
        start, length = clip_window(duration, clip_seconds, start_ratio)
        clip = os.path.join(workdir, "clip.wav")
        stage("cut", 0.02, self.cut, src, clip, start, length)
        vocals, inst = stage("separate", 0.08, self.separate, clip, workdir)
        level = self.vocal_level_db(vocals)
        if level < float(self.cfg["mix"].get("min_vocal_db", -50)):
            raise RenderError("这段几乎没有人声，像是纯音乐，换一首吧")
        rvc = self.cfg["rvc"]
        source_hz = 0.0
        stats = None
        if str(pitch).lower() == "auto":
            stats = stage("pitch", 0.45, self.f0_stats, vocals)
            source_hz = stats[1]
            if str(rvc.get("pitch_mode", "fit")).lower() == "octave":
                applied = octave_shift(source_hz, float(rvc.get("target_f0_hz", 330)), int(rvc.get("max_auto_shift", 12)))
            else:
                applied = fit_shift(*stats, target_hz=float(rvc.get("target_f0_hz", 330)),
                                    min_hz=float(rvc.get("min_f0_hz", 200)), max_hz=float(rvc.get("max_f0_hz", 480)),
                                    max_shift=int(rvc.get("max_auto_shift", 12)))
        else:
            applied = int(pitch)
        converted = os.path.join(workdir, "converted.wav")
        stage("rvc", 0.5, self.convert, vocals, converted, applied)
        residual = applied - 12 * round(applied / 12)
        stage("mix", 0.92, self.mix, converted, inst, out_wav, length, residual)
        progress("done", 1.0)
        return {"applied_pitch": applied, "source_f0_hz": round(source_hz, 1),
                "source_f0_range_hz": [round(stats[0], 1), round(stats[2], 1)] if stats else None, "start": start,
                "length": round(length, 1), "song_duration": round(duration, 1), "vocal_db": round(level, 1),
                "timings": timings}

    def cut(self, src, dst, start, length):
        _run(["ffmpeg", "-y", "-hide_banner", "-nostdin", "-ss", f"{start:.2f}", "-t", f"{length:.2f}",
              "-i", src, "-vn", "-ac", "2", "-ar", "44100", "-c:a", "pcm_s16le", dst])

    def separate(self, clip, workdir):
        import torch
        from audio_separator.separator import Separator
        sep_cfg = self.cfg["separator"]
        out_dir = os.path.join(workdir, "stems")
        os.makedirs(out_dir, exist_ok=True)
        params = {"segment_size": 256, "override_model_segment_size": False, "batch_size": 1,
                  "overlap": sep_cfg.get("overlap"), "pitch_shift": 0}
        separator = Separator(output_dir=out_dir, output_format="WAV",
                              model_file_dir=self.path(sep_cfg.get("model_dir", "models/separator")),
                              use_autocast=bool(sep_cfg.get("use_autocast", True)), use_soundfile=True,
                              mdxc_params=params)
        try:
            separator.load_model(model_filename=sep_cfg["model"])
            outputs = separator.separate(clip, {"Vocals": "vocals", "Instrumental": "instrumental",
                                                "Other": "instrumental"})
        finally:
            del separator
            gc.collect()
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
        paths = [p if os.path.isabs(p) else os.path.join(out_dir, p) for p in outputs]
        vocals = next((p for p in paths if "vocals" in os.path.basename(p).lower()), None)
        inst = next((p for p in paths if p != vocals), None)
        if not vocals or not inst:
            raise RenderError(f"人声分离没有得到两个音轨：{[os.path.basename(p) for p in paths]}")
        return vocals, inst

    @staticmethod
    def vocal_level_db(path):
        import numpy as np
        import soundfile as sf
        data, _ = sf.read(path, dtype="float32", always_2d=True)
        rms = float(np.sqrt(np.mean(np.square(data)))) if data.size else 0.0
        return 20 * math.log10(rms) if rms > 0 else -120.0

    @staticmethod
    def median_f0(path):
        """原唱人声的中位基频（Hz）。"""
        return Pipeline.f0_stats(path)[1]

    @staticmethod
    def f0_stats(path):
        """原唱人声基频的 (第 10 百分位, 中位, 第 95 百分位)，Hz。用 pyworld.dio，90 秒几秒内完成。"""
        import numpy as np
        import soundfile as sf
        import pyworld
        data, sr = sf.read(path, dtype="float64", always_2d=True)
        mono = np.ascontiguousarray(data.mean(axis=1))
        f0, _ = pyworld.dio(mono, sr, f0_floor=65.0, f0_ceil=1100.0, frame_period=10.0)
        voiced = f0[f0 > 0]
        if voiced.size <= 50:
            return 0.0, 0.0, 0.0
        low, mid, high = np.percentile(voiced, [10, 50, 95])
        return float(low), float(mid), float(high)

    def convert(self, vocals, dst, semitones):
        import soundfile as sf
        import torch
        from infer_rvc_python import BaseLoader
        rvc = self.cfg["rvc"]
        hubert = self.path(rvc.get("hubert")) or None
        if hubert and not os.path.exists(hubert):
            hubert = None
        loader = BaseLoader(only_cpu=not torch.cuda.is_available(), hubert_path=hubert,
                            rmvpe_path=self.path(rvc.get("rmvpe")))
        try:
            loader.apply_conf(tag="voice", file_model=self.path(rvc["model"]),
                              pitch_algo=rvc.get("f0_method", "rmvpe"), pitch_lvl=int(semitones),
                              file_index=self.path(rvc.get("index")) or "",
                              index_influence=float(rvc.get("index_rate", 0.5)),
                              respiration_median_filtering=int(rvc.get("filter_radius", 3)),
                              envelope_ratio=float(rvc.get("rms_mix_rate", 0.25)),
                              consonant_breath_protection=float(rvc.get("protect", 0.33)))
            audio, sr = loader.generate_from_cache(audio_data=vocals, tag="voice")
            sf.write(dst, audio, sr)
        finally:
            loader.unload_models()
            del loader
            gc.collect()
            if torch.cuda.is_available():
                torch.cuda.empty_cache()

    def mix(self, vocals, inst, dst, length, inst_semitones):
        tmp = dst + ".part.wav"
        _run(["ffmpeg", "-y", "-hide_banner", "-nostdin", "-i", vocals, "-i", inst,
              "-filter_complex", mix_filter(length, self.cfg["mix"], inst_semitones), "-map", "[out]",
              "-ac", "1", "-c:a", "pcm_s16le", tmp])
        os.replace(tmp, dst)


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)
