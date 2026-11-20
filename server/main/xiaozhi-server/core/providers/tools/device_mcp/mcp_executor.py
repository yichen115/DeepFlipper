"""设备端MCP工具执行器"""

from typing import Dict, Any, TYPE_CHECKING

if TYPE_CHECKING:
    from core.connection import ConnectionHandler
from ..base import ToolType, ToolDefinition, ToolExecutor
from plugins_func.register import Action, ActionResponse
from .mcp_handler import call_mcp_tool
from .deepflipper_jobs import await_flipper_job
from core.utils import flipper_screen


class DeviceMCPExecutor(ToolExecutor):
    """设备端MCP工具执行器"""

    def __init__(self, conn):
        self.conn = conn

    async def execute(
        self, conn: "ConnectionHandler", tool_name: str, arguments: Dict[str, Any]
    ) -> ActionResponse:
        """执行设备端MCP工具"""
        if not hasattr(conn, "mcp_client") or not conn.mcp_client:
            return ActionResponse(
                action=Action.ERROR,
                response="设备端MCP客户端未初始化",
            )

        if not await conn.mcp_client.is_ready():
            return ActionResponse(
                action=Action.ERROR,
                response="设备端MCP客户端未准备就绪",
            )

        try:
            # 转换参数为JSON字符串
            import json

            # 危险按键（发送/模拟/写入/删除）确认开关：关闭时自动带 confirm:true
            arguments = flipper_screen.apply_confirm_policy(conn, tool_name, arguments)
            # 对话记录要留下每一步的屏幕截图：按键类工具自动带 bitmap（history_screenshots: false 可关）
            if (tool_name in ("flipper_press_keys", "flipper_ui_select")
                    and conn.config.get("history_screenshots", True)
                    and isinstance(arguments, dict) and "bitmap" not in arguments):
                arguments = {**arguments, "bitmap": True}
            args_str = json.dumps(arguments) if arguments else "{}"

            # 调用设备端MCP工具
            result = await call_mcp_tool(conn, conn.mcp_client, tool_name, args_str)
            screen_png = None
            if tool_name.startswith("flipper_") and tool_name not in (
                "flipper_job_get", "flipper_job_cancel"
            ):
                async def call_job(name, arguments):
                    return await call_mcp_tool(conn, conn.mcp_client, name, arguments)
                result = await await_flipper_job(result, call_job)
                # 截图：位图转 PNG，从给模型的文字里去掉 bitmap，并保存给管理页面
                result, screen_png, screen_data = flipper_screen.split_screenshot(result)
                flipper_screen.remember_screen(conn, tool_name, screen_data, screen_png)
                flipper_screen.note_confirmation(conn, screen_data)

            resultJson = None
            if isinstance(result, str):
                try:
                    resultJson = json.loads(result)
                except Exception as e:
                    pass

            # 视觉大模型不经过二次LLM处理（Flipper 结果里的 action 是按钮名，例如 needs_confirmation 的 "Send"）
            if (
                resultJson is not None
                and isinstance(resultJson, dict)
                and "action" in resultJson
                and not tool_name.startswith("flipper_")
            ):
                return ActionResponse(
                    action=Action[resultJson["action"]],
                    response=resultJson.get("response", ""),
                )

            response = ActionResponse(action=Action.REQLLM, result=str(result))
            # 连接层按模型是否支持看图，决定是否把截图作为 image_url 发给模型
            response.screen_png = screen_png
            return response

        except ValueError as e:
            return ActionResponse(action=Action.NOTFOUND, response=str(e))
        except Exception as e:
            return ActionResponse(action=Action.ERROR, response=str(e))

    def get_tools(self) -> Dict[str, ToolDefinition]:
        """获取所有设备端MCP工具"""
        if not hasattr(self.conn, "mcp_client") or not self.conn.mcp_client:
            return {}

        tools = {}
        mcp_tools = self.conn.mcp_client.get_available_tools()

        for tool in mcp_tools:
            func_def = tool.get("function", {})
            tool_name = func_def.get("name", "")

            if tool_name:
                tools[tool_name] = ToolDefinition(
                    name=tool_name, description=tool, tool_type=ToolType.DEVICE_MCP
                )

        return tools

    def has_tool(self, tool_name: str) -> bool:
        """检查是否有指定的设备端MCP工具"""
        if not hasattr(self.conn, "mcp_client") or not self.conn.mcp_client:
            return False

        return self.conn.mcp_client.has_tool(tool_name)
