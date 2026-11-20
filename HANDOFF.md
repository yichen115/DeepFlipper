# DeepFlipper 开发交接

更新于 2026-11-20（北京时间）。历次构建、测试和故障排查见 [开发日志](docs/development-log.zh-CN.md)，最近三轮在第 21–23 节。

## 当前状态

| 模块 | 进度 |
|---|---|
| 语音服务 | 本轮改了：连续对话开关、对话记录带操作和截图（聊天视图）、云端语音响度拉平、语气更平稳、Sub-GHz 录制提示词、唱歌时动画和缓冲。需要重建 Docker 镜像。 |
| RVC 唱歌 | 曲库 = `singing\songs_in` + 本机网易云下载目录（502 首，路径在 `singing/config.local.yaml`）。自动移调改为“按音域适配”，高音不再超出音色模型练过的范围；已练过的歌下次点会重新练一次。worker 需要重启（并补装 yt-dlp）。 |
| 唱歌动画 | 第 13 组 `sing` 已进 `home_frames.bin`：练歌时和唱歌时显示拿麦克风唱歌。需要 Flipper + ESP32 都更新。 |
| Flipper ↔ ESP32 | AI 用 `flipper_app_open` 开的 App 保持连接；手动开 App 断开（例外：主菜单、设置、AI 设置页、宠物）。AI 开启时锁住 5V，防止 Sub-GHz 等 App 断 ESP32 电。待 Windows 构建和真机验证。 |
| 固件升级卡在只有电池条 | 原因未定位，复现时的取证步骤见开发日志第 21 节。 |

## 需要重新构建

| 端 | 包含 | 命令 |
|---|---|---|
| Flipper | 5V 保持、宠物页保持连接、About、唱歌动画与表情 12 | `.\scripts\build_flipper.ps1 -Package` |
| ESP32 | 连续对话、唱歌表情映射、3 秒解码队列 | `.\scripts\build_esp32.ps1 -Idf v6.1.1` |
| 语音服务 | 上面列的服务端改动 | `docker compose -f server/compose.local.yaml up -d --build` |
| 唱歌 worker | 音域适配、多目录、链接下载 | `singing\venv\Scripts\python -m pip install yt-dlp`，再重启 worker |

旧产物（`build/release/...`，2026-11-18）不含这些改动。

## 待真机验证

1. 开启 AI 让助手打开 Sub-GHz 进入 Read RAW 并开始录制：语音不断、真的按下 REC；对话记录里能看到每一步和截图。
2. 管理页打开“连续对话”：按一次键说话，说完自动回答，回答完不按键接着说；8 秒不说话自动停。
3. 点一首没练过的歌：练歌期间主页显示拿麦克风；练好后问“要不要现在听”并自动听回答；唱的时候显示唱歌动画。听高音是否还哑、中途是否断。
4. 主页右键进宠物不断开；About 第一页是 DeepFlipper。
