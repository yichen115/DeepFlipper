"""Continuous conversation: keep listening after each reply, stop after a quiet follow-up window.

How it fits together (see docs/development-log 第 23 节):
  * server hello carries ``continuous: true`` -> the device listens in ``auto`` mode, the server's VAD
    ends each sentence (no second button press);
  * every ``tts stop`` carries ``listen: "auto"`` -> after playback the device listens again;
  * the device announces ``listen start mode=auto`` -> we start a follow-up timer; if nobody speaks
    within ``continuous_follow_up_s`` we send ``listen stop`` and the device goes quiet (no ASR, no
    goodbye). The connection itself stays open as before.
A single turn can also ask for one answer (``conn.listen_after_turn``), e.g. "要不要现在听？".
"""
import asyncio
import json
import time

from core.conversation_history import record_event


def enabled(conn):
    return bool((getattr(conn, "config", None) or {}).get("continuous_chat", False))


def follow_up_seconds(conn):
    try:
        value = float((getattr(conn, "config", None) or {}).get("continuous_follow_up_s", 8))
    except (TypeError, ValueError):
        value = 8.0
    return max(3.0, min(30.0, value))


def wants_listen(conn):
    """Called when a reply finishes: should the device listen for an answer right away?"""
    asked = bool(getattr(conn, "listen_after_turn", False))
    conn.listen_after_turn = False
    if getattr(conn, "close_after_chat", False) or getattr(conn, "client_abort", False):
        return False
    return asked or enabled(conn)


def on_listen_start(conn, mode):
    task = getattr(conn, "follow_up_task", None)
    if task is not None and not task.done():
        task.cancel()
    conn.follow_up_task = None
    # Only the automatic listen after a reply has a short window; a button press is deliberate.
    if mode != "auto" or not getattr(conn, "follow_up_pending", False):
        conn.follow_up_pending = False
        return
    conn.follow_up_pending = False
    try:
        conn.follow_up_task = asyncio.get_running_loop().create_task(_watch(conn, time.time()))
    except RuntimeError:
        pass


async def _watch(conn, started):
    await asyncio.sleep(follow_up_seconds(conn))
    if conn.stop_event.is_set() or getattr(conn, "client_listen_mode", "") != "auto":
        return
    if getattr(conn, "client_is_speaking", False) or getattr(conn, "client_have_voice", False):
        return
    if float(getattr(conn, "last_activity_time", 0) or 0) > started * 1000:
        return                      # the user spoke; the ASR/VAD path takes it from here
    try:
        await conn.websocket.send(json.dumps({"type": "listen", "state": "stop",
                                              "session_id": getattr(conn, "session_id", "")}))
    except Exception:
        return
    conn.client_listen_mode = "manual"
    record_event(conn, "status", "连续对话：一会儿没听到说话，先停下等你按键")
