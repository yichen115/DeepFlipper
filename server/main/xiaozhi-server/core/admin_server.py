"""Small authenticated settings page hosted by the existing Python process."""
import asyncio
import hmac
import ipaddress
import os
from pathlib import Path
import re
import secrets
import time
from urllib.parse import urlsplit

from aiohttp import web

from core.admin_config import AdminConfig, ConfigConflict, configured
from core.auth import AuthManager
from core.conversation_history import history_for

SESSION_KEY = web.RequestKey('admin_session', str) if hasattr(web, 'RequestKey') else 'admin_session'


class AdminServer:
    def __init__(self, root, runtime, token=None, signing_key=None):
        self.root = Path(root)
        self.store = AdminConfig(root)
        self.runtime = runtime
        self.token = token if token is not None else os.environ.get('DEEPFLIPPER_ADMIN_TOKEN', '')
        self.signing_key = signing_key if signing_key is not None else os.environ.get('DEEPFLIPPER_AUTH_KEY', '')
        if len(self.token) < 32:
            raise ValueError('Run scripts/setup_local_server.py to create DEEPFLIPPER_ADMIN_TOKEN')
        if len(self.signing_key) < 32:
            raise ValueError('A persistent DEEPFLIPPER_AUTH_KEY is required')
        self.sessions = {}
        self.attempts = {}
        self.lock = asyncio.Lock()
        self.runner = None

    @web.middleware
    async def security(self, request, handler):
        unsafe = request.method not in ('GET', 'HEAD')
        if unsafe:
            origin = request.headers.get('Origin')
            if origin and origin != f'{request.scheme}://{request.host}':
                raise web.HTTPForbidden(text='跨站请求已拒绝')
            if request.content_type != 'application/json':
                raise web.HTTPUnsupportedMediaType(text='需要 JSON 请求')
        if request.path.startswith('/api/') and request.path != '/api/login':
            authorization = request.headers.get('Authorization', '')
            bearer = authorization.startswith('Bearer ') and hmac.compare_digest(
                authorization[7:].encode('utf-8'), self.token.encode('utf-8'))
            session_id = request.cookies.get('deepflipper_admin', '')
            session = self.sessions.get(session_id)
            valid_session = session and session['expires'] > time.time()
            if not bearer and not valid_session:
                raise web.HTTPUnauthorized(text='请先登录')
            if not bearer and unsafe and not hmac.compare_digest(
                request.headers.get('X-CSRF-Token', '').encode('utf-8'), session['csrf'].encode('utf-8')):
                raise web.HTTPForbidden(text='页面验证已过期，请刷新')
            request[SESSION_KEY] = session_id if valid_session else ''
        try:
            response = await handler(request)
        except ConfigConflict as error:
            response = web.json_response({'error': str(error)}, status=409)
        except (ValueError, TypeError, KeyError) as error:
            response = web.json_response({'error': str(error)}, status=400)
        response.headers['Cache-Control'] = 'no-store'
        response.headers['X-Content-Type-Options'] = 'nosniff'
        response.headers['Content-Security-Policy'] = (
            "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; "
            "img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
        return response

    async def body(self, request):
        value = await request.json()
        if not isinstance(value, dict):
            raise ValueError('请求应为 JSON 对象')
        return value

    async def login(self, request):
        now = time.time()
        peer = request.remote or 'local'
        attempts = [when for when in self.attempts.get(peer, []) if now - when < 60]
        if len(attempts) >= 10:
            raise web.HTTPTooManyRequests(text='尝试次数过多，请稍后再试')
        body = await self.body(request)
        value = body.get('token', '')
        if not isinstance(value, str) or not hmac.compare_digest(value.encode('utf-8'), self.token.encode('utf-8')):
            self.attempts[peer] = attempts + [now]
            raise web.HTTPUnauthorized(text='管理口令不正确')
        self.attempts.pop(peer, None)
        self.sessions = {key: value for key, value in self.sessions.items() if value['expires'] > now}
        if len(self.sessions) >= 64:
            self.sessions.pop(next(iter(self.sessions)))
        session_id = secrets.token_urlsafe(32)
        csrf = secrets.token_urlsafe(32)
        self.sessions[session_id] = {'csrf': csrf, 'expires': now + 8 * 3600}
        response = web.json_response({'ok': True, 'csrf': csrf})
        response.set_cookie('deepflipper_admin', session_id, httponly=True, samesite='Strict',
                            secure=request.secure, max_age=8 * 3600, path='/')
        return response

    async def session(self, request):
        session = self.sessions.get(request.get(SESSION_KEY), {})
        return web.json_response({'ok': True, 'csrf': session.get('csrf', '')})

    async def logout(self, request):
        self.sessions.pop(request.get(SESSION_KEY), None)
        response = web.json_response({'ok': True})
        response.del_cookie('deepflipper_admin', path='/')
        return response

    async def settings(self, request):
        return web.json_response(self.store.public())

    async def save(self, request):
        payload = await self.body(request)
        async with self.lock:
            config = self.store.save(payload)
        self.runtime.apply()
        return web.json_response({'ok': True, 'revision': config['revision'], 'applying': True})

    async def status(self, request):
        return web.json_response(self.runtime.status())

    async def history(self, request):
        device = request.query.get('device', '')
        if len(device) > 80:
            raise ValueError('设备标识过长')
        before = int(request.query.get('before', '0'))
        if before < 0:
            raise ValueError('分页参数无效')
        result = await asyncio.to_thread(history_for(str(self.root)).read, device, before)
        return web.json_response(result)

    async def test_tts(self, request):
        from core.utils.modules_initialize import initialize_tts
        from core.tts_settings import LOCAL_TTS
        payload = await self.body(request)
        selected = payload.get('tts_provider', self.store.public()['tts_provider'])
        if selected not in ('LocalTTS', 'EdgeTTS'):
            raise ValueError('不支持的语音合成方式')
        _, config = self.store.read()
        config.setdefault('selected_module', {})['TTS'] = selected
        config.setdefault('TTS', {}).setdefault('LocalTTS', LOCAL_TTS.copy())
        try:
            tts = await asyncio.to_thread(initialize_tts, config)
            audio = await asyncio.wait_for(tts.text_to_speak('你好，我是鲸鱼娘。语音测试成功。', None), 20)
            if not audio:
                raise ValueError('语音合成没有返回音频')
            return web.Response(body=audio, content_type='audio/wav' if tts.audio_file_type == 'wav' else 'audio/mpeg')
        except FileNotFoundError:
            return web.json_response({'error': '缺少本地语音模型，请运行 python scripts/download_models.py --tts-only'}, status=409)
        except (TimeoutError, asyncio.TimeoutError):
            return web.json_response({'error': '语音合成超时，请检查所选服务或本机负载'}, status=504)

    def device_credentials(self, payload):
        device, client = payload.get('device_id', ''), payload.get('client_id', '')
        for value in (device, client):
            if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9_.:-]{1,80}', value):
                raise ValueError('Device ID / Client ID 格式不正确')
        _, config = self.store.read()
        auth = config.get('server', {}).get('auth', {})
        if device in auth.get('revoked_devices', []):
            raise ValueError('设备已被吊销')
        if auth.get('allowed_devices') and device not in auth['allowed_devices']:
            raise ValueError('设备不在允许列表内')
        manager = AuthManager(self.signing_key, auth.get('expire_seconds'))
        token = manager.generate_token(client, device)
        return {'device_id': device, 'client_id': client, 'token': token,
                'expires_in': manager.expire_seconds, 'issued_at': int(token.rsplit('.', 1)[1]),
                'expires_at': int(token.rsplit('.', 1)[1]) + manager.expire_seconds}

    async def issue_token(self, request):
        result = self.device_credentials(await self.body(request))
        return web.json_response({'token': result['token'], 'expires_in': result['expires_in']})

    async def connection_profile(self, request):
        payload = await self.body(request)
        _, config = self.store.read()
        url = payload.get('server_url', config.get('server', {}).get('websocket', ''))
        if not isinstance(url, str) or not 1 <= len(url) <= 192 or any(ord(c) <= 32 or ord(c) >= 127 for c in url):
            raise ValueError('服务器地址应为设备可访问的 WS/WSS 地址，最多 192 个 ASCII 字符')
        try:
            parsed = urlsplit(url)
            port = parsed.port
        except ValueError:
            raise ValueError('服务器地址格式不正确') from None
        if (parsed.scheme not in ('ws', 'wss') or not parsed.hostname or parsed.username or parsed.password
                or parsed.query or parsed.fragment or any(c in url for c in ('@', '?', '#', '\\', '[', ']'))
                or any(not (c.isalnum() or c in '.-') for c in parsed.hostname)
                or parsed.netloc.endswith(':') or port == 0):
            raise ValueError('服务器地址不支持凭据、查询参数、片段或 IPv6 字面量')
        if parsed.hostname.lower() == 'localhost' or parsed.hostname.lower().endswith('.localhost'):
            raise ValueError('ESP32 不能访问电脑的 localhost，请使用局域网 IP 或服务器域名')
        try:
            address = ipaddress.ip_address(parsed.hostname)
        except ValueError:
            address = None
        if address and (address.is_loopback or address.is_unspecified or address.is_multicast):
            raise ValueError('请使用 ESP32 可访问的局域网 IP 或服务器域名')
        credentials = self.device_credentials(payload)
        return web.json_response({'format': 'deepflipper.connection', 'version': 1,
                                  'server_url': url, **credentials},
                                 headers={'Content-Disposition': 'attachment; filename="deepflipper-connection.json"'})

    async def test_model(self, request):
        from openai import AsyncOpenAI, APIStatusError, APIConnectionError, APITimeoutError
        payload = await self.body(request)
        model = self.store.model(payload)
        if not configured(model['api_key']):
            raise ValueError('请填写 API Key')
        if not configured(model['model_name']):
            raise ValueError('请填写模型名称')
        try:
            async with AsyncOpenAI(api_key=model['api_key'], base_url=model['base_url'],
                                   timeout=15, max_retries=0) as client:
                response = await client.chat.completions.create(
                    model=model['model_name'], max_tokens=32,
                    messages=[{'role': 'user', 'content': 'Call deepflipper_probe to report readiness.'}],
                    tools=[{'type': 'function', 'function': {'name': 'deepflipper_probe',
                        'description': 'Harmless connection test, no hardware action.',
                        'parameters': {'type': 'object', 'properties': {}}}}], tool_choice='auto')
            calls = response.choices[0].message.tool_calls if response.choices else []
            return web.json_response({'ok': True, 'tool_call_returned': any(
                call.function.name == 'deepflipper_probe' for call in (calls or []))})
        except APITimeoutError:
            return web.json_response({'error': '接口连接超时'}, status=504)
        except APIStatusError as error:
            return web.json_response({'error': f'模型接口返回 HTTP {error.status_code}，请检查地址、模型和密钥'}, status=502)
        except APIConnectionError:
            return web.json_response({'error': '无法连接模型接口，请检查地址和网络'}, status=502)

    def app(self):
        app = web.Application(middlewares=[self.security], client_max_size=96 * 1024)
        app.add_routes([
            web.post('/api/login', self.login), web.get('/api/session', self.session),
            web.post('/api/logout', self.logout), web.get('/api/settings', self.settings),
            web.post('/api/settings', self.save), web.get('/api/status', self.status),
            web.get('/api/history', self.history),
            web.post('/api/tts-test', self.test_tts),
            web.post('/api/device-token', self.issue_token), web.post('/api/model-test', self.test_model),
            web.post('/api/connection-profile', self.connection_profile),
        ])
        for route, filename in [('/', 'index.html'), ('/app.js', 'app.js'), ('/style.css', 'style.css')]:
            path = self.root / 'admin' / filename
            async def static(request, path=path):
                return web.FileResponse(path)
            app.router.add_get(route, static)
        return app

    async def start(self, host, port):
        self.runner = web.AppRunner(self.app(), access_log=None)
        await self.runner.setup()
        await web.TCPSite(self.runner, host, port).start()

    async def close(self):
        if self.runner:
            await self.runner.cleanup()
