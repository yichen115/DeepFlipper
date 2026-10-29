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

填写模型前、缺少语音模型或语音初始化失败时，管理页仍能打开。
保存并应用会结束当前语音连接。配置与所有密钥仅存于忽略的 `data/` 中。
仍可直接编辑 `data/.config.yaml`；旧 Java 管理 API 配置会被明确拒绝。

设备控制通过系统提示词、MCP 工具定义和已实现的硬件执行代码完成。
当前提供 Flipper 状态查询、NFC 协议扫描和任务查询／取消。增加提示词
不会自动获得 RF、BLE、写卡等尚未实现的能力。

默认 OTA／视觉 HTTP 服务关闭（`server.http_port: 0`）。模型文件单独下载，
不进入 Git 或镜像。保留 [LICENSE](LICENSE)，来源见
[sources.lock.json](../sources.lock.json)。完整部署与接线见
[项目 README](../README.md)。
