# DeepFlipper 开发日志

归档截至 2026-11-19（北京时间）。保留历次开发、构建、验证和故障排查记录。文中的日期、版本、运行状态和待办对应当时的记录。命令和代码块中的相对路径从项目根目录算起。 [当前交接](../HANDOFF.md)

## 1. 2026-11-15 交接快照

当前项目是 **Unleashed 定制固件 + ESP32-S3 语音终端 + 小智 Python 服务端**。
远期目标是语音陪伴宠物，并通过统一接口控制 Flipper 外设与应用；目前仍沿用 Unleashed 的系统、Loader 和 SD 应用生态，没有重写整个操作系统。

当时最新一轮（本文件第 13 节）已完成：**主页信息融入房间物件（闹钟显示时间、电饭煲显示电量、头顶气泡显示状态），
宠物 App 改成全屏房间 + 底部单项英文菜单（方案 B2），互动逻辑优化 8 项，新增 Catch Rice 小游戏，主页只读同步宠物状态。**
代码与主机测试通过；**完整 F7 更新包已在 Linux 沙箱用官方工具链 39 构建成功**（固件 876300 字节，radio 余量 4340 ≥ 4096），
产物已放到 `build/release/flipper/`，**尚未刷机、未做真机验证**。ESP32 和服务端本轮没有改动，不需要重新编译或部署。

下一轮第一件事（2026-11-15 时）：请用户安装 `build/release/flipper/flipper-z-f7-update-local.tgz`（只更新 Flipper），收集真机反馈。不要重新把喂饭、摸摸等操作塞回主页。

## 2. 用户已经明确的方向

- 外观、灵动、丰富表情和情绪价值优先，对战和好友后做。
- 保留已接受的鲸鱼娘身份与原始 64×64 单色像素角色，不擅自换角色。**2026-11-15 曾尝试参考“大肥鱼”重画 Q 版角色，用户看了草稿明确否决（“不好看、不可爱”），要求保留原设计。不要再重画角色。**
- 像素画必须在 128×64 原尺寸下看得清，不要糊成一团；大面积不用抖动灰点，用稀疏规则纹理和描边清楚的物件。
- 大的视觉改动先出原尺寸 + 放大的草图／动画给用户看，确认后再写固件。
- 主页信息不要生硬地贴在四个角，要融合在场景物件里；App 文字用简短英文（固件字体），用户比较过中文 12px 后选了英文。
- **最喜欢白米饭**。用户参考图已保存到 [favorite-rice.png](../assets/whale-pet/source/favorite-rice.png)。
- 不喜欢“左边角色、右边单纯换行文本”的界面；生活场景也不能只是几个简陋符号。
- **主页只做预览**，保持系统菜单和其他 App 的入口。照料、数值、表情展览放在宠物 App 内。
- 宠物 App 用户选定 **方案 B2**：底部一次只显示一个“图标 + 英文”动作，左右切换、OK 执行（只靠图标看不懂）。
- AI 开启时：短按 OK 开始／结束录音并发送，长按 OK 打开系统菜单。
- 默认模式下，主页短按右键直接进入宠物 App。
- 不在主页显示长聊天字幕，回答主要走语音。
- 曾反馈“按什么键都到 Lenada 页面”，后来确认是用户误开了另一模式，页面确实是 Passport。不要再按固件按键故障去修 Dummy Mode。

## 3. 环境与工作区

| 项目 | 交接时状态 |
|---|---|
| 工作区 | `C:\Users\yichen\Desktop\DeepFlipper` |
| 终端 | Windows PowerShell |
| 分支 | `master` |
| HEAD | `3acf089`，提交说明“修复音频断断续续的问题” |
| 工作树 | 存在大量修改和未跟踪的新文件，尚未提交 |
| Flipper FAP API | `88.25` |
| ESP32 | `deepflipper-s3`，N16R8，ESP-IDF `release/v6.1` |

本轮 SDK、采集能力、宠物 App、素材、主页等改动大量还在工作树里；**只读取 HEAD 会漏掉已经实现的功能**。
继续前先看 `git status --short` 和实际文件，保留已有修改，不做整树 reset／restore／clean。
本轮未创建提交，也未发布 Release。

修改 ESP32 前阅读其局部说明：[firmware/esp32/AGENTS.md](../firmware/esp32/AGENTS.md)。
构建输出、日志和部分预览在忽略目录内；换机器时不能假定这些本地产物还存在。

## 4. 实际主页：已经接进固件（画面与角标部分已被第 13 节取代）

### 画面

- 128×64 全屏生活房间，角色按原始 64×64 尺寸显示在 `x=33..96`。
- 左侧窗帘、窗户与柜子，右侧书架、鲸鱼画、灯和沙发，中间有地毯。
- 左上时间、右上电池百分比、左下单个语音状态、右下 `PET>`；锁定时右下替换为锁图标。
- 使用本机 RTC，07:00 起日间、20:00 起夜间。遵循系统 12／24 小时时制。
- 沿用主页原有的 **12 组语音角色动画**及实时嘴型；**尚未接入宠物存档或 App 的 19 组照料动作**。
- 静音或零音量回复使用待机素材并显示 `MUTE`，不继续播放说话嘴型。

主机渲染预览（不是实机截图）：[日间](../assets/whale-pet/home/ready.png)、[夜间](../assets/whale-pet/home/night.png)、[倾听](../assets/whale-pet/home/listening.png)、[资源回退](../assets/whale-pet/home/fallback.png)。

### 按键

| 场景／按键 | 当前行为 |
|---|---|
| AI 开启，短 OK | `talk/toggle`，录音／发送 |
| AI 开启，长 OK | 系统应用菜单；Debug 标志不会夺走这个入口 |
| AI 关闭，短 OK | 原有系统菜单 |
| 默认模式，短右 | 打开 `/ext/apps/Games/whale_pet.fap` |
| 系统警告／等级动画中的右键 | 优先确认原有警告或动画，再恢复普通快捷入口 |
| 上、下、左、长右 | 保留原有入口；长 OK 在 AI 关闭时保留原配置／Debug 行为 |
| Dummy Mode | 保留原有 Dummy 配置，不能假定短右一定进入宠物 |

短右经过动画层输入回调，最终在桌面场景的 `DesktopAnimationEventInteractAnimation` 中处理。
保留 `animation_manager_interact_process()` 优先级后才调用普通 Loader 启动宠物；缺少 FAP 时走系统标准错误提示。
不要绕开这条路径，把警告确认、长按或 Dummy Mode 一起抢走。

### 主要文件

| 文件 | 作用 |
|---|---|
| [deepflipper_sprite.c](../firmware/flipper/applications/services/deepflipper/deepflipper_sprite.c) | 后台读帧、实时嘴型、完整主页和状态角标 |
| [whale_data.h](../firmware/flipper/applications/services/deepflipper/whale_data.h) | 生成的序列表、嘴型和完整主页回退图；本轮是有意修改 |
| [desktop_scene_main.c](../firmware/flipper/applications/services/desktop/scenes/desktop_scene_main.c) | OK／菜单与短右打开宠物 |
| [desktop_view_main.c](../firmware/flipper/applications/services/desktop/views/desktop_view_main.c) | 原有主视图输入分发，本轮无需改动 |
| [gui.c](../firmware/flipper/applications/services/gui/gui.c) | 主页自行显示状态时保留后台窗口提示，位置已调整 |
| [export_flipper_assets.py](../scripts/export_flipper_assets.py) | 导出主页动画表、完整回退图和 SD 帧库 |
| [make_home_assets.py](../scripts/make_home_assets.py) | 将角色与日夜房间预合成 |
| [make_pet_room_concepts.py](../scripts/make_pet_room_concepts.py) | 共享房间绘制和不透明遮罩函数，同时供设计预览使用 |
| [render_home_preview.py](../scripts/render_home_preview.py) | 根据原生测试捕获的生产绘制调用，使用固件字体与图标生成预览 |

### 内存与读帧约束

- SD 文件 `/ext/deepflipper/home_frames.bin`：日夜各 129 帧，每帧 1024 字节，共 **264192 字节**。
- 显示帧和工作暂存各一个静态 1024 字节缓冲区，不把整个帧库载入 RAM。
- GUI 线程栈只有约 2 KiB，不要在绘制路径上新增 1 KiB 整屏局部数组。
- 后台服务工作线程栈 3072 字节；GUI 只在短暂持有 `sprite_mutex` 时画内存，**GUI 不读 SD**。
- 文件缺失、长度错误、定位失败或短读均回退到完整内置房间与角色，避免半帧污染。
- 实时嘴型平移后的区域为 `x=58..62, y=33..36`；行跨度为 16 字节，不能照搬旧 64 像素画面的 8 字节跨度。

## 5. Whale Pet：独立 SD 应用（界面与按键已被第 13 节取代）

入口：`Applications → Games → Whale Pet`，或普通主页短右。
源码：[whale_pet/](../firmware/flipper/applications/examples/whale_pet/)。详细设计：[宠物系统](pet-system.zh-CN.md)。

已经实现：

- 离线摸摸、喂白米饭、玩球、睡觉／叫醒、五级成长、数值页和表情展览。
- 19 组动作，808 个序列帧、237 个去重帧；`pet_frames.bin` 为 **121344 字节**。
- 期待吃饭、捧碗吃饭、满足、吃饱等阶段；喜欢白米饭不只是文案标签。
- 温和的离线变化、冷却、RTC 回拨处理，不因久未打开而死亡或丢等级／亲密度。
- 两份轮换 CRC32 存档：`/ext/deepflipper/pet/state-a.bin`、`state-b.bin`。写后 sync 并读回校验。
- 双份存档均损坏时只读，不自动覆盖旧宠物；保存失败不声称成功。

App 内按键：短 OK 摸摸，上喂饭，右玩球，下睡觉／叫醒，左进表情展览，长 OK 看数值；返回退出子页或 App，长返回从任意页面退出。

**界面边界：本轮优化的是实际主页。宠物 App 主页面仍是左侧角色、右侧说明的旧布局，并没有同步换成全屏房间。**
此前交互式生活房间只是设计预览，不能把它当作 App 已完成改版的证据。

**语音边界：手动打开宠物 FAP 会暂停 AI UART 并释放后台资源，退出后恢复。当前不能在该手动 App 内连续语音聊天。**
RPC 入口提供 `pet.state`、`pet.interact`；与本机照料共用模型和存档，但 RPC 照料只更新状态，返回 `displayed:false`，不会播放前台宠物动作。

主要文件：`whale_pet.c`（界面／输入／SDK 动作）、`pet_model.c`（养成规则）、`pet_store.c`（持久化）、生成的 `pet_art.h`（动画表）。
素材生成：[make_pet_assets.py](../scripts/make_pet_assets.py)；原图与预览：[assets/whale-pet/](../assets/whale-pet/)。

## 6. 前面已完成的统一 App 控制架构

这个方向已经有实际代码，不要下一轮重新设计一套平行协议。先读 [App 控制接口](app-control.zh-CN.md)。

- SDK 位于 [lib/deepflipper_app/](../firmware/flipper/lib/deepflipper_app/)，应用声明动作、参数说明与处理函数。
- 复用 Expansion、原生 Protobuf RPC、Loader、DataExchange 和 SD 有界结果文件；串口只有一个读取者。
- 登记文件由 FAP 与 `ai_app.json` 导出，核对路径、大小和摘要；构建后应重生成登记，不能混用旧 JSON 与新 FAP。
- 三个通用工具：`flipper_app_catalog`、`flipper_app_describe`、`flipper_app_call`。新 App 动作不需要逐个增加 ESP32／Python 专用工具。
- 当前语义协议 v1、App SDK v2；这与固件的 FAP API `88.25` 是不同的版本概念。
- SDK v2 支持 `keep_open` 保留会话、查询／停止、续期和锁屏／断连清理；lease 范围 5–60 秒。
- MCP 的 `arguments` 是编码后的 JSON **字符串**，桥接后才还原成对象，不能随意改成外层对象参数。
- 普通第三方 FAP 仍可手动安装／打开；只有主动接入 SDK 的应用才有可发现、可查询的语义动作，不能声称任意 App 内部功能都可由 AI 控制。

第一批工具 FAP：[deepflipper_tools/](../firmware/flipper/applications/examples/deepflipper_tools/)。已接入 NFC 读取、IR 学习、Sub-GHz OOK650 RAW 录制、离线解析、跨多次调用的射频接收 start/state/stop。
ESP32 侧主要在 `deepflipper_bridge.cc/.h`、`native_actions.h`、`app_catalog.h`。

射频解析复用协议注册表，主机用真实 Princeton 解码函数测试；不等于所有调制／加密信号都能解析。
`decoded:false` 是未识别；本机发送结束不证明外部目标响应。没有完成通用 FSK、任意加密业务解析或任意第三方 App 自动适配。

## 7. 构建与测试：可复用命令

在仓库根目录的 PowerShell 执行。Python 资产生成需要 Pillow；原生测试用 `uv` 提供 Zig。

```powershell
# 完整构建：会准备依赖、同步协议、生成主页和宠物素材、编译两个 FAP、导出登记并生成更新包
.\scripts\build_flipper.ps1 -Package

# 单独修改素材时可先重新生成，再运行测试
python scripts/export_flipper_assets.py
python scripts/make_pet_assets.py

# 编译并执行生产代码的主机测试
uv run --with ziglang python scripts/test_native.py

# 项目 Python 测试；不改变系统 Python 的依赖安装
uv run --with aiohttp --with pyyaml --with numpy --with openai --with pillow python -m unittest discover -s tests -v

# 运行原生测试后，可重新生成实际主页与宠物 App 的生产绘制预览
python scripts/render_home_preview.py
python scripts/render_pet_preview.py
```

ESP32 只有修改相应代码时才需要重建；先阅读局部 AGENTS 并准备 IDF 6.1 环境：

```powershell
.\scripts\build_esp32.ps1
# 已激活正确的 IDF 6.1 环境时，也支持：
.\scripts\build_esp32.ps1 -UseCurrentEnvironment
```

以上包装脚本不会自动刷机。本轮主页改动没有再次修改或构建 ESP32。
此前统一 SDK 阶段已记录 ESP32 目标编译成功，详见 [实现记录](implementation.md)，不要误写成本轮新验证。

### 最近验证结果

| 检查 | 结果／本地日志 |
|---|---|
| 原生测试 | **35 组通过**；`build/home-native-tests.log` |
| 项目 Python 测试 | **89 项：77 通过、12 跳过**；跳过的是缺少可选完整语音运行依赖的测试；`build/home-python-tests.log` |
| 主页帧测试 | 258 张合成帧逐像素核对角色黑色像素、白色内部和房间；`build/home-asset-tests.log` |
| 完整 Flipper 包 | 构建成功；`build/home-flipper-build.log` |
| 包内资源 | 已解压 `resources.ths` 核对主页、宠物帧、宠物 FAP 和登记与导出文件逐字节一致；`build/home-release-validation.json` |
| 真机 | **未刷机，未验证这版主页／宠物／射频端到端行为** |

关键回归测试：[desktop_pet_shortcut_test.c](../tests/desktop_pet_shortcut_test.c)、[home_sprite_test.c](../tests/home_sprite_test.c)、[test_home_assets.py](../tests/test_home_assets.py)。
原生测试执行实际输入、动画确认和场景分发，覆盖 AI 聊天／菜单、短右、系统警告、升级动画、原快捷键和 Dummy Mode。
完整绘制测试还覆盖日夜边界、嘴型范围、静音、缺帧回退以及 GUI 不读 SD。

注意：`docs/implementation.md` 的旧章节保留历史测试数字，最新结果以末尾主页章节和本节为准。
此前跑过的继承 ESP32 大套脚本测试存在 Windows 环境错误，不能宣称整个上游测试套件全通过。

## 8. 历史构建产物与安装边界（现用产物见第 20 节）

| 本机产物 | 用途 |
|---|---|
| `build/release/flipper/flipper-z-f7-update-local.tgz` | **本轮完整更新包，包含主页代码、主页素材、两个 FAP 和登记** |
| `build/release/flipper/f7-update-local/` | 完整更新目录，安装说明见 [SD 更新说明](flipper-update.zh-CN.md) |
| `build/release/flipper/WhalePet-sd-preview.zip` | 仅宠物 FAP、宠物帧和登记；**不会更新主页和按键**；要求匹配 API 88.25 固件 |
| `build/release/flipper/firmware.bin/.dfu/.elf` | 构建导出，不应只刷一个 bin 然后遗漏 SD 资源 |
| `build/release/esp32/` | 之前导出的 ESP32 产物，不是本轮主页构建的新结果 |

交接时完整 TGZ 为 **3684287 字节**，固件 bin 为 **875788 字节**。
固件到 radio 地址 `0x080D7000` 的余量是 **4852 字节**，仅比项目的 4096 字节最低检查多 756 字节。
后续功能优先放在 SD FAP 或生成资源，不要忽略内部 Flash 的余量。

完整包 SHA256：

```text
842135802b172120b9cad2f8bc33ddf7339222969d5cf574008877f733d2ca27
```

这些数字只识别本次产物，下一次构建后应更新记录。完整资源哈希可查本地 `build/home-release-validation.json`。
`build/verify_home_release.py` 是本机临时辅助脚本，需 `heatshrink2` 与 Pillow；它不在版本控制内，迁移机器时不能依赖它必然存在。

## 9. 已踩过的坑

1. **概念预览不等于固件已修改。** `assets/whale-pet/concepts/` 和旧交互预览曾展示六个照料按钮；那不是现在用户确认的主页功能分工。最新真实主页预览在 `assets/whale-pet/home/`。
2. **遮罩丢角色。** Pillow mode-1 掩码可能用 0／1，不一定是 0／255。反色后再用 `pixel == 0` 打包会得到空掩码。保持 `pack()`（黑色画面）和 `pack_mask()`（布尔不透明）分开，保留角色白色脸部、衣服内部和边缘纸色。
3. **预览糊成一块。** 旧浏览器概念预览的透明主题色／累积绘制曾造成叠影；修正为不透明 RGB 和整帧替换。真实固件现在整帧重绘，但 LCD 实际拖影仍需设备验证。
4. **旧清理脚本有过期假设。** `build/restore_generated_assets.py` 原来假定 `whale_data.h` 与 HEAD 仅换行不同；现在该头文件包含新主页回退图，不能照旧整体还原。只对确认解码后像素相同的 Passport PNG 做局部还原，不丢弃新头文件。
5. 构建会重编码 13 张 Passport PNG；本次已确认像素完全相同并还原这些无意义差异。原角色没有换稿。
6. 不要全局忽略 `.bin`／图片；`home_frames.bin`、`pet_frames.bin` 和其他固定素材属于运行资源。源码、生成表与帧库必须配套。
7. 不要增加第二个 UART／RPC 读取者。手动应用暂停语音是当前资源策略，不是已经支持“App 内边玩边聊”。
8. README 仍有少量旧描述，例如前面的“角色在左、状态在右”条目，以及未包含宠物打包步骤的旧 Linux 手工命令片段；当前实现以本文件、最新规格和实际包装脚本为准，后续可做文档统一。

## 10. 尚未完成及建议续写顺序

1. **先做真机反馈闭环。** 检查完整包安装、短 OK／长 OK／短右、返回桌面、锁屏提示和 SD 缺失回退；观察角色节奏、实际 LCD 清晰度、拖影和资源加载延迟。
2. **宠物 App 自身的界面仍待优化。** 可将用户喜欢的生活房间延伸进 App，改善右侧文字堆积；照料交互保留在 App 内，主页继续只预览。
3. **共享状态尚未接好。** 后续让主页被动展示与 App／语音相同的宠物心情、睡眠和生活状态。先设计共享数据与并发保存规则，不要复制出第二套养成模型。
4. **App 内持续语音尚未实现。** 需要结合前台生命周期、UART／RPC 所有权与内存预算解决，不能简单去掉暂停逻辑后声称可用。
5. **表现层可继续拆分。** 当前表情是预绘组合；独立行为、情绪和嘴型、被打断、等待、安慰和偏好记忆尚有空间。
6. **通用 App 能力逐项扩展。** 在现有 SDK 上新增动作／状态／清理接口，避免对每个动作再改一套 ESP32 与服务端专用分支。对外设操作按真实结果验收。
7. 好友、NFC／RFID 贴一贴、Sub-GHz／BLE／GPIO／iButton 玩法和对战留在后续阶段；没有已完成的联网对战实现。

这些是交接建议，不是用户已批准同时启动的全部任务；下次以用户当时的优先级继续。

## 11. 服务端与 Docker 的历史问题

用户此前报告 Docker Desktop 的 Linux engine API 500，以及执行 `wsl.exe -l -v --all` 超时。
本段对话后续转向宠物，**没有证据证明 Docker／WSL 已修好，也没有在本轮重新检查其状态**。
需要继续服务端时，先核实当前 Docker／WSL 的实际状态，不能把旧报错推断成应用本身启动失败。

项目服务端入口、私有配置和初始化步骤见 [README 服务端部分](../README.md#服务端部署与运行) 与 [server/README.md](../server/README.md)。标准启动命令为：

```powershell
docker compose -f server/compose.local.yaml up -d --build
```

PowerShell 命令末尾不要附 Linux 风格的反斜杠。不要把设备／服务端私有 Token、管理员口令或模型 API Key 写进交接文档。

## 12. 下次接手的阅读顺序

1. [当前交接](../HANDOFF.md)和用户的新消息。需要背景时再读本文件第 2、13、17–20 节。
2. [README.md](../README.md)、[最终规格](final-spec.zh-CN.md)。
3. [宠物系统](pet-system.zh-CN.md)、[App 接口](app-control.zh-CN.md)。
4. [实现记录](implementation.md) 最后的 “Offline pet preview” 和 “Display-only home and pet shortcut” 两节。
5. 根据任务查看对应源码；修改 ESP32 前再读其 AGENTS。
6. 检查当前工作树、当前构建时间和当前设备反馈，再继续实现，不重复已经完成的架构和素材修复。

## 13. 本轮（2026-11-15 晚）：主页物件化信息 + 宠物 App B2 + 互动优化

本节优先于上文第 4、5 节中关于主页四角、App 左图右字的旧描述。设计草图在 `assets/whale-pet/ui-preview/`（含未采用方案，不是固件输出）；
生产代码渲染的真实画面在 `assets/whale-pet/home/` 与 `assets/whale-pet/app/overview.png`。

### 主页（主固件，Flash 敏感）

- 房间重画：菱形小花墙纸、双腰线、错缝地板、地毯、带帷幔窗帘的窗户（夜晚窗外全黑 + 月亮星星）、柜子上的闹钟、搁板与鲸鱼玩偶、台面上的电饭煲、右下门口地垫 `>>`。
  素材函数在 `scripts/make_pet_room_concepts.py` 的 `room()`／`home_room()`；`export_flipper_assets.py` 照旧合成 `home_frames.bin`（尺寸不变 264192）。
- `deepflipper_sprite.c`：**后台线程**把实时内容画进它本来就合成的帧：闹钟屏幕上的时间（4×7 自绘数字，所有时间左右 ≥2 px）、电饭煲屏幕上的电量（`100%` 左 3 右 2 px）、
  充电热气（`snapshot.charging`，来自 `PowerInfo.is_charging`）、头顶 9×9 气泡（锁 > AI 状态 > 宠物愿望）。GUI 回调只拷贝整帧，不再格式化文字、不读 SD。
- 旧的 READY/REC 文字角标改为图标：麦克风、…、声波、喇叭打叉、?、!、Zz、锁。后台窗口提示移到左上角（`gui.c`）。
- **Flash 估算**：用 clang -Os（Cortex-M4）对比新旧 `deepflipper_sprite.c` 目标文件，代码+只读数据约 **+507 字节**；上轮余量 4852 → 预计约 4340，仍高于 4096 检查线但很紧。
  这是估算，gcc 实测可能不同；**必须用真实构建确认**。若超线，优先把气泡图标或数字表挪到 SD 资源，或去掉 `wifi sleeping` 分支。

### 主页同步宠物状态（只读）

- App 每次保存成功后写 `/ext/deepflipper/pet/home.bin`：`"DFH2"` + 8 × `{u32 时间, u8 愿望}`（0 无、1 睡觉、2 饿、3 困、4 无聊），
  由 `whale_pet_forecast()` 用真实结算规则（含 6 小时补算上限、自动醒）预测未来 48 小时，只记录变化点。
- 主页约每 5 秒读一次，取最后一个已到时间的条目：饿→饭碗气泡，困→Zz，无聊→球（三者 4.8 s 显示 / 2.4 s 隐藏），睡觉→Zz 气泡并播放主页睡觉动画（AI 忙时让位）。
- 存档槽仍是唯一真实数据，主页从不写宠物文件；预测文件缺失／损坏时什么都不显示。RPC `pet.interact` 走同一 `whale_pet_store_commit`，同样会更新预测。

### 宠物 App（SD 上的 FAP，不占主固件 Flash）

- 新文件：`pet_ui.c/h`（纯逻辑控制器，无 Furi 依赖，主机可测）、生成的 `pet_clips.h`（动作长度/循环）、`pet_art.h` 新增日夜房间、遮罩回退图和全部小图标。
- `pet_masks.bin`（121344 字节，与帧同序）随资源打包；`package_pet_preview.py` 已把它加入 SD 预览包并校验。
- 画面：全屏房间（左上数值卡、左下玩具篮、右侧窗户 + 白米饭餐桌），角色先画遮罩再画墨线；底部单项菜单 `◀ 图标 Feed ▶`，动作播放时菜单让开。
- 互动优化：需求气泡 + 智能默认项、成功后数值条旁闪 `+`、冷却/吃饱/太累用气泡说明、动作播完前只响应返回、只有真实变化才写 SD、
  睡到精力 100 自动醒、升级 `Lv.N!` 庆祝 + 数值页“Next”进度条、按离开时长/时段的欢迎语、连摸 `Hehe~ → Heh... → Hey!`。
- 表情展览移到数值页里：长按 OK 或菜单 Stats 进数值页，左右进入展览。
- Catch Rice：左半屏接米粒（饭团 3 分），3 颗心或 30 秒结束；与玩球共用 120 s 冷却和精力 ≥25 规则（`whale_pet_game()`）。

### 本轮验证

| 检查 | 结果 |
|---|---|
| 原生测试 `scripts/test_native.py` | **36 组通过**（新增 `pet_ui_test`、`pet_render_test`，删除旧 `pet_input_test`；`home_sprite_test` 改为检查物件上的数字、气泡、睡觉同步、热气、锁，并与真实 App 编码端到端对接） |
| `pet_render_test` | 用固件 u8g2 字体（`scripts/host_fonts.py` 导出）光栅化 `whale_pet.c` 的生产绘制函数，断言所有文字不出屏 |
| 宠物 FAP 源码 | 用 zig/clang 以 Cortex-M4 + 真实 Flipper 头文件 `-Wall -Wextra -Werror` 编译通过；新用到的 API 都在 `api_symbols.csv` 中 |
| Python 素材测试 | `test_pet_assets`、`test_pet_room_concepts`、`test_home_assets` 通过；全量 unittest 在本次沙箱中 72 项运行、14 跳过，唯一错误是沙箱缺 `aiohttp` 无法导入 `test_admin_server`（与本轮无关） |
| F7 完整构建 / radio 余量 | Linux 沙箱 + 官方工具链 39 构建 `updater_package DEBUG=0` 成功；固件 **876300 字节（比上轮 +512）**，radio 余量 **4340 字节**（≥4096）；解压 `resources.ths` 逐字节核对 8 个资源（含 `pet_masks.bin`、两个 FAP、两份登记）与项目文件一致，见 `build/home-release-validation.json` |
| 真机 | **未刷机、未验证** |

### 本轮构建产物（2026-11-15 晚，`build/release/flipper/`）

| 产物 | 说明 |
|---|---|
| `flipper-z-f7-update-local.tgz` | **完整更新包**，3695385 字节，SHA256 `583babf688bf93d2d472bc748bc79fcdb5afca750fb8f65258323b55fee9dc95` |
| `f7-update-local/` | 同一更新包的解压目录（`update.fuf`、`resources.ths` 等） |
| `firmware.bin/.dfu/.elf` | 固件 876300 字节；不要只刷 bin 而遗漏 SD 资源 |
| `WhalePet-sd-preview.zip` | 仅宠物 FAP + 帧 + 遮罩 + 登记，**不含主页改动** |

只需更新 Flipper：ESP32 运行时从 SD 登记文件读取 FAP 大小／MD5 并与实际文件核对，不编译进固件；`pet.state`／`pet.interact` 接口和服务端提示词都没变。

**Linux 构建注意**：官方 Linux 工具链的 `lib/bfd-plugins/` 里没有 `liblto_plugin.so`，fbt 打包后执行的 `arm-none-eabi-ranlib` 会把 LTO 静态库的符号索引重写成只有 `__gnu_lto_slim`，
导致固件链接时大量 `undefined reference`（aci_gatt_*、version_get_* 等）。沙箱里的做法是把 `libexec/gcc/arm-none-eabi/12.3.1/liblto_plugin.so` 复制到工具链 `lib/bfd-plugins/` 后删除旧 `.a` 重建，
**没有修改项目的 `firmware.scons`**。Windows 构建此前正常。

### 下一轮建议

1. 用户安装完整更新包后的真机反馈；如再构建，更新上表的大小与 SHA256。
2. 真机看：闹钟/电饭煲数字清晰度、气泡位置、热气、Catch Rice 手感（下落速度、碗速 2 px/50 ms）、按住左右是否顺滑。
3. 下一批扩展（用户已认可方向）：亲密度解锁房间装饰、每日陪伴（只奖励不惩罚）。

## 14. 2026-11-16 反馈后的调整（未构建）

- 用户要求：**以后用用户的 Windows 环境编译**（`.\scripts\build_flipper.ps1 -Package`），不要在沙箱另装工具链。第 13 节的更新包是沙箱编的，已被本节改动取代，需要用户在 Windows 重编。
- 宠物 App 数值卡去掉了顶部图钉（原尺寸像一个“+”）。
- Catch Rice 降难度：碗 13→17 px、移动 2→3 px/步且轻点即挪 4 px、心 3→5、同屏 4→3 粒、下落起速 3→2（上限 8→4，按每 8 分加速）、生成间隔 26→34 tick（下限 18）、判定上下各放宽。
- 精力：睡觉 +8→+15 /10 分钟；喂饭成功 +6 精力。宠物不会死亡（数值最低 20，不扣等级／亲密度）。
- 系统页面鲸鱼娘替换：草图 `assets/whale-pet/ui-preview/system-icons-draft.png`、`system-slides-draft.png`，**等用户确认后再替换**。
  图标在主固件 Flash 内（余量仅约 4.3 KB），替换前要比较压缩后大小；幻灯片（更新完成 `assets/slideshow/update_default`、首次开机 `first_start`）在资源里，不占 Flash。
- SDK：本项目 API 88.25；Unleashed 最新 unlshd-095（2026-11-11）为 88.26，官方 1.5.1（2026-10-15）为 88.2，工具链仍是 39。差距很小，合并需要确认本 fork 的上游基准提交（可能是 `../refer/unleashed-firmware`）。

### 14.1 Flash 瘦身第 1 项（2026-11-16，待用户 Windows 构建确认）

- 用户 Windows 构建（第 14 节改动之前一版）：固件 876300 字节，radio 余量 4340。
- 体积分析（按 `firmware.elf` 符号）：Sub-GHz 库约 118 KB、NFC 库约 106 KB、furi_hal 约 59 KB、GUI 约 46 KB、图标/动画约 38 KB、mjs 约 30 KB、API 表 24 KB；DeepFlipper 主页渲染约 3.9 KB。
- 已删除主页看不到的海豚资源：`assets/icons/Animations/Levelup1|2_128x64`（约 12.6 KB）、内置 `L1_Tv`、`L1_BadBattery`，`L1_NoSd` 只留 1 帧（`HARDCODED_ANIMATION_NAME` 改为它）。
- 升级流程：仍先显示 L0 NewMail 卡片；按键后 `dolphin_upgrade_level()`，不再进入 one-shot 海豚动画，而是 `deepflipper_celebrate()`：主页约 4 秒播放开心动画 + 星星气泡（语音状态优先），然后回到待机。`one_shot_animation_view.c` 不再被引用，由 LTO 去除。
- 预计省约 18–19 KB；原生测试 36 组通过（`desktop_pet_shortcut_test` 与 `home_sprite_test` 已更新）。实际数字以用户下一次 Windows 构建为准。
- 备选后续（未做）：电源设置／存储设置／文件浏览器／Sub-GHz 界面改成 SD 上的 FAP（约 18–44 KB）；删除 JS 引擎（约 28 KB，会改动 API，不推荐）。

## 15. 2026-11-16：服务端与 ESP32 优化（待用户构建/部署验证）

- 主固件瘦身第 1 项已由用户 Windows 构建确认：210 页（末页 24.41%），约 857064 字节，radio 余量约 23.6 KB（此前 4340）。
- 用户要求：助手**不用 emoji**（Flipper 像素屏画不出）。prompt 明令禁止；`connection.py` 对 LLM 输出 `strip_emoji`；发往设备的情绪改为按首句用词判断（`textUtils.guess_emotion`），ASR 情绪标签不再转成 emoji。
- 服务端（用户选“全部本地、鲸鱼娘、大胆删”）：
  - ASR 改 `SherpaASR`（sherpa-onnx SenseVoice int8，模型见 `models.lock.json`，ModelScope 固定版本），去掉 torch/funasr/modelscope 等大量依赖；旧 `FunASR` 配置自动迁移。旧的 `models/SenseVoiceSmall`（约 1 GB）可手动删除。
  - TTS 默认按句流水线（`tts/base.py` 的 `_pipeline_*`），按实测合成速度决定何时开播，慢机自动退回整段合成，不会断续；MeloTTS 加载时预热；LocalTTS `tts_timeout: 30`。
  - 人设鲸鱼娘（config.yaml / 两个示例 / `config/deepflipper_defaults.py` / `agent-base-prompt.txt`）；删除天气/位置联网、唤醒词回复、未用 provider/插件/性能测试；`config.yaml` 约 200 行。
  - 部署：先 `python scripts/download_models.py` 下载新 ASR 模型，再 `docker compose -f server/compose.local.yaml up -d --build` 重建镜像。
- ESP32：只保留 deepflipper-s3 与 en-US；输出改名 `deepflipper.bin`（脚本/测试已同步）；板级 `config.json` 加大 Wi-Fi/lwIP 缓冲与 TCP 窗口；Opus 解码优先级 2→3，audio_output 栈 4 KB；逐条 JSON 日志降为 debug；聆听/说话时锁最高 CPU 频率；Kconfig 菜单与 OTA 默认去小智。需用户用 `.\scripts\build_esp32.ps1` 构建并实机测：剩余内存、卡顿、待机功耗。
- 测试：原生 36 组、Python 103 项全部通过（新增 `test_tts_pipeline.py`、`test_no_emoji.py`、`test_config_migration.py`）。

## 16. 2026-11-17：更新完成页 + 两项调研

- 用户要求：固件更新成功后只要一页提示，不要 Unleashed 的 4 页（Logo／防骗／Discord／GitHub）。
  新脚本 `scripts/make_update_slide.py` 生成 `firmware/flipper/assets/slideshow/update_default/frame_00.png`（原始 64×64 鲸鱼娘 happy/00 + `DeepFlipper / Firmware updated! / OK▸`），并删除 frame_01..03。
  单帧幻灯片按 OK／右／返回即结束（`desktop_view_slideshow.c` 原逻辑，无需改代码）；打包为 675 字节，在资源里，不占主固件 Flash。预览 `assets/whale-pet/ui-preview/update-slide.png`；测试 `tests/test_update_slide.py`。需要 Windows `build_flipper.ps1 -Package` 重新出包后真机看。
- 仍未改：`first_start` 首次开机幻灯片（仍是海豚）、设置 → About 里的 `github.com/DarkFlippers` 字符串（`applications/settings/about/about.c:112`，在 Flash 内）。
- 调研结论（ESP32 控制 Flipper 的同类项目、BreezeBlue TTS／大肥鱼音色、RVC 唱歌）记在 [docs/research-2026-11.zh-CN.md](research-2026-11.zh-CN.md)。

### 16.1 BreezeBlue 云端语音（已实现，待用户 Docker 重建后实机听）

- 新 provider `server/main/xiaozhi-server/core/providers/tts/breeze.py`，配置块 `TTS.BreezeTTS`（`type: breeze`）。本机私有配置已选 `TTS: BreezeTTS`、`voice_id: clever`（大肥鱼·机灵；`dreamy` = 迷糊）。
  **API Key 在 `data/server.env` 的 `BREEZE_API_KEY`**（git 忽略，compose 的 env_file 注入），不要写进 config.yaml 或文档。
- 默认 `mode: realtime`：每次回复一个 `stream-input` WebSocket 回合，逐句 `text.append + text.flush`，结束 `turn.end`；服务端 TTFA 实测 150–210 ms，句间无停顿。
  `connection.chat` 在大模型开始思考时调用 `tts.prewarm()` 预连；连接 1.5 s 内没就绪则本次回复改用 HTTP。barge-in 发送 `turn.cancel`。空闲 120 s 关连接（期间 12 s 一次 JSON ping）。
- `mode: http`（或 `speed≠1.0` 时自动）：逐句流式 POST。**免费档账号只允许 1 个并发生成**（实测 429 "concurrent generation limit"），所以默认并发 1，排队的句子合并成一次请求，429 自动重试。
- 任一路径失败：本轮剩余句子改用本地 MeloTTS（模型存在时），云端暂停 60 s（遵守 Retry-After）。音量：realtime 不支持服务端 volume，本地乘 1.5 并限幅。
- 管理页面：合成方式多了 BreezeTTS 和音色下拉；试听走 HTTP 整段。
- 测试：`tests/test_breeze_tts.py` 14 项（含本地假 WebSocket 服务器的 realtime 测试）通过；`test_tts_pipeline` 等相关测试通过。沙箱到 api.breeze.blue 的 WebSocket 握手很不稳定（1.5–20 s），真实耗时以用户网络为准。
- 部署：`docker compose -f server/compose.local.yaml up -d --build`（requirements 没变，websockets 已在内）。注意本机 `data/.config.yaml` 里 DeepSeek 的 model_name / api_key 仍是占位符，需在管理页填写。

### 16.2 AI 像人一样操作（规划已被第 17 节实现取代）

- 规划见 [docs/ai-ui-control-plan.zh-CN.md](ai-ui-control-plan.zh-CN.md)。

## 17. 2026-11-18：AI 界面操作 + 多模态截图 + RVC 唱歌（当时实现，现状见第 20 节）

用户确认：照规划全部实现；工具轮数可以多；DeepSeek 用 `deepseek-flash`（官方文档确认 Chat Completions 支持 user/system/**tool** 消息里的 image_url，assistant 里不行）；有 RTX 4060 8 GB 做 RVC。

### 17.1 Flipper 主固件（需 `build_flipper.ps1 -Package`）

- `gui/canvas.c` + `canvas_i.h`：按需捕获一整帧里 `canvas_draw_str(_aligned)` 画的文字（坐标、u8g2 颜色：0 = 白字即反色高亮）；键盘逐字 `canvas_draw_glyph` 同行同色合并。默认关闭，只在请求时 arm 一帧；静态约 0.8 KB RAM。
- 新文件 `services/deepflipper/deepflipper_screen.c/.h`：companion JSON op `screen`（`bitmap` 可选，等待 GUI 重绘 ≤400 ms，位图走 `gui_add_framebuffer_callback`）与 `screen.page`（文字分页、位图 3 段×384 字节 base64）。`deepflipper.c` 处理时先释放服务锁；断开时释放缓存。
- **重要事实**：经 RPC（`flipper_app_open`）启动的应用保留 AI 串口；在主菜单/Archive 里本机按 OK 启动会走 `deepflipper_app_suspend_uart()` 断开 AI 链路，所以 AI 必须用 `flipper_app_open` 开应用（提示词和工具描述都写了）。
- 测试：`tests/screen_capture_test.c`（真实 deepflipper_screen.c + frozen，桩在 `tests/screen-stubs/`），已加入 `scripts/test_native.py`。

### 17.2 ESP32（需 `build_esp32.ps1`）

- 新 MCP 工具：`flipper_screen{bitmap}`、`flipper_press_keys{keys:"down,down,ok,back:long",delay_ms,bitmap,confirm}`（按完自动读屏）、`flipper_ui_select{text,confirm,bitmap}`（逐格移动高亮直到选中再按 OK，必要时滚动）、`flipper_go_home`。`flipper_app_open` 结果附带 app/view/selected。
- 输出：`app`（空 = 主页）、`view`（按行，高亮前加 ▶，底部按钮 `[◀ Back] [OK x] [Send ▶]`）、`selected`、`buttons`、可选 `bitmap`（1024 字节 u8g2 页布局 base64）。
- 危险门：按 OK/左/右 前读屏，标签整词匹配 send/tx/emulate/write/delete/erase/format/brute/wipe/jam/run/flash/reset → `needs_confirmation`（不按）。
- 新文件 `screen_view.h`；改 `native_actions.h`、`native_policy.h`、`deepflipper_bridge.*`（`Call()` 可设超时，读屏 1500 ms）、`deepflipper_board.cc`。测试 `tests/native_screen_test.inc`（被 native_actions_test.cpp 引入）。

### 17.3 服务端（需 Docker 重建）

- `max_tool_rounds: 24`（原写死 5）；`ui_progress_voice`：本轮首次界面工具且还没出声时先说"好，我来操作一下。"。
- 截图：`core/utils/flipper_screen.py` 把 bitmap 转 512×256 PNG（纯 zlib）；`vision: auto`（模型名含 deepseek-flash/vl/vision 等）时放进 tool 消息的 image_url，只保留最新一张；`conn.last_screen(_png)` 供管理页。
- 管理页"设备连接"页新增 Flipper 屏幕卡片（刷新=调用设备 `flipper_screen bitmap:true`）；"提示词与工具"页新增 `ui_confirm_dangerous` 开关。开关打开时服务端只在"上一轮设备返回 needs_confirmation、用户接着说话"的情况下放行模型的 `confirm:true`；关闭时自动带 confirm。
- 提示词 `config/deepflipper_defaults.py` 重写为正面操作指引，删掉免责话。
- **用户要做**：管理页把模型名改成 `deepseek-flash` 才能看截图。

### 17.4 RVC 唱歌

- 新目录 `singing/`（Windows 本机跑，用 GPU）：`setup_windows.ps1`、`worker.py`（aiohttp :8790，`X-Singing-Token`）、`pipeline.py`（截 90 s → BS-Roformer 分离 → 自动八度 → RVC `infer-rvc-python` → ffmpeg 混音 → 24 kHz WAV，缓存）、`make_voice_dataset.py`（用 BreezeBlue 大肥鱼声音生成约 11 分钟训练集，会先估算额度再确认）、`fetch_models.py`、`start_worker.ps1`、`README.zh-CN.md`（含 Applio 训练设置）。
- 服务端插件 `plugins_func/functions/sing_song.py`：`sing_song`/`list_songs`；有缓存直接唱，没缓存先回"我去练一下"、后台渲染完在设备空闲时主动开一轮唱；`singing.enabled` 控制加载（`plugin_executor.py` 改 3 行）。compose 加了 `extra_hosts: host.docker.internal:host-gateway`。
- **完全未在 Windows/GPU 上跑过**：依赖安装（numpy 2 + pyworld 0.3.5 --no-deps）、显存、耗时都待验证。

### 17.5 本轮验证

- Python：`unittest discover -s tests` 160 项通过（12 跳过），含 `test_flipper_ui_control`(21)、`test_singing_worker`/`test_singing_plugin`、`test_breeze_tts`、`test_admin_server`。
- 原生（沙箱用 gcc/g++ 代替 zig）：`native_actions_test`（含新屏幕/按键测试）、`screen_capture_test`、`companion_interop_test`、`rpc_wire_test` 通过。
- 未做：Flipper/ESP32 固件真实编译、真机、真实 DeepSeek 看图效果、GPU 唱歌流程。

### 17.6 已知风险

- 底部按钮靠"y≥59 且反色"识别；自己画按钮/不用反色高亮的第三方 App，`ui_select` 会返回 `no_highlight`，AI 改用 press_keys + 截图。
- 每翻一格读一次屏（≤400 ms），长列表搜索可能 10–20 s。
- 服务器默认 120 s 无语音断开，后台练好的歌多半等下次点歌才唱。


## 18. 2026-11-18：本机 RVC 唱歌落地（已完成）

用户提供 BreezeBlue API Key，要求接续 RVC 唱歌。Key 已仅保存到忽略的 `server/main/xiaozhi-server/data/server.env`，不要复制到文档、源码或日志。

### 已安装并验证

- RTX 4060 Laptop 8 GB，驱动 572.83；推理、训练两套环境均使用 PyTorch 2.11.0/cu128，真实 GPU 运算通过。
- 原 Miniconda Python 3.11.5 自带 VC runtime 14.27，导致 `c10.dll / WinError 1114`。已下载独立 Python 3.11.15 到 `.tools/python/`，只切换本项目的 `singing/venv` 和 `.tools/Applio/.venv`；原 Conda 未改。`setup_windows.ps1` 已增加自动处理。
- FFmpeg 9.0.2 已通过 winget 安装；另有 `.tools/ffmpeg/ffmpeg.exe`，Pipeline 和启动脚本能自动发现。
- 原依赖存在真实冲突：`infer-rvc-python` → `torchcrepe 0.0.20` → `librosa 0.9.1`，与 separator 的 `librosa>=0.10` 不兼容。现在由 uv + `dependency-overrides.txt` 覆盖为 `torchcrepe 0.0.24`、`librosa 0.11.0`、`pyworld 0.3.5`，各原生库导入和真实转换均通过。
- 公共模型已下载并核验官方 SHA-256：RMVPE、ContentVec、40k G/D 预训练、BS-Roformer 317 及 YAML；分离器所需的三份公共 JSON 也已准备。推理特征模型改为与训练一致的 ContentVec，仍放 `models/hubert_base/`。

### 已训练并部署

- BreezeBlue 大肥鱼·机灵真实请求成功，`singing/dataset/clever/0001.wav` 至 `0165.wav` 共 **12.6 分钟**、40 kHz 单声道 16 位。`audit.json` 检查无损坏、空音频或重复。
- Applio 3.7.1 源码在 `.tools/Applio`，提交 `6f69726b140c615c8cc6831a7db8b7c5e48ba8af`；训练环境独立。新增 `setup_training.ps1`、`requirements-training.txt`、`train_voice.py`，自动完成预处理、提取、训练、索引及部署。
- 362 个训练片段全部提取成功。**100 轮、9200 步训练完成**，batch size 4、FP16、`--cache-gpu`；首轮约 3 分钟，之后每轮约 25–28 秒，显存约 5.3GB。
- 成品：`singing/models/whale.pth`、`singing/models/whale.index`。RVC v2、40 kHz、单说话人、27,509,698 个推理参数；索引 768 维、36,968 个向量。最终模型严格加载、所有权重有限值、推理环境读取索引均通过。
- `singing/training/whale/deepflipper-status.json` 当前为 `stage: done`。日志 `deepflipper.log`，25/50/75/100 轮导出快照均保留；G/D 可恢复检查点为最新 100 轮。训练已结束，无需等待或重新执行。

### 真实渲染与服务对接

- 测试源：`singing/songs_in/小星星（哼唱测试）.wav`，由现有“啦”音节素材生成旋律和简单伴奏，约 12.7 秒；来源记录 `singing/training/validation/humming-demo.json`。
- **真实 GPU 全流程通过**：截取 0.1s、人声分离 5.4s、音高分析 0.3s、RVC 8.8s、混音 0.4s，总计 **15.1s**。输出 24 kHz 单声道 16 位 WAV；14 个音均被检测到，音高中位误差约 0.015 半音。再次请求已验证命中缓存。
- 可直接试听：`singing/cache/whale-twinkle-demo.wav`。worker 缓存 ID `aa5320edeef2b20f`。详细结果 `singing/training/validation/final-render.json`。25 轮无伴奏初版另存 `singing/training/validation/whale-25e-dry.wav`。
- worker 当前已通过 `start_worker.ps1 -Background` 在隐藏窗口运行，监听 8790，日志 `singing/worker.log`。此选项不创建开机任务；下次开机可再次运行 `powershell -ExecutionPolicy Bypass -File singing/start_worker.ps1 -Background`。
- Docker voice 已重建并启动，共享口令已同步。真实验证：容器到 `host.docker.internal:8790` 鉴权返回 200；实际翻唱已下载到服务端 `data/songs/aa5320edeef2b20f.wav`，开场白→动作→音频文件的播放队列验证通过（模拟设备连接）。尚未做设备扬声器的实际试听。
- 25 项唱歌相关测试在服务端 Docker 环境全部通过；Python 语法、PowerShell 解析、私有配置和模型/音频的 Git 忽略检查通过。主 README、唱歌 README 已更新。

### 当前点歌配置与待办

- **DeepSeek 已接通（2026-11-18 21:35）**：用户补充 Key，已仅写入忽略的 `data/.config.yaml` 的 `LLM.DeepSeekLLM.api_key`。真实 `/models` 验证可用模型为 `deepseek-flash`、`deepseek-v4-pro`，本项目选择前者；Docker voice 已重启。实际通过项目 LLM provider 流式调用，正确得到 `sing_song` 工具请求，参数歌名为“陈楚生 庙堂之外”。不要把 Key 写入文档或源码。
- **用户第一首陈楚生《庙堂之外》已制作完成（2026-11-18 21:47）**：用户提供 桌面上的 `庙堂之外.mp3`，已复制到 `singing/songs_in/陈楚生 - 庙堂之外.mp3`，原文件保留。当前曲库两首：这首正式歌曲、《小星星（哼唱测试）》。
- 《庙堂之外》默认 90 秒版：取原曲 85–175 秒，自动升 12 半音；GPU 全流程 40.7 秒。缓存 ID `ddd542d81552242c`，试听 `singing/cache/miaotangzhiwai-whale-preview.wav`；已进入 Docker 服务端文件缓存，开场白/动作/WAV 播放队列检查通过。
- 《庙堂之外》完整版：282.19 秒（约 4 分 42 秒），自动升 12 半音；GPU 全流程 118.1 秒。缓存 ID `0b77f90df2d9bff2`，试听 `singing/cache/miaotangzhiwai-whale-full.wav`。两版均为 24 kHz 单声道 16 位 WAV，音频长度/音量检查通过、无削波，重复请求命中缓存；报告在 `singing/training/validation/miaotangzhiwai-preview.json` 和 `miaotangzhiwai-full.json`。
- 设备当前 `sing_song` 默认仍请求 90 秒片段；完整版本已提供为本机试听文件及 worker 的 `clip_seconds: 0` 缓存，没有改变全局点歌默认长度。完整步骤见 `singing/README.zh-CN.md`。
- 临时下载分块清理被自动策略检查拦截，未执行清理；分块仍在忽略的 `tmp/wheels/`、`tmp/model-pieces/`，不影响使用。完整 PyTorch wheel 也保留在 `tmp/wheels/`。



## 19. 2026-11-18：宠物数值反馈符号修复（代码、测试、应用编译完成，待安装）

用户真机反馈：Play 时精力下降，但数值条旁仍显示加号。确认是显示错误：模型本来就消耗 5 精力、开心最多 +12，UI 却把 Play 和 Catch 的精力、开心都标为正向；绘制函数也只有加号。

- `pet_ui.c/.h`：照料和游戏结算改为比较操作前快照与成功保存后的实际数值；`flash` 表示发生变化的数值，`flash_decrease` 标记其中的减少项。`whale_pet.c` 按方向画加／减号。养成数值、冷却与存档格式未改变。
- Play、Catch：精力消耗显示减号，开心真正增加才显示加号。Feed 的 +6 精力现在也能正确提示；饱腹、精力、开心已到上限而未变化时不再闪假加号。冷却、失败保存清除旧反馈；反馈在小游戏页面也能按时结束。
- Catch 结果文字同样检查实际开心／经验增量，满值时不再虚报对应奖励。
- Windows 原生回归通过：`pet_model_test`、`pet_store_test`、`pet_ui_test`（含 Play/Catch 方向、Feed 精力、满值、冷却、保存失败、小游戏反馈过期）、`pet_render_test`（直接验证生产绘图里精力减号和开心加号的像素）。
- 本机真实构建 `fbt.cmd fap_whale_pet DEBUG=0 -j8` 成功，API **88.25**，FAP **42,332 字节**。没有编译／刷写整套 Flipper 或 ESP32 固件。
- 更新包：`build/release/pet-feedback-fix/WhalePet-feedback-fix.zip`（24,580 字节），包含 `apps/Games/whale_pet.fap` 和匹配 FAP MD5 的 `deepflipper/apps/deepflipper.pet.json`，以及安装说明、SHA256SUMS；不含宠物存档或帧素材。压缩包完整性和登记文件哈希核验通过。
- 安装：先退出 Whale Pet，将包内 `apps/`、`deepflipper/` 合并覆盖到 SD 根目录，再打开宠物 App。要求已有匹配 SDK 88.25 的 DeepFlipper 固件。本轮尚未安装到实际设备。
- 实际生产绘图预览：`build/release/pet-feedback-fix/play-feedback-128x64.png`、`play-feedback.png`（6 倍，原尺寸像素放大）。角色与房间素材未改。
- 修复 ZIP SHA-256：`0f43957393529d21539b518eb805c0af20b2694a7a5517b67cd9236be7780d1e`。


## 20. 2026-11-19：点歌缓存、助手设置掉线与两端固件交接

### 点歌与设备播放

- 2026-11-18 22:37–22:38，用户多次点《庙堂之外》。请求进入普通对话，同一时段翻唱 worker 没收到这些请求。回复里的“要练一下”“等曲库回调”没有工具结果支撑。当时 90 秒版已在服务端缓存，本次未播放发生在点歌路由。
- 已在 `core/connection.py` 把明确的点歌指令直接交给 `sing_song`。带额外条件的请求仍可由模型调用工具，提示词要求按工具结果报告状态。Docker 语音服务已重建并达到 `ready`。用“给我唱一首庙堂之外”验证命中 `ddd542d81552242c.wav`，生成 1500 个 60 ms Opus 帧。该轮 26 项唱歌相关测试通过。 [点歌入口](../server/main/xiaozhi-server/core/connection.py) · [唱歌插件](../server/main/xiaozhi-server/plugins_func/functions/sing_song.py)
- ESP32 边收边播，默认点歌返回 **90 秒**。完整版 **282.19 秒**保存在 worker，缓存 ID 为 `0b77f90df2d9bff2`，可以用 `clip_seconds: 0` 请求。完整音频已验证可编码为 4704 个 Opus 帧。用户已在设备上播过《小星星》，《庙堂之外》的默认片段和完整版仍需确认设备实际播到了结尾。 [曲库与复用方法](../singing/README.zh-CN.md)

### 助手设置与通信

- 旧版 Flipper 把 **AI Assistant 设置页**当作普通本机 App 启动，Loader 会暂停 UART，所以即使进设置前语音正常，进去仍会看到 `UART: offline`。打开主菜单本身没有这一步。已修改 Loader 和 DeepFlipper 服务，进入助手设置时保留 UART，设置页内重新启用 AI 也会启动 UART。普通本机 App 仍按原规则释放 UART。 [Loader](../firmware/flipper/applications/services/loader/loader.c) · [DeepFlipper 服务](../firmware/flipper/applications/services/deepflipper/deepflipper.c)
- 2026-11-18 22:33 的日志显示，工具 App 曾打开并读到屏幕。随后 22:33:37 请求打开 Sub-GHz，22:33:48 语音连接断开，22:34:07 工具超时。语音日志只能确认 ESP32 ↔ 服务端断线。这次断线的触发原因仍需结合 ESP32 串口日志确认。
- ESP32 现在允许 UART 在约 **5 秒**内恢复，并在这段时间保留语音会话。AI 通过 `flipper_app_open` 启动 App 时，串口应答上限由 5 秒增至 **10 秒**，仍受任务总期限约束。当轮 Windows 原生测试全套通过，覆盖设置页保留 UART、掉线宽限、读屏和音频队列。 [ESP32 桥接](../firmware/esp32/main/boards/deepflipper-s3/deepflipper_bridge.cc) · [恢复窗口](../firmware/esp32/main/boards/deepflipper-s3/companion_updates.h)

### 固件产物

| 产物 | 本机构建记录（北京时间） | 重建命令 |
|---|---|---|
| [Flipper 完整更新包](../build/release/flipper/flipper-z-f7-update-local.tgz) | 2026-11-18 23:03，SDK 88.25，含宠物 Play／Catch 反馈修复 | `.\scripts\build_flipper.ps1 -Package` |
| [ESP32 应用固件](../build/release/esp32/deepflipper.bin) | 2026-11-18 23:10，ESP-IDF v6.1.1 | `.\scripts\build_esp32.ps1 -Idf v6.1.1` |

本机安装的是 ESP-IDF v6.1.1，重建时显式指定版本。ESP32 已有安装且分区布局和启动槽符合要求时，可按说明只刷应用以保留 Wi-Fi 与设备身份。首次安装或分区布局不同时使用完整镜像。 [Flipper 安装说明](flipper-update.zh-CN.md) · [ESP32 刷写说明](../build/release/esp32/FLASHING.txt)

### 真机验证顺序

目前两端固件均有本机构建产物，尚无这批固件的刷入或真机验收记录。

1. 现有语音设备可先说“给我唱一首庙堂之外”，观察服务端是否走点歌工具并命中缓存。用《小星星》确认短音频播放可用。
2. 安装匹配的两端固件后，在 Flipper 主页确认语音可用，打开主菜单再进入 AI Assistant 设置，检查 `UART` 持续在线。回到主页进入宠物 App，确认 Play／Catch 消耗精力时显示减号。
3. 让助手打开已知可用的工具 App 后连续读屏，再复测打开 Sub-GHz 的场景。若出现断线，记下 App、操作步骤及北京时间，对照 ESP32 串口的 UART／Wi-Fi 断开原因与服务端语音日志。
4. 点《庙堂之外》并确认默认版能连续播完 90 秒。再验证完整版的 282.19 秒播放，记录是否卡顿、被其他语音打断或提前停止。

## 21. 2026-11-19：5V 被 App 切断、宠物页保持连接、点歌改为“练好后询问”、About 页

### Flipper 与 ESP32 的连接规则（用户确认）

- **AI 用 `flipper_app_open` 打开的 App**：调用者是 RPC 会话线程，Loader 记为 `keep_rpc`，UART 保持，AI 可以 `flipper_screen` 读屏、`flipper_press_keys`／`flipper_ui_select` 按键。这是“截图 + 操作”的唯一入口，提示词和工具描述已要求 AI 这样开 App。
- **用户手动从菜单打开的 App**：仍然断开 UART（`deepflipper_app_suspend_uart`）。原因不是省电，而是给大 App 腾堆内存（RPC 会话线程 3 KB 栈 + 1 KB 流缓冲 + Expansion 两个 768 B 线程等，以及 WhaleTools 3 KB 栈），并避免和自己要用 13/14 脚串口的 App 冲突。ESP32 在 UART 断开约 5 秒后暂停语音。用户确认保留这个规则。
- **例外（不断开）**：长按 OK 进主菜单、菜单里的 Settings 列表（二者都在 Loader 菜单内，本来就不经过启动流程）、AI Assistant 设置页、**Whale Pet（主页右键或 Apps → Games 打开 `/ext|/any/apps/Games/whale_pet.fap`）**。新增 `loader_deepflipper_keeps_link()`，`assistant_memory_test` 覆盖路径匹配。

### 打开 Sub-GHz 后语音掉线的原因：5V 被 App 关掉

- ESP32 模块由 Flipper 顶部 1 脚 5V（OTG 升压）供电。Sub-GHz 启动时默认探测外置 CC1101：`subghz_txrx_radio_device_set()` 先开 OTG，探测失败后调用 `subghz_txrx_radio_device_power_off()` → `furi_hal_power_disable_otg()`，**直接把 5V 关掉**；空闲时每 5 秒再探测一次。电源服务随后又按 `is_otg_requested` 重新打开，ESP32 因此掉电重启，语音和 UART 一起断。这与 2026-11-18 22:33 “打开 Sub-GHz 后 11 秒语音断开、工具超时”一致（尚需真机确认）。
- 同类调用还有：iButton 读/写结束 `power_enable_otg(false)`（会把请求也清掉，5V 一直不回来）、GPIO 5V 开关、RPC GPIO、若干 Sub-GHz 系 FAP 的 `radio_device_loader`。
- 修复：`targets/f7/furi_hal/furi_hal_power_i.h`（`_i.h` 不进 SDK，不改 API 版本）新增 `furi_hal_power_otg_hold()`。AI 开启时 `deepflipper_set_enabled(true)` 持有，关闭 AI 时释放；持有期间 `furi_hal_power_disable_otg()` 不关 5V，电源服务的 SwitchOTG(false) 保留请求。OTG 过流故障时先释放再关闭，安全优先。新增原生测试 `tests/otg_hold_test.c`（直接编译生产代码）。
- 副作用：AI 开启时，GPIO 菜单里的“5V 关闭”不会真正断电，这是有意的。

### 点歌

- worker：`songs_dirs` 多个原曲目录（含 3 层子文件夹、同名取先列出的目录）、`download_dir`、`cache_dir` 可配置；`GET/PUT /config` 保存到 `singing/config.local.yaml`；`POST /download` + `GET /downloads/<id>` 用 yt-dlp 下载单首音频（≤20 分钟、≤300 MB、不转码、拒绝播放列表、同名加“ (2)”），下载后按**具体文件**提交练习，不再按歌名模糊匹配。requirements 加 `yt-dlp`。
- 插件：没练过的歌回复“《X》我还没练过，得先练一下……练好了我叫你”；练习期间 `conn.singing_hold_until` 让 `no_voice_close_connect` 和 `_check_timeout` 不断开；练好后等设备空闲 3 秒，主动说“《X》练好啦，要不要现在听？”，在 `_handle_direct_song` 里先识别回答（要／好呀／唱吧 → 直接播放缓存；先不听／算了 → 留着；说别的 → 不当回答，记入“刚练好”）。设备断开时保留 24 小时，下次连上、用户说完第一句话后再问（`helloHandle` → `greet_pending`）。`sing_song` 新增 `url` 参数。
- 管理页新增“唱歌”标签：服务状态、三个路径（保存即生效）、链接下载并显示进度、曲库列表；有设备连着时，管理页添加的歌练好后同样由鲸鱼娘询问。
- 测试：`test_singing_worker` 11 项、`test_singing_plugin` 16 项、`test_admin_server` 19 项（含唱歌代理），全量 Python 173 项、原生 `scripts/test_native.py` 全部通过（Linux 沙箱；未做 Flipper 固件真实构建）。

### 固件升级卡在“只有电池条”

- 用户用 qFlipper 安装 tgz；卡住后按 返回+左 重启即可进入 UPDATING 并完成升级。代码排查范围：qFlipper 的 `system_reboot(UPDATE)` → 电源服务 `power_handle_reboot()`；PreUpdate 开机后 `updater_start_app()`（定时器线程）→ Loader → `loader_prepare_deepflipper()`；更新器界面是全屏，会画 “UPDATING”，所以“只有电池条”的画面不是更新器，而是普通固件的窗口层。**没有找到确定原因，本轮未改这部分代码。**
- 下次复现时请勿立即重启：关闭 qFlipper，用串口终端（或 lab.flipper.net 的 CLI）连上 Flipper，执行 `ps`、`loader info`、`free`，再执行 `log debug` 观察几秒，把输出保存下来；同时记下是在点 Install 之后、还是 UPDATING 进行到某一步之后卡住。

### About

- 设置 → About 的三页 Unleashed 文案换成一页 “DeepFlipper / AI companion firmware / Based on Unleashed and Flipper Zero firmware / Open source, GPLv3”。后面的产品、合规、硬件和固件版本页不变。

### 需要用户做

1. `.\scripts\build_flipper.ps1 -Package`，安装后验证：AI 开启时打开 Sub-GHz 不再掉线；主页右键进宠物语音不断；About 第一页。
2. ESP32 本轮没有改动。
3. 唱歌：`singing\venv\Scripts\python -m pip install yt-dlp`，重启 worker；`docker compose -f server/compose.local.yaml up -d --build` 重建语音服务。

## 22. 2026-11-19 晚：网易云曲库、说话音量与语气、唱歌动画草图

- 用户已在管理页把 本机的网易云音乐下载目录 加为第二个原曲目录（`singing/config.local.yaml`），实测点《这世界那么多人》→ 练 107.7 s → 主动询问 → “唱吧” 播放，整条流程在真机走通。曲库 502 个文件全是普通 MP3/M4A（没有加密 .ncm），`VipSongsDownload` 为空；有 1 个文件头全零（《考试什么的都去死吧》），可能已损坏。
- 匹配改进：`(高品质)`、`[mqms2]`、`(1)` 这类标记在比较时去掉（worker 与插件两份规则同步）；同一首歌在两个目录里同样像时，优先已经练好的那份，其次按目录顺序，避免换目录后重新练。worker 启动和改路径后在后台预扫一遍曲库，大曲库第一次点歌不再超时。
- “一惊一乍”（用户选：音量忽大忽小、措辞太激动）：
  - 原因：BreezeTTS 实时模式把每块 PCM 固定 ×1.5 再硬削波，响的字被削成爆音；HTTP 模式靠服务端 volume 1.5；本地兜底 MeloTTS 归一化到 -20 dBFS，三者响度不一致。
  - 新增 `core/utils/speech_level.StreamLeveler`：按最近语音响度（静音不计）慢慢把增益调向 -20 dBFS（和本地合成一致），增益在每 20 ms 内渐变，峰值过 -6 dBFS 后软膝限幅、不超过 -1 dBFS。每个连接一个实例，句与句、轮与轮之间增益延续。实时与 HTTP 路径都用它，HTTP 请求的 volume 改为 1.0。`TTS.BreezeTTS.level: false` 可恢复旧行为。
  - 语气：BreezeTTS 默认 `instructions` 改为“语气平稳自然、温和亲切……不要惊呼”；提示词“活泼、喜欢用嗯嗯好呀嘿嘿”改为“安静、平和自然”，并加“少用感叹号和语气词”。三处同步：`config.yaml`、`config/deepflipper_defaults.py`、用户的 `data/.config.yaml`。
  - 测试：`test_speech_level` 新增 4 项（拉平、无削波、静音不抬增益、分块一致），全量 Python 178 项通过。需要重建语音服务后实听。
- 唱歌动画：在原画上叠加麦克风会和头发衣服糊在一起；用户选择“新画唱歌帧”。草图按现有动画同一套方式在原 64×64 角色上分层绘制（不改原图文件）：抬手握麦、眯眼、嘴型变化、身体左右晃 1 px、尾巴摆、头顶飘音符。草图在会话输出里（`sing-sketch-*.gif/png`），**等用户确认后**再加入 `make_character_animations.py`、导出 `home_frames.bin`，并打通服务端→ESP32→Flipper 的“练歌中/唱歌中”状态。

## 23. 2026-11-20：唱歌动画进固件、连续对话、对话记录带截图和操作、高音与断音

### 唱歌动画（用户确认草图，要求嘴型放慢、加睁眼）
- `make_character_animations.py` 新增第 13 组 `sing`（32 帧 × 120 ms）：抬手握麦、嘴型每 240 ms 换一次、大部分时间睁眼，中间眨一次眼、眯眼笑一段；身体每 480 ms 左右晃 1 px，尾巴摆，头顶两颗音符慢慢飘。原有 12 组动画逐字节不变；`whale_frames.bin` 129→153 帧，`home_frames.bin` 264192→313344 字节（SD 资源，不占内部 Flash）。在沙箱里跑脚本时中文字体不同，所以只保留了数据文件和 `frames/sing`、`sing_*.gif`，其余预览图恢复原样；Passport 图像素未变，已还原原文件。
- Flipper：`deepflipper_sprite.c` 中表情 12 = 唱歌，空闲或说话状态下显示，并且不叠加实时嘴型；听/想的状态仍优先。`deepflipper.c` 接受 emotion 0–12。`home_sprite_test` 增加对应断言。
- ESP32：`SetEmotion("singing"/"practicing")` → 12。
- 服务端：歌声开始时（和音频同一发送队列）发 `emotion: singing`；后台练歌期间设备空闲时每 3 秒发 `practicing`，练完发 `neutral`。

### Sub-GHz“说开始录制但没按”
- 容器日志不在本机，无法确认当时是模型没调用按键，还是调用被拦。提示词加了两条：说“我按了/开始录制了”之前本轮必须真的调用过按键且 view 变化；Sub-GHz Read RAW 的 REC/OK 不是发送，用户要录就在同一轮按下去，再提醒按遥控器。原来“录制前先提示用户”的说法改成“开始后再提示”。
- 以后能在对话记录里直接看到每一步（见下）。

### 对话记录
- `conversation_history` 新增 `tool` 事件：工具名、筛选过的参数（丢弃 token/key 类字段）、成功/错误码/needs_confirmation、屏幕 app/view/selected，以及截图（`data/history_images/<随机名>.png`，最多 400 张，清空记录时一起删）。`/api/history/image` 只认 32 位十六进制文件名。模型上下文仍只回放用户和助手的文字。
- 按键类工具（press_keys、ui_select）默认自动带 `bitmap:true`，每一步都有截图（`history_screenshots: false` 可关；每步多读约 1 KB 屏幕）。
- 管理页“对话记录”改成聊天视图：你在右、鲸鱼娘在左，操作是带状态标签的卡片（完成/等待确认/错误码），截图像素放大显示，屏幕文字可展开；录音/播放类状态默认隐藏，有开关；不同语音会话之间有分隔线。

### 连续对话（开关在管理页“提示词与工具”，默认关）
- 调研：ElatoAI 用服务端 VAD 判断说完、设备一直开麦、单次会话最长约 20 分钟，ESP32 端不支持打断。小智固件本身有 AutoStop（服务端 VAD 结束一句，回答完自动再听）和 Realtime（要 AEC）两种模式；本设备没有 AEC，所以采用 AutoStop + 跟进窗口。
- 服务端 hello 带 `continuous: true` → 设备按一次键用 auto 模式（不用再按第二次）；每次回答的 `tts stop` 带 `listen: "auto"` → 播完接着听；服务端在 `continuous_follow_up_s`（默认 8 秒，3–30）内没听到说话就发 `{"type":"listen","state":"stop"}`，设备安静回到待机，不识别、不道别，连接不断。
- 即使不开连续对话，鲸鱼娘问“练好啦，要不要现在听？”时也会自动听一次回答。
- 改动：ESP32 `protocol.h`、`websocket_protocol.cc`、`application.cc`；服务端 `core/utils/continuous_chat.py`、hello/listen/tts stop 三处、管理页开关；测试 `test_continuous_chat.py`。MQTT 协议路径没有加 continuous（本项目用 WebSocket）。

### 唱高音发哑、播放会断
- 原因：音色模型只用约 12 分钟说话素材训练，实测素材基频 5%/50%/95%/99% 约 212/290/424/545 Hz。旧的自动移调只看中位音高、按整八度移：男声歌 +12 后副歌常到 600–700 Hz，超出模型见过的范围，于是发虚、发哑。
- 新的 `pitch_mode: fit`（默认）：看整段人声的第 10/50/95 百分位，从 -12..+12 半音里选一个让高音不超过 `max_f0_hz`(480)、低音不低于 `min_f0_hz`(200)、中位接近 330 Hz；能整八度就整八度，否则伴奏一起移（实际只移与人声相差整八度的余数，变化最小）。例：男声歌（130/185/300 Hz）从 +12 变成 +8。缓存键包含新参数，**已练过的歌下次点会重新练一次**。旧做法 `pitch_mode: octave`。
- 根本改善要靠训练素材里加哼唱/长音；这次没有重新训练。
- 断音：设备解码队列原来最多 1.2 s，服务端领先发送约 0.84 s，Wi-Fi 卡顿超过这个就断。现在歌曲播放时领先量提到 `singing.song_lead_ms`（默认 650 ms，总领先约 1.1 s，旧 ESP32 固件也安全）；ESP32 解码队列提到 3 s，更新 ESP32 后可把 `song_lead_ms` 调到 2000。设备网页的“对话音频诊断”里 starvation 次数可确认是不是网络问题。

### 语气（续第 22 节）
- 用户这轮没反馈，响度拉平和语气提示保持。

### 测试
- Python 全量 186 项、原生 `test_native.py` 全部通过、管理页 UI 测试通过（Linux 沙箱）。Flipper 与 ESP32 都需要 Windows 重新构建：Flipper（动画、表情 12、5V 保持、宠物页、About），ESP32（连续对话、表情映射、3 s 解码队列）。
