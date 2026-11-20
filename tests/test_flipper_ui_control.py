"""AI operates Flipper apps: screenshots, image pruning, confirm gate, tool rounds."""
import asyncio
import base64
import importlib.util
import json
from pathlib import Path
import queue
import sys
import threading
import types
import unittest
import zlib
from unittest.mock import AsyncMock, patch

import yaml

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'server/main/xiaozhi-server'
sys.path.insert(0, str(SERVER))

from core.utils import flipper_screen  # noqa: E402  (stdlib only)
from core.utils.dialogue import Dialogue, Message, OLD_IMAGE_PLACEHOLDER  # noqa: E402

HAS_AIOHTTP = importlib.util.find_spec('aiohttp') is not None


def connection_importable():
    try:
        import core.connection  # noqa: F401
        return True
    except Exception:
        return False


HAS_CONNECTION = connection_importable()


def frame_with(*pixels):
    frame = bytearray(flipper_screen.FRAME_BYTES)
    for x, y in pixels:
        frame[(y // 8) * 128 + x] |= 1 << (y % 8)
    return bytes(frame)


def decode_png(png):
    """Minimal decoder for the 1-bit grayscale, filter-0 PNG we produce."""
    assert png[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, header = 8, b'', None
    while pos < len(png):
        length = int.from_bytes(png[pos:pos + 4], 'big')
        kind = png[pos + 4:pos + 8]
        body = png[pos + 8:pos + 8 + length]
        assert zlib.crc32(kind + body) & 0xFFFFFFFF == int.from_bytes(png[pos + 8 + length:pos + 12 + length], 'big')
        if kind == b'IHDR':
            header = body
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    width, height = int.from_bytes(header[:4], 'big'), int.from_bytes(header[4:8], 'big')
    assert header[8:10] == b'\x01\x00'  # 1-bit grayscale
    raw = zlib.decompress(idat)
    stride = 1 + (width + 7) // 8
    rows = []
    for y in range(height):
        line = raw[y * stride:(y + 1) * stride]
        assert line[0] == 0
        rows.append([(line[1 + x // 8] >> (7 - x % 8)) & 1 for x in range(width)])
    return width, height, rows  # 1 = white, 0 = black


class BitmapTests(unittest.TestCase):
    def test_png_is_4x_black_on_white_with_u8g2_page_layout(self):
        # (5, 10): byte (10 // 8) * 128 + 5 = 133, bit 2. (0, 0): byte 0, LSB = top row.
        frame = frame_with((5, 10), (0, 0), (127, 63))
        self.assertEqual(frame[133], 0b100)
        png = flipper_screen.bitmap_to_png(base64.b64encode(frame).decode())
        width, height, rows = decode_png(png)
        self.assertEqual((width, height), (512, 256))
        for x, y in ((5, 10), (0, 0), (127, 63)):
            for dy in range(4):
                for dx in range(4):
                    self.assertEqual(rows[y * 4 + dy][x * 4 + dx], 0, (x, y))
        # Neighbours stay white: no transposition or bit-order mix-up.
        self.assertEqual(rows[10 * 4][6 * 4], 1)
        self.assertEqual(rows[11 * 4][5 * 4], 1)
        self.assertEqual(rows[5 * 4][10 * 4], 1)
        self.assertEqual(rows[1 * 4][0], 1)
        black = sum(row.count(0) for row in rows)
        self.assertEqual(black, 3 * 16)

    def test_pillow_reads_the_png_when_available(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow not installed')
        import io
        image = Image.open(io.BytesIO(flipper_screen.bitmap_to_png(frame_with((5, 10))))).convert('L')
        self.assertEqual(image.size, (512, 256))
        self.assertEqual(image.getpixel((21, 41)), 0)
        self.assertEqual(image.getpixel((25, 41)), 255)

    def test_rejects_wrong_size(self):
        with self.assertRaises(ValueError):
            flipper_screen.bitmap_to_png(base64.b64encode(b'\0' * 10).decode())

    def test_split_removes_bitmap_and_keeps_screen_fields(self):
        result = json.dumps({'ok': True, 'app': 'Sub-GHz', 'view': '▶Read\nRead RAW', 'selected': 'Read',
                             'bitmap': base64.b64encode(frame_with((1, 1))).decode()}, ensure_ascii=False)
        text, png, data = flipper_screen.split_screenshot(result)
        self.assertNotIn('bitmap', json.loads(text))
        self.assertEqual(json.loads(text)['selected'], 'Read')
        self.assertTrue(png.startswith(b'\x89PNG'))
        self.assertEqual(flipper_screen.screen_fields(data)['app'], 'Sub-GHz')
        broken, png, _ = flipper_screen.split_screenshot('{"ok":true,"bitmap":"!!"}')
        self.assertIsNone(png)
        self.assertEqual(json.loads(broken)['bitmap_error'], 'invalid_bitmap')
        plain = '{"ok": true, "view": "x"}'
        self.assertEqual(flipper_screen.split_screenshot(plain)[0], plain)

    def test_vision_detection(self):
        self.assertTrue(flipper_screen.is_vision_model({'model_name': 'deepseek-flash'}))
        self.assertTrue(flipper_screen.is_vision_model({'model_name': 'qwen-vl-max'}))
        self.assertTrue(flipper_screen.is_vision_model({'model_name': 'glm-4v-plus'}))
        self.assertFalse(flipper_screen.is_vision_model({'model_name': 'deepseek-chat'}))
        self.assertFalse(flipper_screen.is_vision_model({'model_name': 'deepseek-flash', 'vision': False}))
        self.assertTrue(flipper_screen.is_vision_model({'model_name': 'my-model', 'vision': 'true'}))


def image_content(tag):
    return [{'type': 'text', 'text': '{"view":"%s"}' % tag},
            {'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,' + tag}}]


def tool_round(dialogue, call_id, content):
    dialogue.put(Message(role='assistant', tool_calls=[{'id': call_id, 'type': 'function', 'index': 0,
                         'function': {'name': 'flipper_screen', 'arguments': '{}'}}]))
    dialogue.put(Message(role='tool', tool_call_id=call_id, content=content))


class DialogueImageTests(unittest.TestCase):
    def build(self):
        dialogue = Dialogue()
        dialogue.put(Message(role='system', content='sys'))
        dialogue.put(Message(role='user', content='看看屏幕'))
        tool_round(dialogue, 'a', image_content('old'))
        tool_round(dialogue, 'b', image_content('new'))
        dialogue.put(Message(role='assistant', content=[{'type': 'text', 'text': '好了'}]))
        return dialogue

    def test_only_latest_tool_message_keeps_its_image(self):
        messages = self.build().get_llm_dialogue_with_memory(None, None)
        tools = [m for m in messages if m['role'] == 'tool']
        self.assertIsInstance(tools[0]['content'], str)
        self.assertIn('old', tools[0]['content'])
        self.assertIn(OLD_IMAGE_PLACEHOLDER, tools[0]['content'])
        self.assertIsInstance(tools[1]['content'], list)
        self.assertEqual(tools[1]['content'][1]['image_url']['url'], 'data:image/png;base64,new')
        # Assistant messages never carry list content.
        self.assertEqual(messages[-1], {'role': 'assistant', 'content': '好了'})
        json.dumps(messages)  # serialisable for the OpenAI client and debug log

    def test_text_only_models_get_no_images(self):
        messages = self.build().get_llm_dialogue_with_memory(None, None, None, max_images=0)
        self.assertTrue(all(isinstance(m.get('content', ''), str) for m in messages if 'content' in m))

    def test_tool_message_content_depends_on_vision(self):
        png = flipper_screen.bitmap_to_png(frame_with((0, 0)))
        content = flipper_screen.tool_message_content('{"ok":true}', png, True)
        self.assertEqual(content[0], {'type': 'text', 'text': '{"ok":true}'})
        self.assertTrue(content[1]['image_url']['url'].startswith('data:image/png;base64,iVBOR'))
        text = flipper_screen.tool_message_content('{"ok":true}', png, False)
        self.assertEqual(json.loads(text)['screenshot'], flipper_screen.NO_VISION_NOTE)
        self.assertEqual(flipper_screen.tool_message_content('{"ok":true}', None, True), '{"ok":true}')

    def test_history_records_text_of_list_content(self):
        from core.conversation_history import record_message
        events = []
        conn = types.SimpleNamespace(conversation_history=types.SimpleNamespace(
            append=lambda *args, **kwargs: events.append(args)), device_id='d', headers={}, session_id='s')
        record_message(conn, Message(role='assistant', content=[{'type': 'text', 'text': '好了'},
                                                                 {'type': 'image_url', 'image_url': {'url': 'data:x'}}]))
        record_message(conn, Message(role='tool', content=image_content('x')))
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0][4], '好了\n[截图]')


class ConfirmPolicyTests(unittest.TestCase):
    def test_setting_off_injects_confirm_for_key_tools_only(self):
        conn = types.SimpleNamespace(config={'ui_confirm_dangerous': False}, sentence_id='t1')
        self.assertEqual(flipper_screen.apply_confirm_policy(conn, 'flipper_press_keys', {'keys': 'ok'}),
                         {'keys': 'ok', 'confirm': True})
        self.assertTrue(flipper_screen.apply_confirm_policy(conn, 'flipper_ui_select', {'text': 'Send'})['confirm'])
        self.assertEqual(flipper_screen.apply_confirm_policy(conn, 'flipper_screen', {'bitmap': True}), {'bitmap': True})

    def test_setting_on_requires_the_users_answer_in_the_next_turn(self):
        conn = types.SimpleNamespace(config={}, sentence_id='t1')
        # The model may not confirm on its own.
        self.assertFalse(flipper_screen.apply_confirm_policy(conn, 'flipper_ui_select', {'text': 'Send', 'confirm': True})['confirm'])
        flipper_screen.note_confirmation(conn, {'ok': False, 'code': 'needs_confirmation', 'action': 'Send'})
        self.assertFalse(flipper_screen.apply_confirm_policy(conn, 'flipper_ui_select', {'confirm': True})['confirm'])
        # User answers: the next turn may confirm.
        flipper_screen.start_turn(conn, 't2'); conn.sentence_id = 't2'
        self.assertTrue(flipper_screen.apply_confirm_policy(conn, 'flipper_ui_select', {'confirm': True})['confirm'])
        # A later turn without a fresh question cannot.
        flipper_screen.start_turn(conn, 't3'); conn.sentence_id = 't3'
        self.assertFalse(flipper_screen.apply_confirm_policy(conn, 'flipper_ui_select', {'confirm': True})['confirm'])


def load_executor(call):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import test_mcp_executor
    return test_mcp_executor.load_executor(call)


class ExecutorScreenTests(unittest.IsolatedAsyncioTestCase):
    async def run_tool(self, tool, args, reply, config=None):
        calls = []

        async def call(conn, client, name, arguments):
            calls.append((name, json.loads(arguments)))
            return reply

        executor = load_executor(call)
        conn = types.SimpleNamespace(config=config or {}, sentence_id='t1',
                                     mcp_client=types.SimpleNamespace(is_ready=AsyncMock(return_value=True)))
        result = await executor(conn).execute(conn, tool, args)
        return result, conn, calls

    async def test_bitmap_becomes_png_and_is_kept_for_the_admin_page(self):
        reply = json.dumps({'ok': True, 'app': 'NFC', 'view': '▶Read', 'selected': 'Read',
                            'bitmap': base64.b64encode(frame_with((3, 3))).decode()})
        result, conn, calls = await self.run_tool('flipper_screen', {'bitmap': True}, reply)
        self.assertEqual(result.action, 'llm')
        self.assertNotIn('bitmap', json.loads(result.result))
        self.assertTrue(result.screen_png.startswith(b'\x89PNG'))
        self.assertEqual(conn.last_screen_png, result.screen_png)
        self.assertEqual(conn.last_screen['app'], 'NFC')
        self.assertEqual(calls, [('flipper_screen', {'bitmap': True})])

    async def test_confirm_injected_when_setting_off(self):
        reply = json.dumps({'ok': True, 'app': 'Sub-GHz', 'view': 'Send', 'pressed': 1})
        _, _, calls = await self.run_tool('flipper_press_keys', {'keys': 'ok'}, reply,
                                          config={'ui_confirm_dangerous': False, 'history_screenshots': False})
        self.assertEqual(calls[0][1], {'keys': 'ok', 'confirm': True})

    async def test_key_tools_ask_for_a_screenshot_for_the_transcript(self):
        reply = json.dumps({'ok': True, 'app': 'Sub-GHz', 'view': 'REC', 'pressed': 1})
        _, _, calls = await self.run_tool('flipper_press_keys', {'keys': 'ok'}, reply)
        self.assertTrue(calls[0][1]['bitmap'])
        _, _, calls = await self.run_tool('flipper_ui_select', {'text': 'Read', 'bitmap': False}, reply)
        self.assertFalse(calls[0][1]['bitmap'])                    # an explicit choice is kept
        _, _, calls = await self.run_tool('flipper_app_exit', {}, reply)
        self.assertNotIn('bitmap', calls[0][1])

    async def test_needs_confirmation_is_a_normal_result_for_the_model(self):
        reply = json.dumps({'ok': False, 'code': 'needs_confirmation', 'action': 'Send', 'pressed': 2,
                            'app': 'Sub-GHz', 'view': '▶Send'})
        result, conn, calls = await self.run_tool('flipper_ui_select', {'text': 'Send', 'confirm': True}, reply)
        self.assertEqual(result.action, 'llm')
        self.assertEqual(json.loads(result.result)['action'], 'Send')
        self.assertFalse(calls[0][1]['confirm'])  # self-made confirmation removed
        self.assertEqual(conn.ui_confirm_asked_turn, 't1')


class ConfigTests(unittest.TestCase):
    def test_defaults(self):
        config = yaml.safe_load((SERVER / 'config.yaml').read_text(encoding='utf-8'))
        self.assertEqual(config['max_tool_rounds'], 24)
        self.assertIs(config['ui_confirm_dangerous'], True)
        self.assertIs(config['ui_progress_voice'], True)
        self.assertEqual(config['LLM']['DeepSeekLLM']['vision'], 'auto')

    def test_policy_is_positive_and_lists_ui_tools(self):
        from config.deepflipper_defaults import TOOL_POLICY, TOOLS
        self.assertNotIn('投递事件', TOOL_POLICY)
        self.assertNotIn('不证明页面动作完成', TOOL_POLICY)
        for name in ('flipper_app_open', 'flipper_ui_select', 'flipper_press_keys', 'needs_confirmation', 'bitmap:true'):
            self.assertIn(name, TOOL_POLICY)
        names = {tool['name'] for tool in TOOLS}
        self.assertTrue({'flipper_screen', 'flipper_press_keys', 'flipper_ui_select', 'flipper_go_home'} <= names)


class FakeLogger:
    def bind(self, **kwargs):
        return self

    def __getattr__(self, name):
        return lambda *args, **kwargs: None


def tool_call_delta(call_id, name, arguments):
    return types.SimpleNamespace(index=0, id=call_id, function=types.SimpleNamespace(name=name, arguments=arguments))


@unittest.skipUnless(HAS_CONNECTION, 'Voice server dependencies are tested inside the Docker image')
class ChatLoopTests(unittest.TestCase):
    def setUp(self):
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(2)
        self.loop.close()

    def connection(self, config, tool_result):
        from core.connection import ConnectionHandler
        from plugins_func.register import Action, ActionResponse
        conn = ConnectionHandler.__new__(ConnectionHandler)
        conn.config = config
        conn.logger = FakeLogger()
        conn.dialogue = Dialogue()
        conn.dialogue.put(Message(role='system', content='sys'))
        spoken = []
        conn.tts = types.SimpleNamespace(
            tts_text_queue=queue.Queue(), store_tts_text=lambda *a: None,
            tts_one_sentence=lambda conn, kind, content_detail=None, **k: spoken.append(content_detail))
        conn.intent_type = 'function_call'
        conn.memory = None
        conn.current_speaker = None
        conn.system_introduced_speakers = set()
        conn.client_abort = False
        conn.session_id = 's'
        conn.features = {'emoji': False}
        conn.sentence_id = None
        conn.loop = self.loop
        self.llm_calls = []

        def make_result():
            response = ActionResponse(action=Action.REQLLM, result=tool_result)
            response.screen_png = flipper_screen.bitmap_to_png(frame_with((0, 0)))
            return response

        async def handle(conn, data):
            return make_result()

        conn.func_handler = types.SimpleNamespace(get_functions=lambda: [{'type': 'function'}],
                                                  handle_llm_function_call=handle)
        test = self

        class LLM:
            model_name = 'deepseek-flash'

            def response_with_functions(self, session_id, dialogue, functions=None):
                test.llm_calls.append(('tools', dialogue))
                yield None, [tool_call_delta('call%d' % len(test.llm_calls), 'flipper_press_keys', '{"keys":"down"}')]

            def response(self, session_id, dialogue):
                test.llm_calls.append(('final', dialogue))
                yield '好了，已经选中。'

        conn.llm = LLM()
        return conn

    def drain(self, conn):
        items = []
        while not conn.tts.tts_text_queue.empty():
            items.append(conn.tts.tts_text_queue.get_nowait())
        return items

    def test_max_tool_rounds_then_forced_final_answer(self):
        conn = self.connection({'max_tool_rounds': 3, 'ui_progress_voice': True,
                                'selected_module': {'LLM': 'X'}, 'LLM': {'X': {}}},
                               '{"ok":true,"app":"Sub-GHz","view":"Read"}')
        self.assertEqual(conn._max_tool_rounds(), 3)
        conn.chat('帮我按一下')
        kinds = [kind for kind, _ in self.llm_calls]
        self.assertEqual(kinds, ['tools', 'tools', 'tools', 'final'])
        final_dialogue = self.llm_calls[-1][1]
        self.assertIn('最大工具调用次数', final_dialogue[-1]['content'])
        # deepseek-flash sees exactly one (the latest) screenshot.
        images = [m for m in final_dialogue if isinstance(m.get('content'), list)]
        self.assertEqual(len(images), 1)
        self.assertIs(images[0], [m for m in final_dialogue if m['role'] == 'tool'][-1])
        # One progress line, flushed right away, never stored in the dialogue.
        from core.providers.tts.dto.dto import ContentType, SentenceType
        items = self.drain(conn)
        texts = [i.content_detail for i in items if i.content_type == ContentType.TEXT]
        self.assertEqual(texts.count('好，我来操作一下。'), 1)
        index = next(n for n, i in enumerate(items) if i.content_detail == '好，我来操作一下。')
        self.assertEqual((items[index + 1].content_type, items[index + 1].sentence_type),
                         (ContentType.ACTION, SentenceType.MIDDLE))
        self.assertEqual(items[0].sentence_type, SentenceType.FIRST)
        self.assertEqual(items[-1].sentence_type, SentenceType.LAST)
        self.assertEqual(len({i.sentence_id for i in items}), 1)
        self.assertNotIn('好，我来操作一下。', [m.content for m in conn.dialogue.dialogue])
        self.assertEqual(conn.dialogue.dialogue[-1].content, '好了，已经选中。')

    def test_rounds_config_bounds_and_text_models(self):
        conn = self.connection({'ui_progress_voice': False, 'selected_module': {'LLM': 'X'},
                                'LLM': {'X': {'model_name': 'deepseek-chat'}}}, '{"ok":true}')
        self.assertEqual(conn._max_tool_rounds(), 24)
        conn.config['max_tool_rounds'] = 'x'
        self.assertEqual(conn._max_tool_rounds(), 24)
        conn.config['max_tool_rounds'] = 999
        self.assertEqual(conn._max_tool_rounds(), 64)
        conn.config['max_tool_rounds'] = 1
        conn.chat('按一下')
        self.assertEqual([kind for kind, _ in self.llm_calls], ['tools', 'final'])
        final_dialogue = self.llm_calls[-1][1]
        self.assertFalse(any(isinstance(m.get('content'), list) for m in final_dialogue))
        tool = [m for m in final_dialogue if m['role'] == 'tool'][-1]
        self.assertEqual(json.loads(tool['content'])['screenshot'], flipper_screen.NO_VISION_NOTE)
        from core.providers.tts.dto.dto import ContentType
        texts = [i.content_detail for i in self.drain(conn) if i.content_type == ContentType.TEXT]
        self.assertNotIn('好，我来操作一下。', texts)


@unittest.skipUnless(HAS_AIOHTTP, 'aiohttp not installed')
class AdminScreenTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        import tempfile
        import aiohttp
        from aiohttp.test_utils import TestClient, TestServer
        from core.admin_server import AdminServer
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / 'data').mkdir()
        (self.root / 'admin').mkdir()
        defaults = {'selected_module': {'LLM': 'DeepSeekLLM', 'TTS': 'EdgeTTS'},
                    'LLM': {'DeepSeekLLM': {'type': 'openai', 'base_url': 'https://api.example.test/v1',
                                           'model_name': 'model-a', 'api_key': 'secret-old'}}}
        (self.root / 'config.yaml').write_text(yaml.safe_dump(defaults), encoding='utf-8')
        self.tools = {'flipper_screen'}
        client = types.SimpleNamespace(is_ready=AsyncMock(return_value=True), has_tool=lambda name: name in self.tools)
        self.conn = types.SimpleNamespace(device_id='device-a', stop_event=threading.Event(), mcp_client=client,
                                          loop=None)
        self.runtime = types.SimpleNamespace(apply=lambda: None, server=types.SimpleNamespace(connections=[self.conn]),
                                             status=lambda: {'state': 'ready', 'devices': [], 'error': ''})
        self.token = 'test-admin-token-' + 'a' * 32
        self.server = AdminServer(self.root, self.runtime, self.token, 'test-signing-key-' + 'b' * 32)
        self.client = TestClient(TestServer(self.server.app()), cookie_jar=aiohttp.CookieJar(unsafe=True))
        await self.client.start_server()
        response = await self.client.post('/api/login', json={'token': self.token})
        self.headers = {'X-CSRF-Token': (await response.json())['csrf']}

    async def asyncTearDown(self):
        await self.client.close()
        self.directory.cleanup()

    async def test_refresh_reads_a_frame_and_get_returns_it(self):
        empty = await self.client.get('/api/flipper-screen?device=device-a')
        self.assertEqual(empty.status, 200)
        self.assertEqual((await empty.json())['png'], '')
        calls = []

        async def call_mcp_tool(conn, client, name, arguments, timeout=30):
            calls.append((name, json.loads(arguments)))
            return json.dumps({'ok': True, 'app': 'Infrared', 'view': '▶Learn New Remote', 'selected': 'Learn New Remote',
                               'bitmap': base64.b64encode(frame_with((2, 2))).decode()})

        handler = types.ModuleType('core.providers.tools.device_mcp.mcp_handler')
        handler.call_mcp_tool = call_mcp_tool
        package = types.ModuleType('core.providers.tools.device_mcp')
        package.__path__ = [str(SERVER / 'core/providers/tools/device_mcp')]
        with patch.dict(sys.modules, {'core.providers.tools.device_mcp': package,
                                      'core.providers.tools.device_mcp.mcp_handler': handler}):
            response = await self.client.post('/api/flipper-screen/refresh', json={'device_id': 'device-a'},
                                              headers=self.headers)
        self.assertEqual(response.status, 200, await response.text())
        data = await response.json()
        self.assertEqual(calls, [('flipper_screen', {'bitmap': True})])
        self.assertEqual(data['app'], 'Infrared')
        self.assertIn('Learn New Remote', data['view'])
        self.assertTrue(base64.b64decode(data['png']).startswith(b'\x89PNG'))
        again = await (await self.client.get('/api/flipper-screen?device=device-a')).json()
        self.assertEqual(again['png'], data['png'])
        self.assertEqual((await self.client.get('/api/flipper-screen?device=missing')).status, 404)

    async def test_refresh_needs_the_device_tool(self):
        self.tools = set()
        response = await self.client.post('/api/flipper-screen/refresh', json={'device_id': 'device-a'},
                                          headers=self.headers)
        self.assertEqual(response.status, 409)

    async def test_confirm_setting_round_trips(self):
        settings = await (await self.client.get('/api/settings')).json()
        self.assertIs(settings['ui_confirm_dangerous'], True)
        response = await self.client.post('/api/settings', headers=self.headers, json={
            'revision': settings['revision'], 'ui_confirm_dangerous': False})
        self.assertEqual(response.status, 200, await response.text())
        stored = yaml.safe_load((self.root / 'data/.config.yaml').read_text(encoding='utf-8'))
        self.assertIs(stored['ui_confirm_dangerous'], False)
        settings = await (await self.client.get('/api/settings')).json()
        self.assertIs(settings['ui_confirm_dangerous'], False)
        bad = await self.client.post('/api/settings', headers=self.headers, json={
            'revision': settings['revision'], 'ui_confirm_dangerous': 'no'})
        self.assertEqual(bad.status, 400)


if __name__ == '__main__':
    unittest.main()
