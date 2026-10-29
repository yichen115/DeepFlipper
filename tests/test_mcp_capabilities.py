import importlib.util
import json
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


class QuietLogger:
    def bind(self, **kwargs): return self
    def debug(self, *args): pass
    def info(self, *args): pass
    def error(self, *args): pass
    def warning(self, *args): pass


def load_handler(vision_url, signer):
    package = types.ModuleType('core.providers.tools.device_mcp')
    package.__path__ = []
    client = types.ModuleType('core.providers.tools.device_mcp.mcp_client')
    client.MCPClient = object
    util = types.ModuleType('core.utils.util')
    util.get_vision_url = lambda config: vision_url
    auth = types.ModuleType('core.utils.auth')
    auth.AuthToken = signer
    logger = types.ModuleType('config.logger')
    logger.setup_logging = lambda: QuietLogger()
    name = 'core.providers.tools.device_mcp.tested_handler'
    spec = importlib.util.spec_from_file_location(name, ROOT / 'server/main/xiaozhi-server/core/providers/tools/device_mcp/mcp_handler.py')
    module = importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules, {'core.providers.tools.device_mcp': package,
                                 'core.providers.tools.device_mcp.mcp_client': client,
                                 'core.utils.util': util, 'core.utils.auth': auth,
                                 'config.logger': logger}):
        spec.loader.exec_module(module)
    return module


class MCPCapabilityTests(unittest.IsolatedAsyncioTestCase):
    def connection(self):
        class Socket:
            def __init__(self): self.messages = []
            async def send(self, value): self.messages.append(json.loads(value))
        return types.SimpleNamespace(config={'server': {}}, headers={'device-id': 'device-a'},
                                     features={'mcp': True}, websocket=Socket())

    async def test_voice_only_keeps_mcp_but_does_not_advertise_vision_or_sign_token(self):
        def no_signer(*args): raise AssertionError('Disabled vision must not generate a token')
        handler = load_handler('', no_signer)
        conn = self.connection()
        await handler.send_mcp_initialize_message(conn)
        await handler.send_mcp_tools_list_request(conn)
        initialize, tools = conn.websocket.messages
        self.assertEqual(initialize['payload']['method'], 'initialize')
        capabilities = initialize['payload']['params']['capabilities']
        self.assertNotIn('vision', capabilities)
        self.assertIn('roots', capabilities)
        self.assertIn('sampling', capabilities)
        self.assertEqual(tools['payload']['method'], 'tools/list')

    async def test_explicit_external_vision_service_retains_its_signed_capability(self):
        class Signer:
            def __init__(self, key): self.key = key
            def generate_token(self, device): return 'test-only-token-for-' + device
        handler = load_handler('https://vision.example.test/explain', Signer)
        conn = self.connection()
        conn.config['server']['auth_key'] = 'test-only-key'
        await handler.send_mcp_initialize_message(conn)
        vision = conn.websocket.messages[0]['payload']['params']['capabilities']['vision']
        self.assertEqual(vision['url'], 'https://vision.example.test/explain')
        self.assertEqual(vision['token'], 'test-only-token-for-device-a')
