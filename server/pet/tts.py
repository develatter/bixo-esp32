"""Text to speech with edge-tts, decoded and re-encoded as Opus frames for the device."""

import asyncio
import re

import edge_tts
import numpy as np

from .opus import Encoder

SAMPLE_RATE = 24000
FRAME_MS = 60

_EMOJI = re.compile("[\U0001F000-\U0001FAFF☀-➿️]")
_MARKUP = re.compile(r"[*_#`~>|]")


def speakable(text: str) -> str:
    return re.sub(r"\s+", " ", _MARKUP.sub("", _EMOJI.sub("", text))).strip()


class Speaker:
    def __init__(self, voice: str, rate: str, pitch: str) -> None:
        self.voice = voice
        self.rate = rate
        self.pitch = pitch

    async def _mp3(self, text: str) -> bytes:
        chunks: list[bytes] = []
        stream = edge_tts.Communicate(text, self.voice, rate=self.rate, pitch=self.pitch).stream()
        async for chunk in stream:
            if chunk["type"] == "audio":
                chunks.append(chunk["data"])
        return b"".join(chunks)

    @staticmethod
    async def _decode(mp3: bytes) -> np.ndarray:
        proc = await asyncio.create_subprocess_exec(
            "ffmpeg", "-loglevel", "error", "-i", "pipe:0",
            "-f", "s16le", "-ac", "1", "-ar", str(SAMPLE_RATE), "pipe:1",
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        )
        out, err = await proc.communicate(mp3)
        if proc.returncode != 0:
            raise RuntimeError(f"ffmpeg: {err.decode().strip()}")
        return np.frombuffer(out, dtype=np.int16)

    async def frames(self, text: str) -> list[bytes]:
        """Synthesize one sentence into 60 ms Opus frames at 24 kHz."""
        text = speakable(text)
        if not text:
            return []
        pcm = await self._decode(await self._mp3(text))
        encoder = Encoder(SAMPLE_RATE, FRAME_MS)
        n = encoder.frame_samples
        pcm = np.pad(pcm, (0, -len(pcm) % n))
        return [encoder.encode(pcm[i:i + n]) for i in range(0, len(pcm), n)]
