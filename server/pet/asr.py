"""Speech to text with Parakeet TDT v3 (multilingual) on MLX."""

import asyncio
import logging
import time
from concurrent.futures import ThreadPoolExecutor

import mlx.core as mx
import numpy as np
from parakeet_mlx import from_pretrained
from parakeet_mlx.audio import get_logmel

log = logging.getLogger(__name__)

MODEL_ID = "mlx-community/parakeet-tdt-0.6b-v3"


class Transcriber:
    def __init__(self, model_id: str = MODEL_ID) -> None:
        # MLX is not meant to be driven from several threads: one worker owns the model.
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="asr")
        self._model = self._executor.submit(self._load, model_id).result()

    @staticmethod
    def _load(model_id: str):
        t0 = time.monotonic()
        model = from_pretrained(model_id)
        warmup = np.zeros(16000, dtype=np.int16)
        mel = get_logmel(mx.array(warmup.astype(np.float32) / 32768.0), model.preprocessor_config)
        model.generate(mel)
        log.info("ASR %s ready in %.1fs", model_id, time.monotonic() - t0)
        return model

    def _run(self, pcm16k: np.ndarray) -> str:
        audio = mx.array(pcm16k.astype(np.float32) / 32768.0)
        mel = get_logmel(audio, self._model.preprocessor_config)
        return self._model.generate(mel)[0].text.strip()

    async def transcribe(self, pcm16k: np.ndarray) -> str:
        return await asyncio.get_running_loop().run_in_executor(self._executor, self._run, pcm16k)
