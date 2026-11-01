import importlib.util
import io
import json
from pathlib import Path
import sys
import types
import unittest
import wave
from unittest.mock import AsyncMock, MagicMock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'server/main/xiaozhi-server'))
HAS_RUNTIME = all(importlib.util.find_spec(name) for name in ('funasr', 'opuslib_next'))


@unittest.skipUnless(HAS_RUNTIME, 'Full audio runtime is tested in Docker')
class AudioDiagnosticsTests(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        with patch('config.logger.setup_logging', return_value=MagicMock()), \
                patch('core.utils.wakeup_word.WakeupWordsConfig._ensure_directories'):
            from core.handle.helloHandle import handleHelloMessage
        cls.hello = staticmethod(handleHelloMessage)

    async def test_hello_describes_downlink_encoder_without_changing_microphone_format(self):
        params = {'format': 'opus', 'sample_rate': 16000, 'channels': 1, 'frame_duration': 60}
        conn = types.SimpleNamespace(logger=MagicMock(), sample_rate=24000, websocket=AsyncMock(),
            welcome_msg={'type': 'hello', 'audio_params': {}}, audio_format='opus')
        await self.hello(conn, {'audio_params': params, 'features': {}})
        reply = json.loads(conn.websocket.send.call_args.args[0])
        self.assertEqual(reply['audio_params']['sample_rate'], 24000)
        self.assertEqual(reply['audio_params']['frame_duration'], 60)
        self.assertEqual(params['sample_rate'], 16000)

    async def test_opus_preview_has_correct_duration_channels_and_frequency(self):
        import numpy as np
        from core.audio_diagnostics import opus_preview
        source_rate = 44100
        pcm = (np.sin(2*np.pi*440*np.arange(source_rate)/source_rate)*8000).astype('<i2')
        data = io.BytesIO()
        with wave.open(data, 'wb') as output:
            output.setnchannels(1);output.setsampwidth(2);output.setframerate(source_rate)
            output.writeframes(pcm.tobytes())
        restored = opus_preview(data.getvalue(), 'wav', 24000)
        with wave.open(io.BytesIO(restored), 'rb') as result:
            self.assertEqual((result.getframerate(),result.getnchannels(),result.getsampwidth()),(24000,1,2))
            self.assertGreaterEqual(result.getnframes(),24000)
            self.assertLess(result.getnframes(),24000+1440)
            samples = np.frombuffer(result.readframes(result.getnframes()), dtype='<i2').astype(float)
        spectrum = np.abs(np.fft.rfft(samples[2400:20000]))
        frequency = np.fft.rfftfreq(17600,1/24000)[np.argmax(spectrum)]
        self.assertAlmostEqual(frequency,440,delta=5)
