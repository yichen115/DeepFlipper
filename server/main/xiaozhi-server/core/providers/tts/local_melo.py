"""Offline Chinese/English speech using the existing sherpa-onnx CPU runtime."""
import asyncio
from functools import lru_cache
import io
from pathlib import Path
import threading
import time
import wave

import numpy as np

from config.config_loader import get_project_dir
from core.providers.tts.base import TTSProviderBase
from core.tts_settings import local_model_files
from core.utils.speech_level import normalize_speech


@lru_cache(maxsize=2)
def engine(model_dir, threads):
    import sherpa_onnx
    directory = Path(model_dir)
    config = sherpa_onnx.OfflineTtsConfig(
        model=sherpa_onnx.OfflineTtsModelConfig(
            vits=sherpa_onnx.OfflineTtsVitsModelConfig(
                model=str(directory / 'model.onnx'), tokens=str(directory / 'tokens.txt'),
                lexicon=str(directory / 'lexicon.txt'), dict_dir=str(directory / 'dict')),
            num_threads=threads, provider='cpu', debug=False),
        rule_fsts=','.join(str(directory / name) for name in ('date.fst', 'number.fst', 'phone.fst')
                          if (directory / name).is_file()), max_num_sentences=1)
    if not config.validate():
        raise ValueError('Invalid local TTS model configuration')
    return sherpa_onnx.OfflineTts(config), threading.Lock()


class TTSProvider(TTSProviderBase):
    def __init__(self, config, delete_audio_file=True):
        super().__init__(config, delete_audio_file)
        directory = local_model_files(get_project_dir(), config)
        self.engine, self.engine_lock = engine(str(directory), max(1, min(4, int(config.get('num_threads', 2)))))
        self.audio_file_type = 'wav'
        self.speaker = int(config.get('speaker_id', 0))
        self.speed = float(config.get('speed', 1.0))
        if not 0.5 <= self.speed <= 2 or not 0 <= self.speaker < self.engine.num_speakers:
            raise ValueError('Invalid local TTS speaker or speed')

    def _wav(self, text):
        if not isinstance(text, str) or not text.strip() or len(text) > 2000:
            raise ValueError('TTS text must contain 1..2000 characters')
        deadline = time.monotonic() + self.tts_timeout
        if not self.engine_lock.acquire(timeout=self.tts_timeout):
            raise TimeoutError('Local TTS is busy')
        try:
            def cancelled(samples, progress):
                return int(time.monotonic() >= deadline or
                           bool(self.conn and (self.conn.stop_event.is_set() or self.conn.client_abort)))
            # sherpa's native callback returns 1 to continue and 0 to stop.
            audio = self.engine.generate(text, sid=self.speaker, speed=self.speed,
                                         callback=lambda samples, progress: int(not cancelled(samples, progress)))
            if cancelled(None, None):
                raise TimeoutError('Local TTS cancelled or timed out')
            samples = np.asarray(audio.samples, dtype=np.float32)
            if not samples.size or not np.isfinite(samples).all():
                raise ValueError('Local TTS generated no valid audio')
            samples = normalize_speech(samples)
            buffer = io.BytesIO()
            with wave.open(buffer, 'wb') as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(audio.sample_rate)
                output.writeframes((np.clip(samples, -1, 1) * 32767).astype('<i2').tobytes())
            return buffer.getvalue()
        finally:
            self.engine_lock.release()

    async def text_to_speak(self, text, output_file):
        data = await asyncio.to_thread(self._wav, text)
        if output_file:
            Path(output_file).parent.mkdir(parents=True, exist_ok=True)
            Path(output_file).write_bytes(data)
        else:
            return data
