import json
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from core.connection import ConnectionHandler

TAG = __name__
EMOJI_MAP = {
    "😂": "funny",
    "😭": "crying",
    "😠": "angry",
    "😔": "sad",
    "😍": "loving",
    "😲": "surprised",
    "😱": "shocked",
    "🤔": "thinking",
    "😌": "relaxed",
    "😴": "sleepy",
    "😜": "silly",
    "🙄": "confused",
    "😶": "neutral",
    "🙂": "happy",
    "😆": "laughing",
    "😳": "embarrassed",
    "😉": "winking",
    "😎": "cool",
    "🤤": "delicious",
    "😘": "kissy",
    "😏": "confident",
}
EMOJI_RANGES = [
    (0x1F000, 0x1FAFF),  # emoticons, symbols, flags, skin tones, extended pictographs
    (0x2600, 0x26FF),
    (0x2700, 0x27BF),
    (0x2B00, 0x2BFF),  # stars, arrows used as emoji
    (0xFE00, 0xFE0F),  # variation selectors
    (0x200D, 0x200D),  # zero-width joiner
    (0x20E3, 0x20E3),  # keycap
]
# The Flipper's 1-bit screen cannot draw emoji and TTS would read them out, so
# the whale girl's mood comes from her words. Checked in order; first hit wins.
EMOTION_KEYWORDS = [
    ("sad", ("难过", "伤心", "抱歉", "对不起", "呜呜", "可惜", "遗憾", "sorry")),
    ("angry", ("生气", "哼", "气死")),
    ("surprised", ("哇", "真的吗", "居然", "竟然", "天哪", "没想到", "wow")),
    ("embarrassed", ("害羞", "不好意思", "脸红", "嘿嘿")),
    ("sleepy", ("困了", "好困", "晚安", "睡觉", "打哈欠")),
    ("thinking", ("让我想想", "我想想", "嗯…", "嗯...", "好像", "也许", "可能是")),
    ("happy", ("哈哈", "太好了", "好耶", "开心", "高兴", "喜欢", "米饭", "当然", "没问题", "好呀", "好的")),
]


def get_string_no_punctuation_or_emoji(s):
    """去除字符串首尾的空格、标点符号和表情符号"""
    chars = list(s)
    # 处理开头的字符
    start = 0
    while start < len(chars) and is_punctuation_or_emoji(chars[start]):
        start += 1
    # 处理结尾的字符
    end = len(chars) - 1
    while end >= start and is_punctuation_or_emoji(chars[end]):
        end -= 1
    return "".join(chars[start : end + 1])


def is_punctuation_or_emoji(char):
    """检查字符是否为空格、指定标点或表情符号"""
    # 定义需要去除的中英文标点（包括全角/半角）
    punctuation_set = {
        "，",
        ",",  # 中文逗号 + 英文逗号
        "。",
        ".",  # 中文句号 + 英文句号
        "！",
        "!",  # 中文感叹号 + 英文感叹号
        "“",
        "”",
        '"',  # 中文双引号 + 英文引号
        "：",
        ":",  # 中文冒号 + 英文冒号
        "-",
        "－",  # 英文连字符 + 中文全角横线
        "、",  # 中文顿号
        "[",
        "]",  # 方括号
        "【",
        "】",  # 中文方括号
    }
    if char.isspace() or char in punctuation_set:
        return True
    return is_emoji(char)


def guess_emotion(text):
    """Pick an expression for the Flipper from an emoji or, normally, the words."""
    for char in text:
        if char in EMOJI_MAP:
            return EMOJI_MAP[char]
    lowered = text.lower()
    for emotion, words in EMOTION_KEYWORDS:
        if any(word in lowered for word in words):
            return emotion
    return "neutral"


async def get_emotion(conn: "ConnectionHandler", text):
    """获取文本内的情绪消息"""
    emotion = guess_emotion(text)
    # Text stays plain: the device only uses the emotion name.
    emoji = emotion
    try:
        await conn.websocket.send(
            json.dumps(
                {
                    "type": "llm",
                    "text": emoji,
                    "emotion": emotion,
                    "session_id": conn.session_id,
                }
            )
        )
    except Exception as e:
        conn.logger.bind(tag=TAG).warning(f"发送情绪表情失败，错误:{e}")
    return


def is_emoji(char):
    """检查字符是否为emoji表情"""
    code_point = ord(char)
    return any(start <= code_point <= end for start, end in EMOJI_RANGES)


def check_emoji(text):
    """去除文本中的所有emoji表情"""
    return "".join(char for char in text if not is_emoji(char) and char != "\n")


def strip_emoji(text):
    """Remove emoji but keep line breaks and spacing (used on LLM output)."""
    return "".join(char for char in text if not is_emoji(char))
