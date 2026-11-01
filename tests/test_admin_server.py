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
from core.conversation_history import history_for
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

    async def test_history_is_admin_only_filtered_paginated_and_persistent(self):
        self.assertEqual((await self.client.get('/api/history')).status,401)
        await self.login()
        history=history_for(str(self.root))
        for i in range(105):
            history.append('device-a','client-a','session-a','user',f'utterance-{i}')
        history.append('device-b','client-b','session-b','error','tts_failed')
        history.pending.join()
        response=await self.client.get('/api/history?device=device-a')
        self.assertEqual(response.status,200)
        data=await response.json()
        self.assertEqual(len(data['events']),100)
        self.assertTrue(all(r['device']=='device-a' for r in data['events']))
        older=await self.client.get('/api/history?device=device-a&before='+str(data['before']))
        self.assertEqual(len((await older.json())['events']),5)
        self.assertEqual((await self.client.get('/api/history?before=-1')).status,400)
        history.close()

    async def test_history_clear_requires_auth_csrf_and_explicit_scope(self):
        payload = {'scope': 'device', 'device': 'device-a'}
        self.assertEqual((await self.client.post('/api/history/clear', json=payload)).status, 401)
        headers = await self.login()
        history = history_for(str(self.root))
        try:
            for device in ('device-a', 'device-b'):
                history.append(device, 'c', 's', 'user', 'private question')
                history.append(device, 'c', 's', 'assistant', 'private answer')
            history.pending.join()
            config = (self.root / 'data/.config.yaml').read_bytes()
            self.assertEqual((await self.client.post('/api/history/clear', json=payload)).status, 403)
            denied = await self.client.post('/api/history/clear', json=payload,
                headers={**headers, 'Origin': 'https://other.invalid'})
            self.assertEqual(denied.status, 403)
            for invalid in ({}, {'scope': 'device', 'device': ''}, {'scope': 'all', 'device': 'device-a'},
                            {'scope': 'device', 'device': []}, {'scope': 'device', 'device': 'x'*81}):
                self.assertEqual((await self.client.post('/api/history/clear', json=invalid, headers=headers)).status, 400)
            self.assertEqual(len(history.read()['events']), 4)
            response = await self.client.post('/api/history/clear', json=payload, headers=headers)
            self.assertEqual(response.status, 200)
            self.assertEqual((await response.json())['deleted'], 2)
            self.assertEqual(history.context('device-a', 'c'), [])
            self.assertEqual(len(history.context('device-b', 'c')), 2)
            response = await self.client.post('/api/history/clear', json={'scope': 'all'}, headers=headers)
            self.assertEqual((await response.json())['deleted'], 2)
            self.assertEqual(history.read()['events'], [])
            self.assertEqual((self.root / 'data/.config.yaml').read_bytes(), config)
        finally:
            history.close()

    async def test_tts_preview_opus_mode_is_validated_and_preserves_configuration(self):
        headers = await self.login()
        original = (self.root / 'data/.config.yaml').read_bytes()
        response = await self.client.post('/api/tts-test', json={'preview_mode':'unknown'}, headers=headers)
        self.assertEqual(response.status,400)
        fake = types.SimpleNamespace(audio_file_type='wav',text_to_speak=None)
        async def speak(*args):return b'source-wav'
        fake.text_to_speak = speak
        modules = types.ModuleType('core.utils.modules_initialize')
        modules.initialize_tts = lambda config: fake
        from unittest.mock import Mock, patch
        diagnostics = types.ModuleType('core.audio_diagnostics')
        convert = diagnostics.opus_preview = Mock(return_value=b'roundtrip-wav')
        with patch.dict(sys.modules,{'core.utils.modules_initialize':modules,'core.audio_diagnostics':diagnostics}):
            response = await self.client.post('/api/tts-test',json={'preview_mode':'opus'},headers=headers)
            self.assertEqual(response.status,200)
            self.assertEqual(response.content_type,'audio/wav')
            self.assertEqual(await response.read(),b'roundtrip-wav')
            convert.assert_called_once_with(b'source-wav','wav',24000)
        self.assertEqual((self.root / 'data/.config.yaml').read_bytes(),original)

    async def test_stream_test_requires_connected_idle_device_and_never_calls_llm(self):
        import threading
        from unittest.mock import AsyncMock, Mock, patch
        self.assertEqual((await self.client.post('/api/tts-stream-test',json={})).status,401)
        headers=await self.login()
        self.assertEqual((await self.client.post('/api/tts-stream-test',json={},headers=headers)).status,400)
        payload={'device_id':'device-a','text':'这是一段测试语音。'}
        self.assertEqual((await self.client.post('/api/tts-stream-test',json=payload,headers=headers)).status,409)
        conn=types.SimpleNamespace(device_id='device-a',stop_event=threading.Event(),
            tts=types.SimpleNamespace(text_to_speak=AsyncMock(return_value=b'generated'),audio_file_type='wav'),
            asr_audio=[b'discard'],client_audio_buffer=bytearray(b'discard'),client_is_speaking=False,
            sample_rate=24000,clear_queues=lambda:None)
        self.runtime.server=types.SimpleNamespace(connections=[conn])
        sends=types.ModuleType('core.handle.sendAudioHandle')
        sends.send_tts_message=AsyncMock();sends.sendAudio=AsyncMock()
        diagnostics = types.ModuleType('core.audio_diagnostics')
        diagnostics.prepare_opus_audio = Mock(return_value=(b'test-wav',[b'1',b'2']))
        with patch.dict(sys.modules,{'core.handle.sendAudioHandle':sends,'core.audio_diagnostics':diagnostics}):
            response=await self.client.post('/api/tts-stream-test',json=payload,headers=headers)
            self.assertEqual(response.status,200)
            self.assertEqual((await response.json())['duration_ms'],120)
            self.assertEqual(conn.asr_audio,[])
            self.assertEqual(conn.client_audio_buffer,bytearray())
            self.assertFalse(conn.diagnostic_playback)
            self.assertEqual([call.args[1] for call in sends.send_tts_message.await_args_list],['start','sentence_start','stop'])
            self.assertEqual(sends.sendAudio.await_args.args[1],[b'1',b'2'])
        sample=await self.client.get('/api/tts-stream-test/audio')
        self.assertEqual(await sample.read(),b'test-wav')
        self.assertEqual(sample.content_type,'audio/wav')
        # A live LLM response is not interrupted by a diagnostic request.
        conn.client_is_speaking=True
        self.assertEqual((await self.client.post('/api/tts-stream-test',json=payload,headers=headers)).status,409)

    async def test_segmented_stream_test_uses_real_text_queue_and_captures_a_reference(self):
        import threading
        from unittest.mock import AsyncMock, Mock, patch
        headers=await self.login()
        conn=types.SimpleNamespace(device_id='device-a',stop_event=threading.Event(),
            tts=types.SimpleNamespace(text_to_speak=AsyncMock(),audio_file_type='wav'),
            asr_audio=[],client_audio_buffer=bytearray(),client_is_speaking=False,
            sample_rate=24000,clear_queues=lambda:None)
        messages=[]
        class TextQueue:
            def put(self,message):
                messages.append(message)
                if message.sentence_type.value=='LAST':
                    conn.audio_test_packets.extend([b'frame1',b'frame2'])
                    conn.client_is_speaking=False
        conn.tts.tts_text_queue=TextQueue()
        self.runtime.server=types.SimpleNamespace(connections=[conn])
        sends=types.ModuleType('core.handle.sendAudioHandle')
        sends.send_tts_message=AsyncMock();sends.sendAudio=AsyncMock()
        diagnostics=types.ModuleType('core.audio_diagnostics')
        diagnostics.prepare_opus_audio=Mock()
        diagnostics.decode_opus_packets=Mock(return_value=b'decoded-reference')
        with patch.dict(sys.modules,{'core.handle.sendAudioHandle':sends,'core.audio_diagnostics':diagnostics}):
            response=await self.client.post('/api/tts-stream-test',json={
                'device_id':'device-a','text':'测试分句。','segmented':True},headers=headers)
            self.assertEqual(response.status,200)
            self.assertEqual((await response.json())['sample_kind'],'opus_decoded')
            self.assertEqual([message.sentence_type.value for message in messages],['FIRST','MIDDLE','LAST'])
            self.assertEqual(messages[1].content_detail,'测试分句。')
            diagnostics.decode_opus_packets.assert_called_once_with([b'frame1',b'frame2'],24000)
            conn.tts.text_to_speak.assert_not_awaited()
            sends.sendAudio.assert_not_awaited()
        self.assertFalse(conn.diagnostic_playback)
        self.assertIsNone(conn.audio_test_packets)
        self.assertEqual(await (await self.client.get('/api/tts-stream-test/audio')).read(),b'decoded-reference')

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

    async def test_local_tts_selection_preserves_provider_secrets_and_can_switch_back(self):
        headers=await self.login()
        payload=await self.draft()
        payload['tts_provider']='LocalTTS'
        response=await self.client.post('/api/settings',json=payload,headers=headers)
        self.assertEqual(response.status,200)
        self.assertEqual((await self.draft())['tts_provider'],'LocalTTS')
        _, config=self.server.store.read()
        self.assertEqual(config['TTS']['LocalTTS']['type'],'local_melo')
        self.assertEqual(config['LLM']['DeepSeekLLM']['api_key'],'secret-old')
        payload=await self.draft();payload['tts_provider']='arbitrary-module'
        self.assertEqual((await self.client.post('/api/settings',json=payload,headers=headers)).status,400)
        payload=await self.draft();payload['tts_provider']='EdgeTTS'
        self.assertEqual((await self.client.post('/api/settings',json=payload,headers=headers)).status,200)
        self.assertEqual((await self.draft())['tts_provider'],'EdgeTTS')
        self.assertEqual((await self.client.post('/api/tts-test',json={})).status,403)

    async def test_tts_preview_is_authenticated_and_returns_audio_without_changing_settings(self):
        from unittest.mock import patch
        class Speech:
            audio_file_type='wav'
            async def text_to_speak(self,text,output_file):
                self.assertion=(text,output_file)
                return b'RIFF-test-audio'
        initialized=[]
        module=types.ModuleType('core.utils.modules_initialize')
        module.initialize_tts=lambda config:(initialized.append(config) or Speech())
        headers=await self.login()
        original=(self.root/'data/.config.yaml').read_bytes()
        with patch.dict(sys.modules,{'core.utils.modules_initialize':module}):
            response=await self.client.post('/api/tts-test',json={'tts_provider':'LocalTTS'},headers=headers)
        self.assertEqual(response.status,200)
        self.assertEqual(response.content_type,'audio/wav')
        self.assertEqual(await response.read(),b'RIFF-test-audio')
        self.assertEqual(initialized[0]['selected_module']['TTS'],'LocalTTS')
        self.assertEqual((self.root/'data/.config.yaml').read_bytes(),original)

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

    async def test_connection_profile_is_bound_to_identity_and_excludes_provider_credentials(self):
        self.assertEqual((await self.client.post('/api/connection-profile', json={})).status, 401)
        headers = await self.login()
        original = (self.root / 'data/.config.yaml').read_bytes()
        response = await self.client.post('/api/connection-profile', json={
            'device_id': 'device-a', 'client_id': 'client-a',
            'server_url': 'ws://192.168.1.10:8000/xiaozhi/v1/'}, headers=headers)
        self.assertEqual(response.status, 200)
        data = await response.json()
        self.assertEqual(data['format'], 'deepflipper.connection')
        self.assertEqual(data['version'], 1)
        self.assertEqual(data['issued_at'], int(data['token'].rsplit('.', 1)[1]))
        self.assertEqual(data['expires_at'] - data['issued_at'], data['expires_in'])
        self.assertEqual(data['server_url'], 'ws://192.168.1.10:8000/xiaozhi/v1/')
        self.assertTrue(AuthManager(self.signing).verify_token(data['token'], 'client-a', 'device-a'))
        self.assertFalse(AuthManager(self.signing).verify_token(data['token'], 'client-b', 'device-a'))
        self.assertNotIn('secret-old', json.dumps(data))
        self.assertNotIn(self.signing, json.dumps(data))
        self.assertEqual(response.headers['Cache-Control'], 'no-store')
        self.assertIn('attachment', response.headers['Content-Disposition'])
        self.assertEqual((self.root / 'data/.config.yaml').read_bytes(), original)

    async def test_connection_export_rejects_unusable_device_urls_and_revoked_devices(self):
        headers = await self.login()
        for url in ('http://host/', 'ws://localhost:8000/', 'ws://127.0.0.1:8000/',
                    'ws://0.0.0.0/', 'ws://host:0/', 'ws://host:65536/',
                    'ws://user:secret@host/', 'ws://host/?token=secret',
                    'ws://[::1]/', 'ws://host/#x', 'ws://host:/'):
            with self.subTest(url=url):
                response = await self.client.post('/api/connection-profile', json={
                    'device_id': 'device-a', 'client_id': 'client-a', 'server_url': url}, headers=headers)
                self.assertEqual(response.status, 400)
        (self.root / 'data/.config.yaml').write_text('server:\n  auth:\n    revoked_devices: [device-a]\n', encoding='utf-8')
        response = await self.client.post('/api/connection-profile', json={
            'device_id': 'device-a', 'client_id': 'client-a'}, headers=headers)
        self.assertEqual(response.status, 400)

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
