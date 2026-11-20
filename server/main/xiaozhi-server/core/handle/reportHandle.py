"""Chat-record reporting hooks.

Upstream forwarded ASR/TTS/tool records to the Java manager-api. DeepFlipper
only supports the local Python admin page (history lives in
core/conversation_history.py), so these hooks are intentionally no-ops. They are
kept because the ASR/TTS base classes and tool paths still call them.
"""

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from core.connection import ConnectionHandler


def enqueue_tts_report(conn: "ConnectionHandler", text, opus_data):
    """No remote chat-record service in DeepFlipper."""
    return None


def enqueue_tool_report(
    conn: "ConnectionHandler",
    tool_name: str,
    tool_input: dict,
    tool_result: str = None,
    report_tool_call: bool = True,
):
    """No remote chat-record service in DeepFlipper."""
    return None


def enqueue_asr_report(conn: "ConnectionHandler", text, opus_data):
    """No remote chat-record service in DeepFlipper."""
    return None
