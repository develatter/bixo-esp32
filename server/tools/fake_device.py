"""Pretend to be a xiaozhi device: OTA check, WebSocket hello, speak a WAV, record the reply.

    PET_SECRET=... uv run python tools/fake_device.py question.wav --out reply.wav [--url http://localhost:8003]
"""

import argparse
import asyncio
import json
import os
import subprocess
import sys
import time
import wave
from pathlib import Path

import aiohttp
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from pet.opus import Decoder, Encoder  # noqa: E402

DEVICE_ID = "02:00:00:00:00:01"
MIC_RATE = 16000


def load_pcm(path: str) -> np.ndarray:
    out = subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-i", path, "-f", "s16le", "-ac", "1", "-ar", str(MIC_RATE), "pipe:1"],
        check=True, capture_output=True,
    ).stdout
    return np.frombuffer(out, dtype=np.int16)


async def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--out", default="reply.wav")
    ap.add_argument("--url", default="http://localhost:8003")
    args = ap.parse_args()

    headers = {"Device-Id": DEVICE_ID, "Client-Id": "fake-device"}
    t0 = time.monotonic()

    def stamp(text: str) -> None:
        print(f"{time.monotonic() - t0:6.2f}s  {text}")

    async with aiohttp.ClientSession() as http:
        async with http.post(f"{args.url}/xiaozhi/ota/{os.environ['PET_SECRET']}/", json={"application": {"version": "2.5.2"}}, headers=headers) as r:
            ota = await r.json()
        stamp(f"OTA -> {ota['websocket']['url']}")

        auth = {"Authorization": f"Bearer {ota['websocket']['token']}", "Protocol-Version": "1"}
        async with http.ws_connect(ota["websocket"]["url"], headers={**headers, **auth}) as ws:
            await ws.send_json({"type": "hello", "version": 1, "features": {"mcp": False}, "transport": "websocket",
                                "audio_params": {"format": "opus", "sample_rate": MIC_RATE, "channels": 1, "frame_duration": 60}})
            hello = await ws.receive_json()
            stamp(f"hello <- {hello}")
            out_rate = hello["audio_params"]["sample_rate"]
            session_id = hello.get("session_id", "")
            await ws.send_json({"session_id": session_id, "type": "listen", "state": "start", "mode": "auto"})

            async def speak() -> None:
                enc = Encoder(MIC_RATE, voice=True)
                n = enc.frame_samples
                pcm = load_pcm(args.wav)
                pcm = np.concatenate([np.zeros(n * 5, np.int16), pcm, np.zeros(n * 40, np.int16)])
                pcm = np.pad(pcm, (0, -len(pcm) % n))
                for i in range(0, len(pcm), n):
                    if ws.closed:
                        return
                    await ws.send_bytes(enc.encode(pcm[i:i + n]))
                    await asyncio.sleep(0.06)

            speaker = asyncio.create_task(speak())
            dec = Decoder(out_rate)
            audio: list[np.ndarray] = []
            async for msg in ws:
                if msg.type == aiohttp.WSMsgType.BINARY:
                    if not audio:
                        stamp("first audio frame")
                    audio.append(dec.decode(msg.data))
                elif msg.type == aiohttp.WSMsgType.TEXT:
                    data = json.loads(msg.data)
                    data.pop("session_id", None)
                    stamp(f"<- {data}")
                    if data.get("type") == "tts" and data.get("state") == "stop":
                        break
            speaker.cancel()

    pcm = np.concatenate(audio) if audio else np.zeros(0, np.int16)
    with wave.open(args.out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(out_rate)
        w.writeframes(pcm.tobytes())
    stamp(f"wrote {args.out} ({len(pcm) / out_rate:.1f}s of audio)")


if __name__ == "__main__":
    asyncio.run(main())
