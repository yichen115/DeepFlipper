"""Exercise the real WS admission path without loading speech/model providers."""
import importlib.util
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
from core.auth import AuthManager


class QuietLogger:
    def bind(self, **kwargs): return self
    def debug(self, *args): pass
    def info(self, *args): pass
    def error(self, *args): pass
    def warning(self, *args): pass


def load_server():
    stubs = {}
    for name in ('websockets', 'config.logger', 'core.connection', 'config.config_loader',
                 'core.utils.modules_initialize', 'core.utils.util'):
        stubs[name] = types.ModuleType(name)
    stubs['websockets'].ServerConnection = object
    stubs['config.logger'].setup_logging = lambda *args: QuietLogger()
    stubs['config.config_loader'].reload_local_config = None
    stubs['core.utils.modules_initialize'].initialize_modules = lambda *args: {}
    stubs['core.utils.util'].check_vad_update = None
    stubs['core.utils.util'].check_asr_update = None

    class Connection:
        def __init__(self, *args): pass
        async def handle_connection(self, socket): socket.admitted = True
    stubs['core.connection'].ConnectionHandler = Connection
    path = ROOT / 'server/main/xiaozhi-server/core/websocket_server.py'
    spec = importlib.util.spec_from_file_location('tested_websocket_server', path)
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, stubs):
        spec.loader.exec_module(module)
    return module.WebSocketServer


class Socket:
    def __init__(self, headers):
        self.request = types.SimpleNamespace(headers=headers, path='/xiaozhi/v1/')
        self.admitted = False
        self.closed = False
        self.messages = []
    async def close(self): self.closed = True
    async def send(self, message): self.messages.append(message)


class WebsocketHeaderTests(unittest.IsolatedAsyncioTestCase):
    async def test_valid_esp32_and_lowercase_headers_reach_the_handler(self):
        key = 'test-key-for-header-case-regression'
        token = AuthManager(key).generate_token('client-a', 'device-a')
        server = load_server()({'selected_module': {}, 'server': {'auth_key': key}})
        for device, client in [('Device-Id', 'Client-Id'), ('device-id', 'client-id'),
                               ('DEVICE-ID', 'CLIENT-ID')]:
            with self.subTest(header=device):
                socket = Socket({device: 'device-a', client: 'client-a',
                                 'Authorization': 'Bearer ' + token})
                await server._handle_connection(socket)
                self.assertTrue(socket.admitted)
                self.assertEqual(socket.messages, [])

    async def test_mixed_case_identity_still_requires_a_valid_token(self):
        server = load_server()({'selected_module': {}, 'server': {
            'auth_key': 'test-key-for-header-case-regression',
            'auth': {'allowed_devices': ['device-a']}}})
        for authorization in ('', 'Bearer invalid'):
            socket = Socket({'Device-Id': 'device-a', 'Client-Id': 'client-a',
                             'Authorization': authorization})
            await server._handle_connection(socket)
            self.assertFalse(socket.admitted)
            self.assertTrue(socket.closed)
            self.assertEqual(socket.messages, ['认证失败'])
