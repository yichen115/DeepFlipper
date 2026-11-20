"""Flipper screen readback: screenshots for the LLM and the admin mirror.

Device tools (flipper_screen / flipper_press_keys / flipper_ui_select / ...)
may return ``bitmap``: base64 of the 128x64 1-bit frame in u8g2 page layout
(byte = (y // 8) * 128 + x, bit = y % 8 with LSB on top, 1 = black pixel).
This module turns it into a small PNG without third-party dependencies,
removes it from the JSON text the model reads, keeps the latest frame per
connection and applies the "confirm before dangerous presses" setting.
"""
import base64
import binascii
import json
import re
import struct
import time
import zlib

WIDTH, HEIGHT = 128, 64
FRAME_BYTES = WIDTH * HEIGHT // 8
SCALE = 4

# Tools that drive the Flipper UI like a person would.
UI_TOOLS = frozenset({
    "flipper_screen", "flipper_press_keys", "flipper_ui_select", "flipper_go_home",
    "flipper_app_open", "flipper_press_button", "flipper_app_exit",
})
# Tools whose next OK press may be gated by the device (Send/Emulate/Write/...).
CONFIRM_TOOLS = frozenset({"flipper_press_keys", "flipper_ui_select"})

NO_VISION_NOTE = "截图只显示在管理页面；当前模型看不到图片，请依据 view 文字判断"

# Model names that accept image_url content parts (config ``vision: auto``).
_VISION_MODEL = re.compile(
    r"deepseek-flash|vision|gpt-4o|gpt-4\.1|gpt-5|qwen[\w.-]*-vl|glm-4(?:\.\d+)?v|gemini"
    r"|(?:^|[-_/.])vl(?:$|[-_/.\d])",
    re.IGNORECASE,
)


# ---------------------------------------------------------------- bitmap / PNG
def decode_frame(data):
    """Return the raw 1024-byte frame from base64 text or bytes."""
    if isinstance(data, str):
        frame = base64.b64decode(data.encode("ascii"), validate=True)
    elif isinstance(data, (bytes, bytearray)):
        frame = bytes(data)
    else:
        raise TypeError("bitmap must be base64 text")
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"bitmap must be {FRAME_BYTES} bytes, got {len(frame)}")
    return frame


def pixel(frame, x, y):
    """True when the pixel at (x, y) is black (drawn)."""
    return bool(frame[(y // 8) * WIDTH + x] >> (y % 8) & 1)


def _chunk(kind, payload):
    body = kind + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def bitmap_to_png(data, scale=SCALE):
    """Encode the frame as a 1-bit grayscale PNG, black pixels on white."""
    frame = decode_frame(data)
    scale = int(scale)
    if not 1 <= scale <= 16:
        raise ValueError("scale must be 1-16")
    width, height = WIDTH * scale, HEIGHT * scale
    raw = bytearray()
    for y in range(HEIGHT):
        bits = 0
        for x in range(WIDTH):
            # PNG grayscale bit 1 = white, 0 = black.
            value = 0 if pixel(frame, x, y) else 1
            for _ in range(scale):
                bits = (bits << 1) | value
        padding = (-width) % 8
        row = (bits << padding).to_bytes((width + padding) // 8, "big")
        line = b"\x00" + row  # filter type 0 (None)
        raw.extend(line * scale)
    header = struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", header)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b""))


def png_data_url(png):
    return "data:image/png;base64," + base64.b64encode(png).decode("ascii")


# ------------------------------------------------------------ tool result text
def split_screenshot(text):
    """Split a device tool result into (text without bitmap, png or None, parsed dict or None).

    The text is returned unchanged when it carries no bitmap.
    """
    if not isinstance(text, str):
        return text, None, None
    try:
        data = json.loads(text)
    except (ValueError, TypeError):
        return text, None, None
    if not isinstance(data, dict):
        return text, None, None
    holder = data
    if "bitmap" not in data and isinstance(data.get("result"), dict):
        holder = data["result"]
    if "bitmap" not in holder:
        return text, None, data
    bitmap = holder.pop("bitmap")
    png = None
    try:
        png = bitmap_to_png(bitmap)
    except (ValueError, TypeError, binascii.Error):
        holder["bitmap_error"] = "invalid_bitmap"
    return json.dumps(data, ensure_ascii=False), png, data


def screen_fields(data):
    """Pick the screen description out of a parsed tool result."""
    if not isinstance(data, dict):
        return None
    source = data if "view" in data or "app" in data else data.get("result")
    if not isinstance(source, dict) or not ("view" in source or "app" in source):
        return None
    return {key: source.get(key) for key in ("app", "view", "selected", "locked", "buttons")
            if key in source}


def remember_screen(conn, tool_name, data, png):
    """Keep the latest screen text and screenshot on the connection for the admin page."""
    now = time.time()
    fields = screen_fields(data)
    if fields is not None:
        conn.last_screen = {**fields, "tool": tool_name, "time": now}
    if png:
        conn.last_screen_png = png
        conn.last_screen_png_time = now


def is_vision_model(llm_config):
    """LLM config ``vision``: true/false, or auto (default) = guess from model_name."""
    llm_config = llm_config or {}
    value = llm_config.get("vision", "auto")
    if isinstance(value, bool):
        return value
    text = str(value).strip().lower()
    if text in ("true", "yes", "on", "1"):
        return True
    if text in ("false", "no", "off", "0"):
        return False
    return bool(_VISION_MODEL.search(str(llm_config.get("model_name") or "")))


def tool_message_content(text, png, vision):
    """Tool message content: text plus image part for vision models, text otherwise."""
    if not png:
        return text
    if vision:
        return [{"type": "text", "text": text},
                {"type": "image_url", "image_url": {"url": png_data_url(png)}}]
    try:
        data = json.loads(text)
        if isinstance(data, dict):
            data["screenshot"] = NO_VISION_NOTE
            return json.dumps(data, ensure_ascii=False)
    except (ValueError, TypeError):
        pass
    return text


# ------------------------------------------------------------- confirm policy
def start_turn(conn, new_sentence_id):
    """Called for every new user turn, before ``conn.sentence_id`` changes.

    A confirmation asked in the previous turn may be granted in this one only.
    """
    asked = getattr(conn, "ui_confirm_asked_turn", None)
    previous = getattr(conn, "sentence_id", None)
    conn.ui_confirm_granted_turn = new_sentence_id if asked and asked == previous else None
    conn.ui_confirm_asked_turn = None


def apply_confirm_policy(conn, tool_name, arguments):
    """Return the arguments to send to the device for UI key tools.

    - ``ui_confirm_dangerous: false``: always pass ``confirm: true``.
    - otherwise ``confirm: true`` only counts in the user turn right after the
      device asked for confirmation; earlier/self-made confirmations are removed
      so the device gate asks again and the model has to ask the user.
    """
    if tool_name not in CONFIRM_TOOLS:
        return arguments
    args = dict(arguments or {})
    config = getattr(conn, "config", None) or {}
    if not config.get("ui_confirm_dangerous", True):
        args["confirm"] = True
        return args
    if args.get("confirm"):
        granted = getattr(conn, "ui_confirm_granted_turn", None)
        if granted is None or granted != getattr(conn, "sentence_id", None):
            args["confirm"] = False
    return args


def note_confirmation(conn, data):
    """Remember that the device asked for confirmation in this turn."""
    if isinstance(data, dict) and data.get("code") == "needs_confirmation":
        conn.ui_confirm_asked_turn = getattr(conn, "sentence_id", None)
