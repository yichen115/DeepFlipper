# 鲸鱼娘唱歌（RVC 翻唱）

让鲸鱼娘用她自己的声音唱你曲库里的歌。说一句“唱首晴天”，流程是这样的：

```
设备 ──语音──> 语音服务(Docker) ──HTTP──> 唱歌渲染服务 singing/worker.py（Windows 本机，RTX 4060）
                    │                         截取约 90 秒片段（从 30% 处开始）
                    │                         → BS-Roformer 分离人声/伴奏
                    │                         → 自动音高（男歌手 +12）→ RVC 变成鲸鱼娘的声音
                    │                         → ffmpeg 混音、淡入淡出 → 24 kHz WAV
                    └─ 缓存 data/songs/ ，播放到设备（可随时打断）
```

* 唱过的歌会缓存，再点就立刻开唱；
* 没练过的歌，鲸鱼娘先说“这首我还没练过，得先练一下，练好了我叫你”。练习期间语音连接不会因为没人说话而断开；练好后等你不说话了，她主动问“练好啦，要不要现在听？”，你说“要／好呀／唱吧”就唱，说“先不听”就留着。设备中途断开的话，下次连上、你说完第一句话后她再问；
* 可以在管理页“唱歌”里贴一个 B 站／YouTube 歌曲链接，电脑用 yt-dlp 下载音频再练，流程同上；
* 渲染服务没开时，她会直接说“唱歌服务没开，先在电脑上启动 singing/worker.py 吧”（已经练好的歌照样能唱）。

渲染必须在 Windows 本机跑（要用显卡），不在 Docker 里。

---

## 1. 准备

| 项目 | 要求 |
| --- | --- |
| 显卡 | NVIDIA，8 GB 显存够用（RTX 4060 已按此配置） |
| 驱动 | 570 或更新（CUDA 12.8）。`nvidia-smi` 能看到显卡 |
| Python | 3.11（自动训练环境要求此版本）：`winget install Python.Python.3.11` |
| ffmpeg | `winget install Gyan.FFmpeg`（或 `choco install ffmpeg`），装完重开 PowerShell |
| 磁盘 | 约 8 GB（PyTorch + 模型） |

## 2. 安装

在项目根目录打开 PowerShell：

```powershell
powershell -ExecutionPolicy Bypass -File singing\setup_windows.ps1
# 国内下载 HuggingFace 慢：加 -HfMirror
```

脚本会：

1. 检查驱动和 ffmpeg；
   如果当前 Python 来自旧版 Conda，会下载项目独立的 Python 3.11 并切换本项目环境，避免 `c10.dll / WinError 1114`，不替换系统或 Conda 的 Python；
2. 建 `singing\venv`，安装 PyTorch 2.11（cu128）、`audio-separator[gpu]==0.48.0`（人声分离）、`infer-rvc-python==1.3.1`（RVC 推理，MIT，不依赖 fairseq）。安装器用 `uv` 覆盖旧依赖：`pyworld 0.3.5`、`torchcrepe 0.0.24`、`librosa 0.11.0`；直接 `pip install -r requirements.txt` 无法解决上游旧版本的冲突；
3. 下载公共模型：`models\rmvpe.pt`、`models\hubert_base\`（实际为 ContentVec，与 Applio 训练一致）、`models\separator\model_bs_roformer_ep_317_sdr_12.9755.ckpt`；
4. 生成共享口令 `singing\token.txt`，并在 `server\main\xiaozhi-server\data\server.env` 末尾追加 `SINGING_TOKEN=…`（已有就不动）；
5. 运行 `worker.py --check` 打印 GPU 和模型状态。

## 3. 鲸鱼娘的音色模型

`config.yaml` 默认读取 `singing\models\whale.pth` 和 `singing\models\whale.index`（.index 可选）。

### 3a. 已有 RVC v2 模型

直接把 `.pth`、`.index` 放进 `singing\models\`，改名或者在 `config.yaml` 的 `rvc.model` / `rvc.index` 里写文件名。

### 3b. 用鲸鱼娘说话的声音自己训练

**第一步：生成数据集**（BreezeBlue 云端 TTS，大肥鱼音色）

```powershell
cd singing
# Key 从环境变量读；也可以直接指向语音服务的 server.env（不会打印 Key）
venv\Scripts\python make_voice_dataset.py --voice clever --env-file ..\server\main\xiaozhi-server\data\server.env
# 先试听 5 句：加 --limit 5；dreamy 音色：--voice dreamy
```

* 自带 165 句（`dataset_sentences.txt`），约 3300 字、11 分钟音频；开始前会显示预计额度（每字约 0.2，全量约 670）并询问确认；
* 免费套餐同一时间只能合成一句，脚本逐句请求，大约 15–25 分钟跑完；中断后重新运行会跳过已生成的；
* 输出 `singing\dataset\clever\0001.wav …`（40 kHz 单声道）。云端声音是 24 kHz，高频上限 12 kHz；设备本来就按 24 kHz 播放，所以不影响听感；
* 注意：训练素材是“说话”不是“唱歌”，模型唱高音或长音时可能发虚。想更好听，可以往数据集里加几段鲸鱼娘音色的哼唱/拖长音（任何 40 kHz 单声道 wav 都行）。

**第二步：用 Applio 训练（RTX 4060 8 GB）**

推荐使用自动流程，在项目根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File singing\setup_training.ps1
singing\venv\Scripts\python singing\train_voice.py --epochs 100 --cache-gpu
# 想继续到 300 轮（保持同一批素材），从已保存的检查点继续：
singing\venv\Scripts\python singing\train_voice.py --skip-prepare --epochs 300 --cache-gpu
```

安装器把 Applio 3.7.1 放到 `.tools\Applio`，训练使用独立环境，避免影响唱歌推理。
默认 batch size 为 4，适合这台 8 GB 笔记本显卡。`--cache-gpu` 将这批素材缓存在显存，减少 Windows 上每轮读取素材的耗时；如果素材更多导致显存不足，去掉此参数。自动完成预处理、RMVPE 音高和 ContentVec 特征提取、HiFi-GAN 40k 训练及索引生成，然后部署到 `singing\models\whale.pth` 和 `whale.index`。
检查点、训练日志 `deepflipper.log` 和状态在 `singing\training\whale\`；`deepflipper-status.json` 的 `stage: done` 才代表部署完成。中断后重跑相同命令可以继续，素材变动时请使用新的 `--name`，避免沿用旧训练检查点。

也可以使用图形界面手动训练：

1. 下载 Applio 3.7.1 Windows 版：<https://huggingface.co/IAHispano/Applio/resolve/main/Compiled/Windows/ApplioV3.7.1.zip>，解压到**没有空格和中文的路径**（如 `D:\Applio`），双击 `run-applio.bat`，浏览器打开界面。（不要用管理员身份运行。）
2. **Train** 页签，按下面填写：

| 设置 | 值 |
| --- | --- |
| Model Name | `whale` |
| Dataset Path | `…\DeepFlipper\singing\dataset\clever` |
| Sampling Rate | **40k** |
| Audio cutting / 降噪 | Automatic / 关闭（TTS 素材很干净） |
| Pitch extraction (f0) | **rmvpe** |
| Embedder | contentvec |
| Batch Size | **8**（显存不足报 OOM 就改 4） |
| Total Epoch | **300**（10–15 分钟素材建议 250–300） |
| Save Every Epoch | 25（方便挑最好听的） |
| Pretrained | 默认（与 40k 匹配） |
| GPU | 0 |

3. 依次点 **Preprocess Dataset → Extract Features → Start Training**，训练完点 **Generate Index**。
4. 预计耗时：预处理+特征提取几分钟；训练 300 轮约 3–5 小时（按 4060 Ti 的公开数据推算，未在本机实测）。
5. 在 Applio 的 **Inference** 页签用一段干声试听不同轮数（`logs\whale\whale_<轮数>e_<步数>s.pth`），选最好听的那个，和 `logs\whale\` 里的 `.index` 一起复制为：

```
singing\models\whale.pth
singing\models\whale.index
```

换模型后旧缓存自动失效（缓存键包含模型文件大小和修改时间）。

## 4. 放歌曲

把**你自己的**歌曲文件（mp3 / flac / wav / m4a / ogg / webm …）放进原曲目录，默认是 `singing\songs_in\`。**文件名就是歌名**，建议 `歌手 - 歌名.mp3`，例如 `周杰伦 - 晴天.mp3`。说“唱晴天”“周杰伦的晴天”都能匹配。

### 4a. 原曲、下载和成品放在哪

| 项 | 默认 | 说明 |
| --- | --- | --- |
| 原曲目录 `songs_dirs` | `songs_in` | 可以写多个，也可以是绝对路径（如 `D:\Music`）。会往下找 3 层子文件夹，所以“歌手\专辑\歌.mp3”的音乐库可以直接用。不同目录有同名歌时，用列在前面的那份 |
| 下载目录 `download_dir` | 第一个原曲目录下的 `downloads\` | 链接下载的歌放这里；不在原曲目录里也会算进曲库 |
| 成品目录 `cache_dir` | `cache` | 练好的翻唱（24 kHz WAV + 参数 JSON）。语音服务那边另有一份播放缓存 `server\main\xiaozhi-server\data\songs\` |

两种改法：

1. 管理页 → **唱歌** → “歌曲路径”，保存后立刻生效，写在 `singing\config.local.yaml`（删掉它就恢复 `config.yaml` 的设置）；
2. 直接改 `singing\config.yaml` 的 `songs_dirs` / `download_dir` / `cache_dir`，重启 worker。

原曲目录必须已存在；下载和成品目录会自动创建。正在练歌时不能改成品目录。改了成品目录，旧目录里练好的歌不会自动搬过去（语音服务那边已经缓存的照样能唱）。

### 4b. 用链接添加歌曲

需要 yt-dlp（`setup_windows.ps1` 已包含；旧环境补装：`singing\venv\Scripts\python -m pip install yt-dlp`）。

* 管理页 → **唱歌** → “从链接添加歌曲”：粘贴 B 站、YouTube 等**单首**歌曲链接，可选填歌名（建议“歌手 - 歌名”），点“下载并练习”；有设备连着时，练好后鲸鱼娘会在设备上问你要不要听；
* 对话里给出链接时，模型也可以调用 `sing_song` 的 `url` 参数（语音里一般念不出链接，主要用于以后的文字输入）；
* 只下载音频，不重新编码；单个视频、不超过 20 分钟、300 MB；播放列表会被拒绝；和曲库已有歌曲同名时自动加“ (2)”；
* 下载的内容只用于你自己设备上的个人欣赏，请遵守来源网站的条款和版权规定。

## 5. 启动渲染服务

```powershell
powershell -ExecutionPolicy Bypass -File singing\start_worker.ps1
# 本次后台运行，不创建开机任务：加 -Background
```

* 监听 `0.0.0.0:8790`。第一次启动 Windows 防火墙会询问，**只勾“专用网络”**；
* 开机自动后台启动（登录时，隐藏窗口，日志 `singing\worker.log`）：

```powershell
powershell -ExecutionPolicy Bypass -File singing\start_worker.ps1 -InstallAutostart
Start-ScheduledTask DeepFlipperSingingWorker      # 立即启动
# 取消自启：start_worker.ps1 -RemoveAutostart
```

## 6. 语音服务这边

1. `data\server.env` 里要有 `SINGING_TOKEN=<和 singing\token.txt 相同>`（安装脚本已自动追加）；
2. 代码有更新，重新构建并启动：

```powershell
docker compose -f server/compose.local.yaml up -d --build
```

3. `config.yaml` 里的 `singing:` 段默认已开启：`worker_url: http://host.docker.internal:8790`（Docker Desktop 自带这个主机名），成品缓存在 `data\songs\`。要关掉唱歌，在 `data\.config.yaml` 写：

```yaml
singing:
  enabled: false
```

## 7. 测试

```powershell
$h = @{ 'X-Singing-Token' = (Get-Content singing\token.txt -Raw).Trim() }
Invoke-RestMethod http://127.0.0.1:8790/health                       # GPU、模型、队列
Invoke-RestMethod http://127.0.0.1:8790/songs -Headers $h            # 曲库
$job = Invoke-RestMethod http://127.0.0.1:8790/render -Method Post -Headers $h `
       -ContentType 'application/json' -Body '{"song":"晴天"}'
$job                                                                  # state=queued 或 done（有缓存）
Invoke-RestMethod "http://127.0.0.1:8790/jobs/$($job.job_id)" -Headers $h   # 看进度，直到 state=done
Invoke-WebRequest "http://127.0.0.1:8790/audio/$($job.render_id)" -Headers $h -OutFile test.wav
start test.wav
```

`/render` 可选参数：`pitch`（`"auto"` 或 -24..24 的整数半音）、`clip_seconds`（默认 90，0 = 整首）、`start_ratio`（默认 0.3）。

路径和下载接口（同样要带口令）：`GET/PUT /config`（`songs_dirs`、`download_dir`、`cache_dir`）、`POST /download`（`url`、可选 `title`、`render: true`）、`GET /downloads/<id>`（下载进度，`render` 里是接着练的任务）。

检查容器能否连到渲染服务：

```powershell
docker compose -f server/compose.local.yaml exec voice python -c "import urllib.request as u; print(u.urlopen('http://host.docker.internal:8790/health').read().decode())"
```

然后对鲸鱼娘说：“你会唱什么歌？”、“唱一首晴天”。播放中按设备的打断键或开口说话即可停止。

## 8. 参数（singing\config.yaml）

* `clip_seconds: 90`、`start_ratio: 0.3`：唱哪一段；
* `rvc.default_pitch: auto` + `pitch_mode: fit`（默认）：看整段人声的低音（第 10 百分位）、中位、高音（第 95 百分位），选一个移调让音域落进 `min_f0_hz`–`max_f0_hz`（200–480 Hz，鲸鱼娘说话素材的主要范围），中位尽量接近 `target_f0_hz`（330 Hz）。能整八度就整八度，否则伴奏一起移。音色只用说话训练过，高过约 550 Hz 会唱哑，这样就少唱那么高的音。`pitch_mode: octave` 是旧做法（只按中位整八度移，男歌手的歌通常 +12，副歌容易破）。也可以写成固定整数；非 12 倍数时伴奏会跟着移相同的余数半音；想根本改善高音，可以往 `dataset\clever` 里加几段哼唱/拖长音再训练；
* `rvc.index_rate 0.5`、`protect 0.33`、`filter_radius 3`、`rms_mix_rate 0.25`：RVC 常用值；
* `separator.overlap: 2`：分离速度/质量平衡（4 更好但更慢）；
* `mix.instrumental_volume: 0.8`、`loudnorm: true`：伴奏比人声小一点，并统一响度；
* `max_queue: 5`：同时只渲染一首，其余排队；`max_cached: 60`：最多保留 60 首成品。

每个阶段的耗时会打印在 worker 日志里（`阶段 separate 用时 …`）。显存：分离和 RVC 依次进行，每步结束都会释放显存，8 GB 足够。

## 9. 常见问题

| 现象 | 处理 |
| --- | --- |
| 鲸鱼娘说“唱歌服务没开” | worker 没启动，或容器连不到 `host.docker.internal:8790`（看第 7 节的 docker exec 测试、防火墙） |
| 说“口令不对” | `server.env` 的 `SINGING_TOKEN` 和 `singing\token.txt` 不一致；改完 `docker compose … up -d` |
| 说“唱歌音色模型还没放好” | `singing\models\whale.pth` 不存在 |
| `import pyworld` / numpy 报错 | 重新执行 `venv\Scripts\python -m pip install pyworld==0.3.5 --no-deps` |
| `CUDA out of memory` | 关掉占显存的程序；`separator.overlap` 保持 2；`clip_seconds` 改小 |
| `Weights only load failed` | .pth 不是标准 RVC/Applio 导出的模型，用 Applio 重新导出 |
| HuggingFace 下载失败 | `setup_windows.ps1 -HfMirror`，或稍后重试 `venv\Scripts\python fetch_models.py` |
| 分离模型下载慢 | 它来自 GitHub Releases，可手动下载放进 `singing\models\separator\` |

## 10. 版权

曲库里的歌由你自己提供（本地文件或你给的链接），只用于个人在自己设备上欣赏；不要公开传播翻唱成品。`songs_in/`、`cache/`、`models/`、`dataset/`、`config.local.yaml` 都已在 `.gitignore` 里，不会进仓库。

## 致谢

参数和流程参考了两个 MIT 许可的项目：[aureth-labs/dsh-aurabot-qq](https://github.com/aureth-labs/dsh-aurabot-qq)（`src/song/studio.js`：120 秒片段、30% 起点、BS-Roformer、rmvpe、index 0.5、protect 0.33、伴奏 0.8、淡入淡出）和 [Dhao007/dsh-singing-studio](https://github.com/Dhao007/dsh-singing-studio)（按八度移调、响度统一）。本目录代码为独立实现。推理用 [infer-rvc-python](https://github.com/R3gm/infer_rvc_python)（MIT），分离用 [python-audio-separator](https://github.com/nomadkaraoke/python-audio-separator)（MIT）。
