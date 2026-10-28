"""Exercise the real configuration route without loading speech/model providers."""
import importlib.util
import json
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'server/main/xiaozhi-server'))
from core.auth import AuthManager


class QuietLogger:
    def bind(self,**kwargs): return self
    def debug(self,*args): pass
    def info(self,*args): pass
    def error(self,*args): pass
    def warning(self,*args): pass


class BaseHandler:
    def __init__(self,config):
        self.config=config
        self.logger=QuietLogger()
    def _add_cors_headers(self,response):
        response.headers['X-Test-Cors']='yes'


def load_handler():
    util=types.ModuleType('core.utils.util')
    util.get_local_ip=lambda:'127.0.0.1'
    util.get_vision_url=lambda *args:'https://example.test/vision'
    base=types.ModuleType('core.api.base_handler')
    base.BaseHandler=BaseHandler
    p=ROOT/'server/main/xiaozhi-server/core/api/ota_handler.py'
    spec=importlib.util.spec_from_file_location('tested_ota_handler',p)
    module=importlib.util.module_from_spec(spec)
    with patch.dict(sys.modules,{'core.utils.util':util,'core.api.base_handler':base}):
        spec.loader.exec_module(module)
    return module.OTAHandler


class ConfigurationAuthTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        try:
            self.cls=load_handler()
        except ModuleNotFoundError as exc:
            if exc.name=='aiohttp': self.skipTest('Install the backend aiohttp requirement')
            raise
        self.key='test-only-signing-key-not-a-real-secret'
        self.config={'server':{'auth_key':self.key,'auth':{'enabled':True},
                               'websocket':'wss://voice.example.test/xiaozhi/v1/'}}

    async def test_unauthenticated_request_is_401_before_body_read(self):
        handler=self.cls(self.config)
        class Request:
            headers={}
            async def text(self):
                raise AssertionError('Unauthorized request body must not be consumed')
        response=await handler.handle_post(Request())
        self.assertEqual(response.status,401)

    async def test_allowlisted_id_still_cannot_mint_anonymous_token(self):
        self.config['server']['auth']['allowed_devices']=['device-a']
        handler=self.cls(self.config)
        class Request:
            headers={'device-id':'device-a','client-id':'client-a'}
            async def text(self):
                raise AssertionError('Missing bearer must fail first')
        response=await handler.handle_post(Request())
        self.assertEqual(response.status,401)
        self.assertNotIn('token',json.loads(response.text))
