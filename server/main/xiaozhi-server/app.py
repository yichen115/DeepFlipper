import sys
import os
import uuid
import signal
import asyncio
from aioconsole import ainput
from config.settings import load_config
from config.logger import setup_logging
from core.utils.util import get_local_ip, validate_mcp_endpoint
from core.voice_runtime import VoiceRuntime
from core.admin_server import AdminServer
from config.config_loader import get_project_dir
from core.utils.util import check_ffmpeg_installed
from core.utils.gc_manager import get_gc_manager
from core.handle.helloHandle import preheat_wakeup_audio_cache

TAG = __name__
logger = setup_logging()


async def wait_for_exit() -> None:
    """
    阻塞直到收到 Ctrl‑C / SIGTERM。
    - Unix: 使用 add_signal_handler
    - Windows: 依赖 KeyboardInterrupt
    """
    loop = asyncio.get_running_loop()
    stop_event = asyncio.Event()

    if sys.platform != "win32":  # Unix / macOS
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, stop_event.set)
        await stop_event.wait()
    else:
        # Windows：await一个永远pending的fut，
        # 让 KeyboardInterrupt 冒泡到 asyncio.run，以此消除遗留普通线程导致进程退出阻塞的问题
        try:
            await asyncio.Future()
        except KeyboardInterrupt:  # Ctrl‑C
            pass


async def monitor_stdin():
    """监控标准输入，消费回车键"""
    try:
        while True:
            await ainput()
    except EOFError:
        return


async def main():
    check_ffmpeg_installed()
    config = await load_config()

    # Use the persistent server signing key; never generate an ephemeral replacement.
    auth_key = os.environ.get("DEEPFLIPPER_AUTH_KEY") or config["server"].get("auth_key", "")
    
    # Missing or invalid signing keys must fail startup.
    if not auth_key or len(auth_key) < 32 or "你" in auth_key:
        raise RuntimeError("Set a stable DEEPFLIPPER_AUTH_KEY of at least 32 characters")
    
    config["server"]["auth_key"] = auth_key

    # 添加 stdin 监控任务
    stdin_task = asyncio.create_task(monitor_stdin())

    # 预热唤醒词音频编码缓存，让首条连接就命中
    asyncio.create_task(preheat_wakeup_audio_cache())

    # 启动全局GC管理器（5分钟清理一次）
    gc_manager = get_gc_manager(interval_seconds=300)
    await gc_manager.start()

    runtime = VoiceRuntime()
    admin_server = None
    admin = config.get("admin", {})
    if admin.get("enabled", True):
        admin_server = AdminServer(get_project_dir(), runtime, signing_key=auth_key)
        host = os.environ.get("DEEPFLIPPER_ADMIN_HOST") or admin.get("host", "127.0.0.1")
        await admin_server.start(host, int(admin.get("port", 8001)))
        logger.bind(tag=TAG).info("Python admin page is available on port {}", admin.get("port", 8001))
    runtime.apply()
    # DeepFlipper uses direct voice WebSocket configuration. Auxiliary HTTP is opt-in.
    ota_task = None
    port = int(config["server"].get("http_port", 0))
    if port > 0:
        from core.http_server import SimpleHttpServer
        ota_server = SimpleHttpServer(config)
        ota_task = asyncio.create_task(ota_server.start())
        logger.bind(tag=TAG).info(f"Optional HTTP service enabled on port {port}")
    else:
        logger.bind(tag=TAG).info("Voice-only mode: OTA and vision HTTP service disabled")

    mcp_endpoint = config.get("mcp_endpoint", None)
    if mcp_endpoint is not None and "你" not in mcp_endpoint:
        # 校验MCP接入点格式
        if validate_mcp_endpoint(mcp_endpoint):
            logger.bind(tag=TAG).info("mcp接入点是\t{}", mcp_endpoint)
            # 将mcp计入点地址转成调用点
            mcp_endpoint = mcp_endpoint.replace("/mcp/", "/call/")
            config["mcp_endpoint"] = mcp_endpoint
        else:
            logger.bind(tag=TAG).error("mcp接入点不符合规范")
            config["mcp_endpoint"] = "你的接入点 websocket地址"

    # 获取WebSocket配置，使用安全的默认值
    websocket_port = 8000
    server_config = config.get("server", {})
    if isinstance(server_config, dict):
        websocket_port = int(server_config.get("port", 8000))

    logger.bind(tag=TAG).info(
        "Websocket地址是\tws://{}:{}/xiaozhi/v1/",
        get_local_ip(),
        websocket_port,
    )

    logger.bind(tag=TAG).info(
        "=======上面的地址是websocket协议地址，请勿用浏览器访问======="
    )
    logger.bind(tag=TAG).info(
        "请使用已配置设备 Token 的 DeepFlipper ESP32 连接；部署说明见项目 README"
    )
    logger.bind(tag=TAG).info(
        "=============================================================\n"
    )

    try:
        await wait_for_exit()  # 阻塞直到收到退出信号
    except asyncio.CancelledError:
        print("任务被取消，清理资源中...")
    finally:
        # 停止全局GC管理器
        await gc_manager.stop()

        # 取消所有任务（关键修复点）
        stdin_task.cancel()
        await runtime.close()
        if admin_server:
            await admin_server.close()
        if ota_task:
            ota_task.cancel()

        # 等待任务终止（必须加超时）
        await asyncio.wait(
            [stdin_task, ota_task] if ota_task else [stdin_task],
            timeout=3.0,
            return_when=asyncio.ALL_COMPLETED,
        )
        print("服务器已关闭，程序退出。")


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("手动中断，程序终止。")
