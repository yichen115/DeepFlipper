# 创建 GitHub Release

源码放 Git，编译固件放 **Release Assets**。不要 `git add -f build/`，不要提交 SDK、
模型、服务器私有配置或录音。GitHub 自动提供的 Source code ZIP 只是源码，不含固件和下载依赖。

## 准备对应版本

1. 检查改动、测试、提交源码。确认没有凭据或模型进入 Git。
2. 在该提交上重新构建两端，避免源码提交与附件不一致：

```powershell
.\scripts\build_flipper.ps1 -Package
.\scripts\build_esp32.ps1
python scripts/validate_project.py
python scripts/package_release.py --version v0.1.0-preview.1
```

构建环境和测试命令见 [主 README](../README.md)。验证脚本还依赖项目测试日志，首次发布前
应先按 README 跑测试。不能只拿旧日志作为新版本测试结果。

打包脚本默认拒绝未提交改动；本地检查可使用 `--allow-dirty`，会在发布说明和
`SOURCE_STATE.json` 标记预览。**当前本机准备的 `v0.1.0-preview.1` 是此类本地预览，
未创建远端 Tag／Release，不是正式发布。**正式发布时先提交并重新构建，再选择新版本名打包。

脚本不重新编译，只核对导出文件、完整镜像组件、空白 NVS、Flipper TGZ 和验证报告哈希。
源码指纹在打包时采集，不能证明旧固件来自当前源码；因此提交后重建仍不可省略。
输出目录已存在时拒绝覆盖，可用新版本名避免混淆。

## 上传附件

本地输出在 `build/github-release/版本号/`：

| 文件 | 内容 |
|---|---|
| `DeepFlipper-版本号-flipper-f7.tgz` | qFlipper 更新包，包含 SD 动画和采集 FAP |
| `DeepFlipper-版本号-esp32s3-n16r8.zip` | 完整镜像、应用镜像、分散组件、烧录参数和中文说明 |
| `SOURCE_STATE.json` | HEAD、dirty、打包时间、源码指纹和产物哈希；无本机 SDK 路径 |
| `SHA256SUMS.txt` | 其余四个附件的 SHA-256 |
| `RELEASE_NOTES.md` | 可直接粘贴的中文发布说明 |

ESP32 使用白名单打包，不递归收集构建目录；不附 ELF、日志和 NVS 读回。
发布前用本机设备验证对话、屏幕联动、配网和所需工具，并在说明中写清实测范围。

网页操作：

1. 将准备好的源码提交推送至 [DeepFlipper](https://github.com/yichen115/DeepFlipper)。
2. **Releases → Draft a new release → Choose a tag**，如 `v0.1.0-preview.1`。
3. 新建 Tag 时选择对应的源码提交，勿误选后续无关改动。
4. 标题填 `DeepFlipper v0.1.0-preview.1`，粘贴生成的 `RELEASE_NOTES.md`。
5. 拖入上述五个附件；开发阶段勾选 **Set as a pre-release**。
6. 先 **Save draft** 检查附件和说明，确认后 **Publish release**。

只创建 Release 不会上传本机源码改动，源码必须先提交／推送。

## 可选：GitHub CLI

安装并登录 `gh` 后，可在源码提交已推送、版本确定时执行。下面命令会写远端，
本次文档整理没有执行这些操作：

```powershell
git tag -a v0.1.0-preview.1 -m "DeepFlipper v0.1.0-preview.1"
git push origin v0.1.0-preview.1
$releaseFiles = Get-ChildItem -File build/github-release/v0.1.0-preview.1 | ForEach-Object FullName
gh release create v0.1.0-preview.1 @releaseFiles --draft --prerelease --verify-tag --title "DeepFlipper v0.1.0-preview.1" --notes-file build/github-release/v0.1.0-preview.1/RELEASE_NOTES.md
```

先检查网页中的草稿；确认后可在网页发布。不要覆盖已发布版本的固件而沿用旧校验值，
应递增版本重新打包。

## 下载后的安装与校验

PowerShell 查看校验值：

```powershell
Get-FileHash .\DeepFlipper-v0.1.0-preview.1-flipper-f7.tgz -Algorithm SHA256
Get-FileHash .\DeepFlipper-v0.1.0-preview.1-esp32s3-n16r8.zip -Algorithm SHA256
```

Linux 在附件目录中可执行 `sha256sum -c SHA256SUMS.txt`。
TGZ 直接交给 qFlipper；ESP32 ZIP 先解压，按包内中文说明烧录。
完整 `merged-binary.bin` 在 **0x0**，会覆盖配置和身份；应用 `xiaozhi.bin` 在 **0x20000**，
仅在相同分区、初始 ota_0 槽时使用，不全片擦除可保留 NVS。
更换完整镜像后重新配网、导出新身份并在服务端重新登记。
