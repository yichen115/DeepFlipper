# 调研记录（2026-11-17）：ESP32 控制 Flipper、语音方案

本文只记录调研结论，**这两项都还没有改代码**。是否实施、按什么顺序做，以用户之后的决定为准。

## 1. ESP32／AI 控制 Flipper 的同类项目

| 项目 | 传输方式 | 怎样暴露给 AI | 可参考的地方 |
|---|---|---|---|
| [yasher3413/flipperclaw](https://github.com/yasher3413/flipperclaw) | FAP 在前台独占 USART，115200 波特率，按行 Base64 自定义协议（不走 Expansion，也不走 RPC） | 智能体循环跑在 ESP32 上，直连 Anthropic／OpenAI，只有 3 个 Flipper 工具 | 回答逐字推到屏幕；PING/PONG 断线检测；文件路径拒绝 `..` |
| [roostercoopllc/flipper-mcp](https://github.com/roostercoopllc/flipper-mcp) | ESP32-S2 上的 Rust MCP，走串口 CLI 文本，读到 `>: ` 为止 | SD 卡上的 TOML 命令模板；每个 FAP 生成一个 `app_launch_*` 工具 | 不刷固件就能重新加载工具 |
| [busse/flipperzero-mcp](https://github.com/busse/flipperzero-mcp) | ESP32 完成 Expansion 协商后，把 TCP 透传到串口；protobuf 在主机上解析 | Python MCP，按模块组织工具 | 有连接健康检查和重连工具 |
| [jamiew/flipper-mcp](https://github.com/jamiew/flipper-mcp) | USB 串口 CLI | 64 个工具；发射类默认禁用，设置 `FLIPPER_ENABLE_TX=1` 才开放 | 发射门槛；区分"命令被接受"和"真的成功了" |
| [girishalpha/flipper-zero-mcp](https://github.com/girishalpha/flipper-zero-mcp) | CLI，截屏时临时切到 RPC | 按键／导航工具可选按完立刻截屏 | `gui_start_screen_stream` 取一帧（1024 字节）后马上停止 |
| [flipperdevices/video-game-module](https://github.com/flipperdevices/video-game-module) | 官方模块，RP2040 芯片，Expansion + nanopb RPC | 屏幕流、虚拟显示屏、按键转发 | `gui_start_virtual_display`：由模块往 Flipper 屏幕推画面 |

flipperclaw 读源码确认的不足：

- 发射类工具只返回 "triggered"，不回报结果。
- NFC 和 Sub-GHz 操作在串口接收线程里同步阻塞，最长 30 秒。
- 没有任何确认环节。

结论：我们的 Expansion + 原生 RPC + SD 上的 App SDK + 目录 MD5 校验路线更严谨。不要回退到 CLI 文本解析或独占串口的自定义协议，那样等于多出第二个串口读取者。

### 值得借鉴（按建议优先级）

1. **截屏工具 `flipper_screen_capture`**（M）
   - 做法：用固件已有的 RPC 屏幕流取一帧后立刻停止，不改 Flipper 主固件。
   - 改动位置：ESP32 的 `deepflipper_bridge.cc`／`native_actions.h`；服务端把画面转成 PNG，或交给视觉模型。
   - 注意：必须确认流已经停止。
2. **按键序列 + 可选截屏**（S）：补上"送达不等于成功"的验证缺口。
3. **发射／模拟前确认**（S–M）：服务端开关 + 单次语音确认。改动在 `deepflipper_defaults.py` 和 MCP 执行层。
4. **虚拟显示屏**（M）：ESP32 把中文或波形渲染成 128×64 画面推到屏幕，省掉 Flipper 上的中文字库。需要处理好前台占用和退出。
5. **连接自检**（S）：扩展 `flipper_get_status`，一次报告链路、RPC、前台 App 和截屏是否可用。

未完全核实：

- GitHub API 限流，roostercoop 只看了文档。
- busse 是否按 64 字节拆帧，未确认。

## 2. 语音

| 仓库 | 是什么 | 适合我们吗 |
|---|---|---|
| [Dhao007/dsh-singing-studio](https://github.com/Dhao007/dsh-singing-studio) | DSH 插件（Node + Python），用 BS-Roformer 分离人声，再用 RVC 或 SoulX-Singer 换声后混音；首次安装约 7–9 GB；CPU 上唱一首要几分钟 | 只适合离线做翻唱，不能接进实时对话 |
| [breezeblue-ai/breeze-tts](https://github.com/breezeblue-ai/breeze-tts) | 开源权重，要求 ≥12 GB 显存的 NVIDIA 显卡、Linux；权重只许研究和非商用 | 本地跑不了 |
| BreezeBlue 托管 API（[文档](https://docs.breezeblue.ai/)） | 云端 TTS，支持 HTTP 流式和 WebSocket 实时 | **推荐接入** |
| [aureth-labs/dsh-aurabot-qq](https://github.com/aureth-labs/dsh-aurabot-qq) | 用"大肥鱼"人设做的 QQ 机器人（JS），支持 edge-tts／CosyVoice／GPT-SoVITS／通用 HTTP TTS，可选 RVC | 借鉴思路：失败时退回文字、语音缓存 1 天 |

**大肥鱼音色已核实**：官方博客 [Give Dafeiyu a Voice in Your QQ Group](https://breezeblue.ai/blog/give-dafeiyu-a-voice-in-qq) 列出两个音色。

- Dafeiyu · Clever：`voc_u3nxkd5578wc`
- Dafeiyu · Dreamy：`voc_8yud7hz7c9q2`

未核实的部分：

- 没有 API Key，没测这两个音色能否被任意账号调用。
- 没找到这两个音色单独的授权条款。

### API 要点（以官方文档为准）

| 项 | 内容 |
|---|---|
| 请求 | `POST https://api.breeze.blue/v1/text-to-speech/{voice_id}/stream?output_format=pcm` |
| 请求头 | `xi-api-key` |
| 请求体 | `{"text","language_code":"zh","voice_settings":{"speed"}}` |
| 输出 | 24 kHz 单声道 16 位 PCM。设备默认就是 24000 Hz，不需要重采样 |
| 文本长度 | 单次最多 1000 字 |
| 错误码 | 429 和 503 会带 `Retry-After`；402 表示额度用完 |
| 计费 | 1 个汉字算 2 个计费单位，每 10 个单位折 1 credit，即 1 个汉字 = 0.2 credit，50 字约 10 credits |
| 免费额度 | 每天可领 400 credits |

### 建议接入方式（未实施）

**新 provider：** `core/providers/tts/breeze.py`。

**配置项：**

- `api_key`：只从环境变量 `BREEZE_API_KEY` 读取，不写进仓库。
- 其他：`voice_id`、`base_url`、`speed`、`tts_timeout≈8`、`fallback: LocalTTS`。

**分两期：**

1. 第一期：每句一个请求，拼成 WAV 交给现有的按句流水线。
2. 第二期：改成边收 PCM 边按 60 ms 编码 Opus 推给设备。

**回退与熔断：**

- 一轮对话里只要有一句失败，这一轮剩下的句子都改用 MeloTTS，避免同一段话里声音跳来跳去。
- 连续失败后熔断 60 秒。

**注意事项：**

- **隐私**：即使设置 `enable_logging=false`，服务方仍会保留文本。发出前要过滤 UID、路径等敏感信息。
- **情绪标签**：`get_string_no_punctuation_or_emoji` 会剥掉句首和句尾的方括号。要用 `[笑]` 这类标签，得先改这个函数。
- **唱歌**：不在服务器上实时跑 RVC（需要 torch，延迟也大）。在有显卡的电脑上预先渲染几首短歌，再加一个"唱歌"工具直接播放这些文件。
- **授权**：免费档生成的音频不能商用；"大肥鱼"是别人的角色，对外发布前要先确认授权。
