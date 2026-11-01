# 横屏鲸鱼娘动画素材

当前选定的64×64单色全身角色，保留围裙鲸鱼与尾巴；用于自定义Unleashed系统桌面。项目行为以[最终规格](../../docs/final-spec.zh-CN.md)为准。

## 内容

- `source/accepted-screen.png`：选定造型的128×64原始基准；左侧64×64作为角色底图，右侧只是生成器输入，不是当前产品界面。
- `frames/<state>/NN.png`：12组动画、228张顺序帧，每张64×64、1-bit。
- `whale_frames.bin`：129张去重帧，66048字节。
- `manifest.json`：动画索引、帧时长、嘴型索引和源图路径。
- `whale_animations.h`：相同数据的C数组，可供`canvas_draw_xbm()`使用。
- `mouth_*.png`：6张完整角色嘴型参考帧，不是透明贴片。
- `passport_preview.png`：宠物详情页头像预览，昵称／等级／经验值为示例。
- `passport_happy.png`、`passport_okay.png`、`passport_bad.png`：46×49原生像素人物胸像，包含领口、肩部，不单独放鲸鱼标志。
- `desktop_preview.png`：当前左侧角色／右侧状态布局放大预览。
- `desktop_states.png`：待机、语音、连接、配置、工具、关闭、静音等12种桌面状态总览。
- `animated_characters.gif`、`animated_overview.gif`、`expressions.png`：角色和横屏状态总览。
- `conversation_demo.gif`、`conversation_native_128x64.gif`：顺序交互演示，前者放大、后者原生黑白。
- 每组另有`<state>_native.gif`与`<state>_screen.gif`。
- `whale-girl-assets.zip`：本地生成的分发包，不再提交到源码仓库；源图、序列帧和预览仍保留在本目录，生成脚本位于项目的 `scripts/` 中。

12组：idle待机、listening倾听、thinking思考、speaking说话、happy开心、shy害羞、surprised惊讶、wave挥手、sad难过、angry生气、sleeping困倦、nod点头。

所有GIF只是预览。桌面左侧按原始64×64显示角色，右侧显示时间、电量、AI状态、音量和麦克风状态；预览数值为示例值，右栏粗体状态和小字提示均直接解码固件字体，底部音量／麦克风标签和值分别左／右对齐；关闭或断连时不展示缓存值，**不显示对话字幕**。嘴型为预设节奏。橙色为背光模拟，原生像素只有黑白。固件无需解码GIF，也不应把当前预览当成实际系统桌面截图。

## 数据使用

每帧512字节，逐行排列，每行8字节，字节内最低位对应左侧像素，黑色为1。第N帧偏移`N * 512`，N来自manifest动画索引。每帧120ms，约8.3fps；挥手、点头等接入时可按一次动作播放。

```c
const WhaleAnimation* a = &whale_animations[0];
size_t step = (elapsed_ms / a->tick_ms) % a->count;
canvas_draw_xbm(canvas, 0, 0, 64, 64, whale_frames[a->frames[step]]);
```

宠物详情页由素材导出脚本更新Passport的九个心情／等级图标槽，使用原生像素头像；三个等级保留同一造型，等级／经验数据照常由原系统计算。Passport是SD应用，因此需要完整资源更新包。

系统正式播放使用SD文件与少量帧缓存。示例C数组是数据导出和验证用途，不能据此推定全套应放进常驻RAM。真实音频驱动嘴型时，将眼睛、嘴部与位移分层；现有完整参考帧不可直接覆盖其他姿态。嘴部基准约在`(25,33)`、5×4像素，起伏／点头时锚点随之变化。

## 生成与验证

项目根目录执行`python scripts/make_character_animations.py`，依赖Pillow及Windows微软雅黑字体。生成器依赖本目录的基准图、固件电池图标和u8g2字体数据，不再依赖旧草稿。微软雅黑仅用于屏幕外的预览说明标题；屏幕内英文使用固件字体，不依赖该主机字体。

数据检查包含帧尺寸／位深、manifest与二进制／C数组一致、围裙鲸鱼每帧保持、首张待机帧与选定造型一致、GIF尺寸／时长与循环。尚未进行实机刷新、功耗、字形和系统资源验证。
