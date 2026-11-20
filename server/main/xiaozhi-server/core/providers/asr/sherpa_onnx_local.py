"""Local SenseVoice / Paraformer ASR on CPU through sherpa-onnx (no PyTorch).

DeepFlipper default: the int8 SenseVoice model (~240 MB, about a quarter of
the RAM of the PyTorch FunASR build) with a bounded thread count, decoded in a
worker thread so the event loop keeps sending audio meanwhile.
"""
import asyncio
import os
import time
from typing import List, Optional, Tuple

import numpy as np
import sherpa_onnx

from config.logger import setup_logging
from core.providers.asr.base import ASRProviderBase
from core.providers.asr.dto.dto import InterfaceType
from core.providers.asr.utils import lang_tag_filter

TAG = __name__
logger = setup_logging()


class ASRProvider(ASRProviderBase):
    def __init__(self, config: dict, delete_audio_file: bool):
        super().__init__()
        self.interface_type = InterfaceType.LOCAL
        self.model_dir = config.get("model_dir")
        self.output_dir = config.get("output_dir", "tmp/")
        self.model_type = config.get("model_type", "sense_voice")
        self.language = config.get("language", "auto")
        self.num_threads = int(config.get("num_threads", 2))
        self.delete_audio_file = delete_audio_file
        os.makedirs(self.output_dir, exist_ok=True)

        model = os.path.join(self.model_dir, "model.int8.onnx")
        tokens = os.path.join(self.model_dir, "tokens.txt")
        for path in (model, tokens):
            if not os.path.isfile(path):
                # Models are pinned and verified by scripts/download_models.py.
                raise FileNotFoundError(f"Local ASR model missing: {path}")

        if self.model_type == "paraformer":
            self.model = sherpa_onnx.OfflineRecognizer.from_paraformer(
                paraformer=model,
                tokens=tokens,
                num_threads=self.num_threads,
                sample_rate=16000,
                feature_dim=80,
                decoding_method="greedy_search",
                debug=False,
            )
        else:
            self.model = sherpa_onnx.OfflineRecognizer.from_sense_voice(
                model=model,
                tokens=tokens,
                num_threads=self.num_threads,
                language="" if self.language == "auto" else self.language,
                use_itn=True,
                debug=False,
            )

    def _decode(self, pcm_bytes: bytes) -> dict:
        samples = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32) / 32768.0
        stream = self.model.create_stream()
        stream.accept_waveform(16000, samples)
        self.model.decode_stream(stream)
        result = stream.result
        # SenseVoice reports language/emotion as "<|zh|>" style tags; keep the
        # same dict shape as the old FunASR provider for downstream code.
        tags = (getattr(result, "lang", "") or "") + (getattr(result, "emotion", "") or "")
        return lang_tag_filter(tags + result.text)

    async def speech_to_text(
        self, opus_data: List[bytes], session_id: str, artifacts=None
    ) -> Tuple[Optional[dict], Optional[str]]:
        if artifacts is None or not artifacts.pcm_bytes:
            return "", None
        try:
            started = time.monotonic()
            text = await asyncio.to_thread(self._decode, artifacts.pcm_bytes)
            logger.bind(tag=TAG).debug(
                f"语音识别耗时: {time.monotonic() - started:.3f}s | 结果: {text.get('content')}"
            )
            return text, artifacts.file_path
        except Exception as error:
            logger.bind(tag=TAG).error(f"语音识别失败: {error}", exc_info=True)
            return "", artifacts.file_path
