"""Offline Chinese/English speech using the existing sherpa-onnx CPU runtime."""
import asyncio
from functools import lru_cache
import io
import os
from pathlib import Path
import re
import threading
import time
import wave

import numpy as np

from config.config_loader import get_project_dir
from core.providers.tts.base import TTSProviderBase
from core.tts_settings import local_model_files
from core.utils.speech_level import normalize_speech

_SD_PATH_RE = re.compile(
    r'(?:/ext/)?(?:nfc|subghz|infrared|lfrfid|ibutton|apps)/[A-Za-z0-9_./-]+'
    r'|/ext/[A-Za-z0-9_./-]+'
    r'|[A-Za-z0-9_-]+_\d{6,}_\d+\.[a-z]{2,4}'
)
_ENG_WORD_RE = re.compile(r'[A-Za-z]+')
_HEX_UID_RE = re.compile(r'\b(?:[0-9A-Fa-f]{2}\s+){2,7}[0-9A-Fa-f]{2}\b|\b(?=[0-9A-Fa-f]*[A-Fa-f])(?=[0-9A-Fa-f]*\d)[0-9A-Fa-f]{6,16}\b')
_LETTER_DIGIT_RE = re.compile(r'([A-Za-z])(\d)|(\d)([A-Za-z])')
_CN_EN_RE = re.compile(r'([\u4e00-\u9fff])\s*([A-Za-z])')
_EN_CN_RE = re.compile(r'([A-Za-z])\s*([\u4e00-\u9fff])')
_CLAUSE_SPLIT_RE = re.compile(r'([。！？!?\n；;，,：:])')
_SENTENCE_END_PUNCTS = frozenset('。！？!?\n；;')
_COMPOUND_TERMS = {
    'deepflipper': 'deep flipper',
    'mifare': 'mi fare',
    'ibutton': 'i button',
    'subghz': 'sub giga hertz',
    'ghz': 'giga hertz',
    'mhz': 'mega hertz',
    'khz': 'kilo hertz',
    'wifi': 'w、i、f、i',
}


@lru_cache(maxsize=2)
def _lexicon_words(model_dir: str) -> frozenset:
    path = Path(model_dir) / 'lexicon.txt'
    if not path.is_file():
        return frozenset()
    words = set()
    with path.open(encoding='utf-8', errors='ignore') as f:
        for line in f:
            parts = line.split(None, 1)
            if parts:
                words.add(parts[0])
    return frozenset(words)


def _format_hex_uid(match: re.Match) -> str:
    raw = re.sub(r'\s+', '', match.group(0)).lower()
    pairs = [' '.join(raw[i:i + 2]) for i in range(0, len(raw), 2)]
    return '、'.join(pairs)


def _prepare_spoken_text(text: str, model_dir: str = '') -> str:
    text = _SD_PATH_RE.sub(' S D 卡文件 ', text)
    text = (text.replace('Wi-Fi', 'wifi').replace('wi-fi', 'wifi').replace('Sub-GHz', 'sub ghz')
            .replace('——', '，').replace('—', '，'))
    text = _HEX_UID_RE.sub(_format_hex_uid, text)
    text = _LETTER_DIGIT_RE.sub(lambda m: f'{m.group(1) or m.group(3)} {m.group(2) or m.group(4)}', text)
    text = _LETTER_DIGIT_RE.sub(lambda m: f'{m.group(1) or m.group(3)} {m.group(2) or m.group(4)}', text)
    vocab = _lexicon_words(model_dir) if model_dir else frozenset()

    def _norm_word(match: re.Match) -> str:
        word = match.group(0)
        low = word.lower()
        if low in _COMPOUND_TERMS:
            return _COMPOUND_TERMS[low]
        if len(word) == 1:
            return low
        if word.isupper() and len(word) <= 4:
            return '、'.join(low)
        if vocab and low not in vocab:
            return '、'.join(low)
        return low
    out = _ENG_WORD_RE.sub(_norm_word, text).strip()
    # Add a slight prosodic pause at Chinese<->English transitions so English words/acronyms
    # (like UID, NFC, Mifare) never collide into adjacent Chinese syllables without a gap.
    out = _CN_EN_RE.sub(r'\1、\2', out)
    out = _EN_CN_RE.sub(r'\1、\2', out)
    if out and out[-1] not in '。！？!?.':
        out += '。'
    return out


def _split_sentences(spoken: str) -> list[tuple[str, float]]:
    parts = _CLAUSE_SPLIT_RE.split(spoken)
    clauses = []
    current = ''
    for token in parts:
        if not token:
            continue
        if token in _SENTENCE_END_PUNCTS or token in '，,：:':
            clause = current.strip(' 、')
            if clause:
                pause_sec = 0.35 if token in _SENTENCE_END_PUNCTS else 0.22
                clauses.append((clause + '。', pause_sec))
            current = ''
        else:
            current += token
    tail = current.strip(' 、')
    if tail:
        clauses.append((tail + '。', 0.12))
    return clauses or [(spoken, 0.12)]


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
        self.model_dir = str(directory)
        default_threads = min(6, max(2, (os.cpu_count() or 4) // 2))
        threads = max(1, min(8, int(config.get('num_threads', default_threads))))
        self.engine, self.engine_lock = engine(self.model_dir, threads)
        self.audio_file_type = 'wav'
        self.speaker = int(config.get('speaker_id', 0))
        self.speed = float(config.get('speed', 1.0))
        if not 0.5 <= self.speed <= 2 or not 0 <= self.speaker < self.engine.num_speakers:
            raise ValueError('Invalid local TTS speaker or speed')

    def _wav(self, text):
        if not isinstance(text, str) or not text.strip() or len(text) > 2000:
            raise ValueError('TTS text must contain 1..2000 characters')
        spoken = _prepare_spoken_text(text, self.model_dir)
        deadline = time.monotonic() + self.tts_timeout
        if not self.engine_lock.acquire(timeout=self.tts_timeout):
            raise TimeoutError('Local TTS is busy')
        try:
            def cancelled(samples, progress):
                return int(time.monotonic() >= deadline or
                           bool(self.conn and (self.conn.stop_event.is_set() or self.conn.client_abort)))
            sentences = _split_sentences(spoken)
            chunks = []
            sample_rate = 24000
            for idx, (sent, pause_sec) in enumerate(sentences):
                # sherpa's native callback returns 1 to continue and 0 to stop.
                audio = self.engine.generate(sent, sid=self.speaker, speed=self.speed,
                                             callback=lambda samples, progress: int(not cancelled(samples, progress)))
                if cancelled(None, None):
                    raise TimeoutError('Local TTS cancelled or timed out')
                part = np.asarray(audio.samples, dtype=np.float32)
                if not part.size or not np.isfinite(part).all():
                    continue
                sample_rate = int(audio.sample_rate)
                if not chunks and part.size >= max(160, sample_rate // 10):
                    # Ensure utterance head has a 140ms lead-in even when starting with English.
                    chunks.append(np.zeros(int(sample_rate * 0.14), dtype=np.float32))
                chunks.append(part)
                if idx < len(sentences) - 1 and part.size >= max(160, sample_rate // 10):
                    chunks.append(np.zeros(int(sample_rate * pause_sec), dtype=np.float32))
            if not chunks:
                raise ValueError('Local TTS generated no valid audio')
            samples = np.concatenate(chunks) if len(chunks) > 1 else chunks[0]
            samples = normalize_speech(samples)
            buffer = io.BytesIO()
            with wave.open(buffer, 'wb') as output:
                output.setnchannels(1)
                output.setsampwidth(2)
                output.setframerate(sample_rate)
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
