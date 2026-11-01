# Flipper 更新与上传失败处理

更新期间先在 Flipper 的 **Settings → AI Assistant** 将 **AI session** 设为 OFF，
并关闭／断开 ESP32 模块，再使用 qFlipper 的 **Install from file** 安装完整 TGZ。
这能避免旧固件同时维持 ESP32 UART、USB RPC 和屏幕投送；新固件的优化只有安装后才生效。

## 上传阶段重启

2026-11-01 本机 qFlipper 日志中，两次失败都发生在
`Storage Write /ext/update/f7-update-local/resources.ths`，随后 USB 丢失。
没有进入系统更新／重启到刷写器的步骤。包内 updater、radio 的 CRC 和 TGZ 内容校验通过；
USB 只读查询也仍能正常响应。**这说明失败发生在传输阶段，不能仅据此确定具体崩溃原因。**
日志中的线上更新信息获取失败与本次本地包上传是不同操作。

上传中断后，SD 卡更新目录可能有未写完的文件；不要运行其中的 `update.fuf`。
重新用完整包安装，或采用下面的 SD 卡直装方式。设备可正常开机时无需恢复出厂或 Repair。

## SD 卡直装，绕过 USB 上传

1. 关闭 AI session、关闭／断开 ESP32，正常关闭 Flipper 电源。
2. 取出 microSD，用读卡器连接电脑。
3. 解压本机生成的 `DeepFlipper-flipper-sd-update.zip`，将其中 **update 文件夹**复制到 SD 根目录。
   最终应存在 `/update/deepflipper-memory-fix-20261101/update.fuf`，六个更新文件须放在一起。
4. 安全弹出 SD，装回 Flipper 并开机。此时可以拔掉连接电脑的 USB，避免 qFlipper 建立屏幕投送。
5. 在 Flipper 文件浏览器中打开 **SD Card → update → deepflipper-memory-fix-20261101 → update.fuf**，
   选择安装并按设备提示完成；保持足够电量，刷写过程中不要断电。
6. 进入桌面后再接回 ESP32、打开 AI session，验证正常 NFC 等应用的启动和读取。

压缩包会附带 SHA256SUMS.txt，可以先核对解压文件。此路径使用原有的设备端更新器，
完整包含固件、无线固件、资源及更新器；并非只刷应用 BIN。

该直装包只在电脑上生成和校验，本次未对设备执行写入、刷机或恢复操作。
如果设备无法正常启动，应先判断它停在崩溃提示、更新器还是 USB 恢复模式，再选择恢复路径；
不要把 ESP32 的烧录地址或文件用于 Flipper。
