import asyncio
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest.mock import patch
import wave

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'server/main/xiaozhi-server'))
HAS_RUNTIME=all(importlib.util.find_spec(name) for name in ('funasr','sherpa_onnx','opuslib_next'))


@unittest.skipUnless(HAS_RUNTIME,'Full voice dependencies are tested inside the Docker image')
class TtsLifecycleTests(unittest.TestCase):
    def test_timeouts_are_bounded_end_failed_turn_once_and_allow_the_next_turn(self):
        from core.providers.tts.base import TTSProviderBase
        from core.providers.tts.dto.dto import SentenceType
        class Failing(TTSProviderBase):
            calls=0
            async def text_to_speak(self,text,output_file):
                self.calls+=1
                await asyncio.sleep(10)
        tts=Failing({'tts_timeout':0.02,'tts_attempts':2},True)
        tts.conn=types.SimpleNamespace(sentence_id='turn-a')
        tts.current_sentence_id='turn-a'
        started=time.monotonic()
        tts.to_tts_stream('测试')
        self.assertLess(time.monotonic()-started,1)
        self.assertEqual(tts.calls,2)
        self.assertEqual(tts.tts_audio_queue.get_nowait(),(SentenceType.LAST,[],None,'turn-a'))
        tts._fail_current_turn()
        self.assertTrue(tts.tts_audio_queue.empty())
        tts.conn.sentence_id='turn-b';tts.current_sentence_id='turn-b'
        tts._fail_current_turn()
        self.assertEqual(tts.tts_audio_queue.get_nowait()[-1],'turn-b')

    def test_partial_files_are_removed_and_not_announced_as_successful_audio(self):
        from core.providers.tts.base import TTSProviderBase
        class Partial(TTSProviderBase):
            async def text_to_speak(self,text,output_file):
                Path(output_file).write_bytes(b'partial')
                raise OSError('provider failed')
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'partial.wav'
            tts=Partial({'tts_attempts':1},False)
            tts.generate_filename=lambda:str(path)
            tts.conn=types.SimpleNamespace(sentence_id='turn')
            tts.current_sentence_id='turn'
            tts.to_tts_stream('test')
            self.assertFalse(path.exists())
            self.assertEqual(tts.failed_sentence_id,'turn')
            self.assertEqual(tts.tts_audio_queue.qsize(),1)

    def test_local_pcm_is_wav_and_engine_is_not_cancelled_between_sentences(self):
        import numpy as np
        from core.providers.tts import local_melo
        class Engine:
            num_speakers=1
            def generate(self,text,sid,speed,callback):
                self.callback_result=callback(np.ones(20),0.5)
                return types.SimpleNamespace(samples=np.ones(100,dtype=np.float32)*0.1,sample_rate=24000)
        engine=Engine()
        with patch.object(local_melo,'local_model_files',return_value=Path('/models/test')), \
             patch.object(local_melo,'engine',return_value=(engine,threading.Lock())):
            tts=local_melo.TTSProvider({},True)
            data=asyncio.run(tts.text_to_speak('你好',None))
            self.assertEqual(engine.callback_result,1)
            with wave.open(io.BytesIO(data)) as reader:
                self.assertEqual((reader.getnchannels(),reader.getsampwidth(),reader.getframerate(),reader.getnframes()),(1,2,24000,100))

    def test_empty_file_provider_does_not_retry_forever(self):
        from core.providers.tts.base import TTSProviderBase
        class Empty(TTSProviderBase):
            calls=0
            async def text_to_speak(self,text,output_file):
                self.calls+=1
                Path(output_file).touch()
        with tempfile.TemporaryDirectory() as directory:
            tts=Empty({'tts_attempts':2},False)
            tts.generate_filename=lambda:str(Path(directory)/'empty.wav')
            tts.conn=types.SimpleNamespace(sentence_id='turn')
            tts.current_sentence_id='turn'
            tts.to_tts_stream('test')
            self.assertEqual(tts.calls,2)
            self.assertEqual(tts.failed_sentence_id,'turn')

    def test_failed_turn_ends_using_the_existing_device_stop_protocol(self):
        from core.handle.sendAudioHandle import sendAudioMessage
        from core.providers.tts.dto.dto import SentenceType
        class Socket:
            sent=[]
            async def send(self,text):self.sent.append(text)
        logger=types.SimpleNamespace(bind=lambda **kw:logger,info=lambda *args:None,debug=lambda *args:None)
        conn=types.SimpleNamespace(sentence_id='turn',session_id='session',websocket=Socket(),
            tts=types.SimpleNamespace(tts_audio_first_sentence=False),calling=False,close_after_chat=False,
            config={},logger=logger,clearSpeakStatus=lambda:None)
        asyncio.run(sendAudioMessage(conn,SentenceType.LAST,[],None,'turn'))
        import json
        self.assertEqual(json.loads(conn.websocket.sent[-1])['state'],'stop')
