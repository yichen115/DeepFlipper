# DeepFlipper Python 服务端

只保留 [Python 语音服务](main/xiaozhi-server/app.py)和同进程的轻量网页管理。
不需要 Java、Node、MySQL 或 Redis。网页使用浏览器系统字体，不捆绑字库。

在项目根目录首次准备并启动：

```powershell
python scripts/setup_local_server.py --host 你的电脑局域网IP
python scripts/download_models.py
docker compose -f server/compose.local.yaml up -d --build
```

启动后打开 **http://127.0.0.1:8001**。管理口令在本机
`server/main/xiaozhi-server/data/admin-token.txt`，不在聊天或日志中输出。
Docker 将管理端口仅发布到本机，设备语音端口为 8000。

网页功能：

- 模型 API 地址、名称、密钥和一次无硬件动作的接口／工具调用测试。
- 系统提示词、设备工具调用开关、自动附加的工具使用规则。
- 已连接设备、设备上报的 MCP 工具和绑定 Device ID／Client ID 的 Token 签发。
- 语音服务状态和保存后应用。API Key 不回显，留空保留，清除需要显式选择。
- 本地 MeloTTS／在线 EdgeTTS 切换与浏览器试听。
- 按设备查看识别文字、模型回复和语音处理错误，对话记录持久化于本机。

默认使用本地 CPU 语音合成（MeloTTS 中英女声），复用已有 sherpa-onnx，
不需要显卡、微软接口或额外服务。首次下载约 167 MB；模型通过校验后才发布，
保存在忽略的 `models/vits-melo-tts-zh_en/`，不进入 Git 或 Docker 镜像。
已经下载 ASR／VAD 的用户只需补充 TTS：

```powershell
python scripts/download_models.py --tts-only
docker compose -f server/compose.local.yaml up -d --build
```

旧配置不会被下载脚本覆盖。在后台选择“本地 MeloTTS”，试听后“保存并应用”。
当前这台电脑已经切换为本地合成。此更改只需更新服务端，不需要重刷设备。
语音识别和合成可以本地运行，DeepSeek 文本模型仍使用已配置的 API。
合成失败会限时重试，结束本轮播放并保留文字，不再无限停在 Thinking。

填写模型前、缺少语音模型或语音初始化失败时，管理页仍能打开。
保存并应用会结束当前语音连接。配置与所有密钥仅存于忽略的 `data/` 中。
仍可直接编辑 `data/.config.yaml`；旧 Java 管理 API 配置会被明确拒绝。

设备控制通过系统提示词、MCP 工具定义和已实现的硬件执行代码完成。
工具能力以设备实际上报为准。增加提示词不会自动获得尚未适配的 BLE、
HID、卡片写入或第三方应用内部动作。

当前设备目录已扩展到 20 项：包括实时 NFC 读取、红外学习与保存、Sub-GHz
OOK RAW 录制，以及已有文件的发送／模拟；也加入了 SD 内容读取、容量、
时钟、GPIO 与长按。录制模块随 Flipper 更新包安装到 SD，不能只更新 ESP32。
能力仍以本次设备实际通过 MCP 上报的列表和最终结果为准。

同一连接内有对话上下文。重连默认恢复同一设备和 Client ID 最近 24 小时内
最多 6 轮完整对话、最多 6000 字，在“提示词与工具”页可设置为 0–12 轮。
0 关闭恢复；网页记录仍保留，这不等于模型拥有无限长期记忆。

默认 OTA／视觉 HTTP 服务关闭（`server.http_port: 0`）。模型文件单独下载，
不进入 Git 或镜像。保留 [LICENSE](LICENSE)，来源见
[sources.lock.json](../sources.lock.json)。完整部署与接线见
[项目 README](../README.md)。
