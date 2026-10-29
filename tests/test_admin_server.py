import importlib.util
import json
import re
from pathlib import Path
import sys
import tempfile
import types
import unittest

import aiohttp
from aiohttp import web
from aiohttp.test_utils import TestClient, TestServer
import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
from core.admin_server import AdminServer
from core.auth import AuthManager
from config.config_loader import merge_local_config


class AdminTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)
        (self.root / 'data').mkdir()
        (self.root / 'admin').mkdir()
        (self.root / 'admin/index.html').write_text('<html>Admin</html>', encoding='utf-8')
        defaults = {'server': {'websocket': 'ws://192.168.1.2:8000/xiaozhi/v1/'},
                    'selected_module': {'LLM': 'DeepSeekLLM', 'ASR': 'FunASR', 'TTS': 'EdgeTTS'},
                    'LLM': {'DeepSeekLLM': {'type': 'openai', 'base_url': 'https://api.example.test/v1',
                                           'model_name': 'model-a', 'api_key': 'secret-old'}},
                    'TTS': {'EdgeTTS': {'voice': 'zh-CN-XiaoxiaoNeural'}}}
        (self.root / 'config.yaml').write_text(yaml.safe_dump(defaults), encoding='utf-8')
        (self.root / 'data/.config.yaml').write_text('prompt: custom\ncustom_preserved: yes\n', encoding='utf-8')
        self.runtime = types.SimpleNamespace(apply=lambda: None,
            status=lambda: {'state': 'needs_config', 'devices': [], 'error': ''})
        self.token = 'test-admin-token-' + 'a' * 32
        self.signing = 'test-signing-key-' + 'b' * 32
        self.server = AdminServer(self.root, self.runtime, self.token, self.signing)
        self.client = TestClient(TestServer(self.server.app()), cookie_jar=aiohttp.CookieJar(unsafe=True))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()
        self.directory.cleanup()

    async def login(self):
        response = await self.client.post('/api/login', json={'token': self.token})
        self.assertEqual(response.status, 200)
        data = await response.json()
        return {'X-CSRF-Token': data['csrf']}

    async def draft(self):
        response = await self.client.get('/api/settings')
        return await response.json()

    async def test_anonymous_cannot_read_settings_and_device_token_is_not_an_admin_token(self):
        self.assertEqual((await self.client.get('/api/settings')).status, 401)
        device = AuthManager(self.signing).generate_token('client-a', 'device-a')
        response = await self.client.get('/api/settings', headers={'Authorization': 'Bearer ' + device})
        self.assertEqual(response.status, 401)
        page = await self.client.get('/')
        self.assertEqual(page.status, 200)
        self.assertIn("frame-ancestors 'none'", page.headers['Content-Security-Policy'])

    async def test_settings_do_not_return_saved_provider_or_server_keys(self):
        await self.login()
        response = await self.client.get('/api/settings')
        body = await response.text()
        self.assertNotIn('secret-old', body)
        self.assertNotIn(self.token, body)
        self.assertNotIn(self.signing, body)
        self.assertTrue(json.loads(body)['api_key_set'])
        board = (ROOT / 'firmware/esp32/main/boards/deepflipper-s3/deepflipper_board.cc').read_text(encoding='utf-8')
        registered = set(re.findall(r'mcp\.AddTool\(\s*"(flipper_[^"]+)"', board))
        catalog = {tool['name'] for tool in json.loads(body)['tools']}
        self.assertEqual(catalog, registered)
        self.assertIn('flipper_run_saved', catalog)
        self.assertEqual((await self.client.get('/api/status')).status, 200)

    async def test_cookie_writes_require_csrf_and_reject_foreign_origins(self):
        headers = await self.login()
        payload = await self.draft()
        self.assertEqual((await self.client.post('/api/settings', json=payload)).status, 403)
        response = await self.client.post('/api/settings', json=payload,
            headers={**headers, 'Origin': 'https://foreign.example.test'})
        self.assertEqual(response.status, 403)

    async def test_blank_key_preserves_old_key_atomic_update_and_revision_conflict(self):
        headers = await self.login()
        payload = await self.draft()
        payload.update(api_key='', model_name='model-b', prompt='updated prompt')
        response = await self.client.post('/api/settings', json=payload, headers=headers)
        self.assertEqual(response.status, 200)
        self.assertNotIn('secret-old', await response.text())
        stored = yaml.safe_load((self.root / 'data/.config.yaml').read_text(encoding='utf-8'))
        self.assertTrue(stored['custom_preserved'])
        self.assertEqual(stored['LLM']['DeepSeekLLM']['api_key'], 'secret-old')
        _, merged = self.server.store.read()
        self.assertIn('[DeepFlipper tool policy]', merged['prompt'])
        self.assertEqual((await self.client.post('/api/settings', json=payload, headers=headers)).status, 409)

    async def test_key_replacement_and_explicit_clear(self):
        headers = await self.login()
        payload = await self.draft()
        payload['api_key'] = 'replacement-secret'
        self.assertEqual((await self.client.post('/api/settings', json=payload, headers=headers)).status, 200)
        payload = await self.draft()
        self.assertNotIn('replacement-secret', json.dumps(payload))
        payload['clear_api_key'] = True
        self.assertEqual((await self.client.post('/api/settings', json=payload, headers=headers)).status, 200)
        self.assertFalse((await self.draft())['api_key_set'])

    async def test_issued_token_is_bound_to_device_and_client_and_logout_revokes_session(self):
        headers = await self.login()
        response = await self.client.post('/api/device-token',
            json={'device_id': 'device-a', 'client_id': 'client-a'}, headers=headers)
        self.assertEqual(response.status, 200)
        token = (await response.json())['token']
        auth = AuthManager(self.signing)
        self.assertTrue(auth.verify_token(token, 'client-a', 'device-a'))
        self.assertFalse(auth.verify_token(token, 'other-client', 'device-a'))
        self.assertEqual((await self.client.post('/api/logout', json={}, headers=headers)).status, 200)
        self.assertEqual((await self.client.get('/api/settings')).status, 401)

    async def test_invalid_model_addresses_and_control_characters_do_not_change_config(self):
        headers = await self.login()
        original = (self.root / 'data/.config.yaml').read_bytes()
        for bad in ('file:///etc/passwd', 'https://secret@host/v1', 'https://host/v1?key=secret'):
            payload = await self.draft()
            payload['base_url'] = bad
            response = await self.client.post('/api/settings', json=payload, headers=headers)
            self.assertEqual(response.status, 400)
        self.assertEqual((self.root / 'data/.config.yaml').read_bytes(), original)

    async def test_model_connection_probe_uses_draft_key_and_only_a_harmless_tool(self):
        received = []
        async def provider(request):
            self.assertEqual(request.headers['Authorization'], 'Bearer probe-only-key')
            value = await request.json()
            received.append(value)
            self.assertEqual(value['tools'][0]['function']['name'], 'deepflipper_probe')
            return web.json_response({'id': 'test', 'object': 'chat.completion', 'created': 0,
                'model': 'model-a', 'choices': [{'index': 0, 'finish_reason': 'tool_calls',
                    'message': {'role': 'assistant', 'content': None, 'tool_calls': [
                        {'id': 'test-call', 'type': 'function', 'function': {
                            'name': 'deepflipper_probe', 'arguments': '{}'}}]}}]})
        app = web.Application()
        app.router.add_post('/v1/chat/completions', provider)
        provider_server = TestServer(app)
        await provider_server.start_server()
        try:
            headers = await self.login()
            original = (self.root / 'data/.config.yaml').read_bytes()
            response = await self.client.post('/api/model-test', json={
                'base_url': str(provider_server.make_url('/v1')), 'model_name': 'model-a',
                'api_key': 'probe-only-key'}, headers=headers)
            self.assertEqual(response.status, 200)
            self.assertTrue((await response.json())['tool_call_returned'])
            self.assertEqual(len(received), 1)
            self.assertEqual((self.root / 'data/.config.yaml').read_bytes(), original)
        finally:
            await provider_server.close()


class LocalOnlyConfigTests(unittest.TestCase):
    def test_java_manager_settings_are_rejected_and_local_settings_still_merge(self):
        for config in ({'manager-api': {'url': 'http://old-manager'}}, {'read_config_from_api': True}):
            with self.subTest(config=config), self.assertRaises(ValueError):
                merge_local_config({}, config)
        merged = merge_local_config({'server': {'port': 8000}}, {'server': {'http_port': 0}})
        self.assertEqual(merged['server'], {'port': 8000, 'http_port': 0})
        self.assertFalse(merged['read_config_from_api'])
