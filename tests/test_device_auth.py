import importlib.util
from pathlib import Path
import sys
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'server/main/xiaozhi-server'))
from core.auth import AuthManager, AuthenticationError
from core.device_auth import authorize_device


class DeviceAuthTests(unittest.TestCase):
    def setUp(self):
        self.auth=AuthManager('unit-test-key-not-a-real-credential',expire_seconds=60)
        self.token=self.auth.generate_token('client-a','device-a')
        self.headers={'device-id':'device-a','client-id':'client-a',
                      'Authorization':'Bearer '+self.token}

    def test_valid_device(self):
        self.assertEqual(authorize_device(self.auth,self.headers),('device-a','client-a'))

    def test_allowlist_does_not_bypass_token(self):
        self.headers.pop('Authorization')
        with self.assertRaises(AuthenticationError):
            authorize_device(self.auth,self.headers,{'device-a'})

    def test_unenrolled_rejected(self):
        with self.assertRaises(AuthenticationError):
            authorize_device(self.auth,self.headers,{'other-device'})

    def test_revoked_signed_device_rejected(self):
        with self.assertRaises(AuthenticationError):
            authorize_device(self.auth,self.headers,revoked_devices={'device-a'})

    def test_device_or_client_cannot_be_changed(self):
        for field in ('device-id','client-id'):
            with self.subTest(field=field),self.assertRaises(AuthenticationError):
                authorize_device(self.auth,{**self.headers,field:'other'})

    def test_expired_and_future_tokens(self):
        for delta in (-120,120):
            with patch('core.auth.time.time',return_value=time.time()+delta):
                token=self.auth.generate_token('client-a','device-a')
            self.assertFalse(self.auth.verify_token(token,'client-a','device-a'))

    def test_missing_identity_and_malformed_token(self):
        self.assertFalse(self.auth.verify_token(self.token,'','device-a'))
        self.assertFalse(self.auth.verify_token('bad','client-a','device-a'))


if __name__=='__main__':
    unittest.main()
