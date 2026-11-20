"""The 1-bit Flipper screen can't draw emoji; replies must be plain text."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))


class NoEmojiTests(unittest.TestCase):
    def test_strip_emoji_keeps_text_and_line_breaks(self):
        from core.utils.textUtils import strip_emoji
        self.assertEqual(strip_emoji('好呀😄！\n今天吃米饭🍚吗❤️⭐'), '好呀！\n今天吃米饭吗')
        self.assertEqual(strip_emoji('Hello 👋🏽 there'), 'Hello  there')

    def test_emotion_comes_from_words_when_there_is_no_emoji(self):
        from core.utils.textUtils import guess_emotion
        self.assertEqual(guess_emotion('哈哈，太好了'), 'happy')
        self.assertEqual(guess_emotion('抱歉，我没听清'), 'sad')
        self.assertEqual(guess_emotion('哇，真的吗'), 'surprised')
        self.assertEqual(guess_emotion('让我想想'), 'thinking')
        self.assertEqual(guess_emotion('现在是下午三点'), 'neutral')
        self.assertEqual(guess_emotion('😭'), 'crying')

    def test_prompts_forbid_emoji(self):
        prompt = (ROOT / 'server/main/xiaozhi-server/agent-base-prompt.txt').read_text(encoding='utf-8')
        self.assertIn('表情符号', prompt)
        from config.deepflipper_defaults import DEFAULT_PROMPT
        self.assertIn('emoji', DEFAULT_PROMPT)
        self.assertNotIn('小智', DEFAULT_PROMPT + prompt)

    def test_asr_emotion_is_a_word_not_an_emoji(self):
        from core.providers.asr.utils import lang_tag_filter
        result = lang_tag_filter('<|zh|><|SAD|><|Speech|><|withitn|>你好。')
        self.assertEqual(result['emotion'], 'sad')
        self.assertNotIn('emoji', result)


if __name__ == '__main__':
    unittest.main()
