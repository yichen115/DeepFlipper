# 宠物表情素材

沿用 `../whale-girl/source/accepted-screen.png` 的 64×64 单色角色，逐像素绘制表情和动作。
最喜欢的食物是白米饭；`source/favorite-rice.png` 是用户提供的性格／饭碗参考。

- `expressions.png`：19 组表情总览，含原生 64×64 小图和开饭过程。
- `<动作>.gif`：动作放大预览，橙色仅模拟 LCD 背光。
- `pet_frames.bin`：237 张去重帧，121344 字节，LSB-first、black=1。
- `pet_masks.bin`：同顺序的不透明遮罩（1 = 不透明，含脸／衣服白色和 1 像素白边），121344 字节；
  App 在带花纹的房间上先画遮罩再画墨线，角色不会和背景糊在一起。
- `manifest.json`：全部帧序列、时长、循环和食物偏好。
- `app/`：**由生产绘制代码渲染**的宠物 App 画面（`tests/pet_render_test.c` 用固件字体光栅化 `whale_pet.c` 的绘制函数），`overview.png` 为总览。
- `home/`：**由生产主页渲染代码输出**的主页画面（时间、电量、气泡都由固件画进帧里）。
- `ui-preview/`：改版前给用户确认用的设计草图与动画（含未采用的方案 A2、中文标签对比），不是固件输出。
- `frames/`：本机生成的逐帧 PNG，未提交，可随时重建。

运行 `python scripts/make_pet_assets.py` 重建帧、遮罩、`pet_art.h`／`pet_clips.h`（房间、图标）。
运行 `uv run --with ziglang python scripts/test_native.py` 后，再运行
`python scripts/render_home_preview.py` 与 `python scripts/render_pet_preview.py` 更新 `home/`、`app/`。
预览不是实机录像。使用方法与边界见 [宠物系统](../../docs/pet-system.zh-CN.md)。
