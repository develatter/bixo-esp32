"""xiaozhi-compatible backend: OTA bootstrap over HTTP and the voice session over WebSocket."""

import asyncio
import hashlib
import hmac
import json
import logging
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime
from zoneinfo import ZoneInfo

import numpy as np
import webrtcvad
from aiohttp import WSMsgType, web

from .asr import Transcriber
from .brain import Brain
from .opus import Decoder
from .tts import FRAME_MS, SAMPLE_RATE, Speaker

log = logging.getLogger(__name__)

MIC_RATE = 16000
VAD_FRAME = MIC_RATE * 30 // 1000  # webrtcvad works on 10/20/30 ms frames
FRAME_S = FRAME_MS / 1000
PLAYBACK_LEAD_S = 0.3  # how far ahead of real time we let the device's buffer fill
# What the device reports when the real wake word fires; any other "detect" text is a command
# from the device itself (the camera's "<detect>1 person detected </detect>") and is answered
# as if heard.
WAKE_WORDS = {"hola bicho", "hi esp", "你好小智"}
# The menu's "Nueva charla": ends the conversation and greets in a new one.
NEW_CHAT_COMMAND = "<nueva charla>"
NEW_CHAT_GREETING = "Empecemos una charla nueva."


@dataclass
class Config:
    secret: str                    # path segment of the OTA URL baked into the firmware
    public_url: str                # e.g. wss://host.ts.net; empty = ws:// on whatever host was asked
    allowed_devices: frozenset[str]
    timezone: str
    vad_aggressiveness: int
    speech_start_ms: int
    speech_end_ms: int
    max_utterance_s: int
    idle_timeout_s: int


@dataclass
class Services:
    config: Config
    asr: Transcriber
    brain: Brain
    speaker: Speaker
    sessions: dict[str, "Session"] = field(default_factory=dict)  # device id -> live session
    photos: dict[str, bytes] = field(default_factory=dict)        # device id -> last camera JPEG
    active_device: str = ""                                        # who spoke last

    def current_session(self) -> "Session | None":
        """The Watcher Hermes is talking to: the one that spoke last, else any connected one."""
        session = self.sessions.get(self.active_device)
        return session or next(iter(self.sessions.values()), None)


class Session:
    def __init__(self, ws: web.WebSocketResponse, device_id: str, services: Services, vision_url: str) -> None:
        self.ws = ws
        self.device_id = device_id
        self.s = services
        self.vision_url = vision_url
        self.command: str | None = None
        self._mcp_id = 100
        self._mcp_waiters: dict[int, asyncio.Future] = {}
        self.session_id = uuid.uuid4().hex
        self.decoder = Decoder(MIC_RATE)
        self.vad = webrtcvad.Vad(services.config.vad_aggressiveness)
        self.mode = "auto"
        self.listening = False
        self.respond_task: asyncio.Task | None = None
        self.mcp_tools: list[str] = []
        self._reset_utterance()
        self.last_voice = time.monotonic()

    def _reset_utterance(self) -> None:
        self.pcm: list[np.ndarray] = []
        self.pending = np.zeros(0, dtype=np.int16)
        self.voiced_run_ms = 0
        self.silence_ms = 0
        self.in_speech = False

    async def send(self, **message) -> None:
        if not self.ws.closed:
            await self.ws.send_str(json.dumps({"session_id": self.session_id, **message}, ensure_ascii=False))

    # --- device -> server -------------------------------------------------------------------

    async def on_json(self, msg: dict) -> None:
        kind = msg.get("type")
        if kind == "hello":
            await self.send(
                type="hello", transport="websocket",
                audio_params={"format": "opus", "sample_rate": SAMPLE_RATE, "channels": 1, "frame_duration": FRAME_MS},
            )
            if msg.get("features", {}).get("mcp"):
                # The camera uploads its photos to vision_url with the same token as the WebSocket.
                vision = {"url": self.vision_url, "token": device_token(self.s, self.device_id)}
                await self.send(type="mcp", payload={"jsonrpc": "2.0", "method": "initialize",
                                                     "params": {"capabilities": {"vision": vision}}, "id": 1})
                await self.send(type="mcp", payload={"jsonrpc": "2.0", "method": "tools/list", "params": {"cursor": ""}, "id": 2})
        elif kind == "listen":
            state = msg.get("state")
            if state == "start":
                self.mode = msg.get("mode", "auto")
                if self.command:
                    text, self.command = self.command, None
                    await self.answer(text)
                else:
                    self.start_listening()
            elif state == "stop":
                await self.finish_utterance()
            elif state == "detect":
                text = msg.get("text", "")
                if text.strip().lower() in WAKE_WORDS:
                    log.info("[%s] wake word: %s", self.device_id, text)
                elif text == NEW_CHAT_COMMAND:
                    self.s.brain.forget(self.device_id)
                    self.command = NEW_CHAT_GREETING
                else:
                    log.info("[%s] command from the device: %s", self.device_id, text)
                    self.command = text
        elif kind == "abort":
            log.info("[%s] abort (%s)", self.device_id, msg.get("reason"))
            await self.cancel_response()
        elif kind == "mcp":
            payload = msg.get("payload", {})
            waiter = self._mcp_waiters.pop(payload.get("id"), None)
            if waiter is not None and not waiter.done():
                waiter.set_result(payload)
                return
            result = payload.get("result", {})
            if "tools" in result:
                self.mcp_tools = [t["name"] for t in result["tools"]]
                log.info("[%s] device tools: %s", self.device_id, ", ".join(self.mcp_tools))
        else:
            log.debug("[%s] ignored %s", self.device_id, msg)

    def start_listening(self) -> None:
        self.listening = True
        self.last_voice = time.monotonic()
        self._reset_utterance()

    async def on_audio(self, packet: bytes) -> None:
        if not self.listening:
            return
        pcm = self.decoder.decode(packet)
        self.pcm.append(pcm)
        if self.mode == "manual":
            return  # the device tells us when the user is done

        cfg = self.s.config
        self.pending = np.concatenate([self.pending, pcm])
        while len(self.pending) >= VAD_FRAME:
            frame, self.pending = self.pending[:VAD_FRAME], self.pending[VAD_FRAME:]
            if self.vad.is_speech(frame.tobytes(), MIC_RATE):
                self.voiced_run_ms += 30
                self.silence_ms = 0
                if self.voiced_run_ms >= cfg.speech_start_ms:
                    self.in_speech = True
                    self.last_voice = time.monotonic()
            else:
                self.voiced_run_ms = 0
                self.silence_ms += 30

        seconds = sum(len(p) for p in self.pcm) / MIC_RATE
        if self.in_speech and (self.silence_ms >= cfg.speech_end_ms or seconds >= cfg.max_utterance_s):
            await self.finish_utterance()
        elif not self.in_speech and seconds > 3:
            self.pcm = self.pcm[-25:]  # keep ~1.5 s of pre-roll while waiting for speech

    async def finish_utterance(self) -> None:
        if not self.listening:
            return
        self.listening = False
        audio = np.concatenate(self.pcm) if self.pcm else np.zeros(0, dtype=np.int16)
        self._reset_utterance()
        if len(audio) < MIC_RATE // 4:
            self.start_listening()
            return
        t0 = time.monotonic()
        text = await self.s.asr.transcribe(audio)
        log.info("[%s] heard (%.2fs audio, %.2fs asr): %r", self.device_id, len(audio) / MIC_RATE, time.monotonic() - t0, text)
        if not text:
            self.start_listening()
            return
        await self.answer(text)

    async def answer(self, text: str) -> None:
        self.listening = False
        self.s.active_device = self.device_id
        await self.send(type="stt", text=text)
        await self.send(type="llm", emotion="thinking", text="")
        self.respond_task = asyncio.create_task(self.respond(text))

    async def call_tool(self, name: str, arguments: dict, timeout: float = 15) -> str:
        """Run one of the device's own MCP tools and return its text result."""
        if name not in self.mcp_tools:
            raise RuntimeError(f"El Watcher no tiene la herramienta {name}.")
        self._mcp_id += 1
        waiter = asyncio.get_running_loop().create_future()
        self._mcp_waiters[self._mcp_id] = waiter
        await self.send(type="mcp", payload={"jsonrpc": "2.0", "method": "tools/call", "id": self._mcp_id,
                                             "params": {"name": name, "arguments": arguments}})
        try:
            payload = await asyncio.wait_for(waiter, timeout)
        finally:
            self._mcp_waiters.pop(self._mcp_id, None)
        if "error" in payload:
            raise RuntimeError(payload["error"].get("message", "error"))
        result = payload.get("result", {})
        content = " ".join(c.get("text", "") for c in result.get("content", []))
        if result.get("isError"):
            raise RuntimeError(content or "error")
        return content

    async def cancel_response(self) -> None:
        if self.respond_task and not self.respond_task.done():
            self.respond_task.cancel()
            await asyncio.gather(self.respond_task, return_exceptions=True)
        self.respond_task = None

    # --- server -> device -------------------------------------------------------------------

    async def respond(self, text: str) -> None:
        queue: asyncio.Queue[tuple[str, list[bytes]] | None] = asyncio.Queue(maxsize=4)
        t0 = time.monotonic()

        async def produce() -> None:
            try:
                async for kind, value in self.s.brain.reply(self.device_id, text):
                    if kind == "emotion":
                        await self.send(type="llm", emotion=value, text="")
                    else:
                        await queue.put((value, await self.s.speaker.frames(value)))
            finally:
                await queue.put(None)

        producer = asyncio.create_task(produce())
        loop = asyncio.get_running_loop()
        started = False
        deadline = loop.time()
        try:
            while (item := await queue.get()) is not None:
                sentence, frames = item
                if not started:
                    log.info("[%s] first audio after %.2fs", self.device_id, time.monotonic() - t0)
                    await self.send(type="tts", state="start")
                    started = True
                await self.send(type="tts", state="sentence_start", text=sentence)
                for frame in frames:
                    now = loop.time()
                    if deadline - now > PLAYBACK_LEAD_S:
                        await asyncio.sleep(deadline - now - PLAYBACK_LEAD_S)
                    await self.ws.send_bytes(frame)
                    deadline = max(deadline, loop.time()) + FRAME_S
            await producer  # surfaces errors from the LLM or TTS
            if not started:
                await self.send(type="llm", emotion="confused", text="")
            await asyncio.sleep(max(0.0, deadline - loop.time()))
        except asyncio.CancelledError:
            producer.cancel()
            raise
        except Exception:
            log.exception("[%s] reply failed", self.device_id)
            await self.send(type="llm", emotion="confused", text="")
        finally:
            if not producer.done():
                producer.cancel()
            if started:
                await self.send(type="tts", state="stop")
        if self.mode != "manual":
            self.start_listening()


def allowed(services: Services, device_id: str) -> bool:
    allow = services.config.allowed_devices
    if allow and device_id.lower() not in allow:
        log.warning("rejected unknown device %r", device_id)
        return False
    return True


def device_token(services: Services, device_id: str) -> str:
    """WebSocket token for a device: only someone who knows the OTA secret can obtain it."""
    return hmac.new(services.config.secret.encode(), device_id.lower().encode(), hashlib.sha256).hexdigest()


async def ota(request: web.Request) -> web.Response:
    """First thing the device calls: hand it the clock and where (and how) to open the WebSocket."""
    services: Services = request.app["services"]
    if not hmac.compare_digest(request.match_info["secret"], services.config.secret):
        log.warning("OTA with a wrong secret from %s", request.remote)
        raise web.HTTPNotFound()
    device_id = request.headers.get("Device-Id", "")
    if not device_id or not allowed(services, device_id):
        raise web.HTTPForbidden()

    body = await request.json() if request.can_read_body else {}
    version = body.get("application", {}).get("version", "0.0.0")
    log.info("OTA check from %s (%s, firmware %s)", device_id, request.remote, version)
    tz = ZoneInfo(services.config.timezone)
    offset_min = int(datetime.now(tz).utcoffset().total_seconds() // 60)
    base = services.config.public_url or f"ws://{request.host}"
    return web.json_response({
        "server_time": {"timestamp": int(time.time() * 1000), "timezone_offset": offset_min},
        "firmware": {"version": version, "url": ""},
        "websocket": {"url": f"{base}/xiaozhi/v1/", "token": device_token(services, device_id), "version": 1},
    })


async def voice(request: web.Request) -> web.WebSocketResponse:
    services: Services = request.app["services"]
    device_id = request.headers.get("Device-Id", "")
    expected = f"Bearer {device_token(services, device_id)}"
    if not device_id or not hmac.compare_digest(request.headers.get("Authorization", ""), expected):
        log.warning("WebSocket with a bad token from %s (%r)", request.remote, device_id)
        raise web.HTTPUnauthorized()
    if not allowed(services, device_id):
        raise web.HTTPForbidden()

    ws = web.WebSocketResponse(heartbeat=30)
    await ws.prepare(request)
    base = services.config.public_url.replace("wss://", "https://", 1).replace("ws://", "http://", 1)
    session = Session(ws, device_id, services, f"{base or 'http://' + request.host}/xiaozhi/vision/")
    services.sessions[device_id] = session
    log.info("[%s] connected from %s", device_id, request.remote)

    async def watchdog() -> None:
        while not ws.closed:
            await asyncio.sleep(5)
            idle = time.monotonic() - session.last_voice
            busy = session.respond_task is not None and not session.respond_task.done()
            if session.listening and not busy and idle > services.config.idle_timeout_s:
                log.info("[%s] idle for %ds, closing", device_id, int(idle))
                await ws.close()

    dog = asyncio.create_task(watchdog())
    try:
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                await session.on_json(json.loads(msg.data))
            elif msg.type == WSMsgType.BINARY:
                await session.on_audio(msg.data)
    finally:
        dog.cancel()
        if services.sessions.get(device_id) is session:
            del services.sessions[device_id]
        for waiter in session._mcp_waiters.values():
            waiter.cancel()
        await session.cancel_response()
        log.info("[%s] disconnected", device_id)
    return ws


async def vision(request: web.Request) -> web.Response:
    """The camera's upload (multipart: question, file). Kept for tools.call to hand to Hermes."""
    services: Services = request.app["services"]
    device_id = request.headers.get("Device-Id", "")
    expected = f"Bearer {device_token(services, device_id)}"
    if not device_id or not hmac.compare_digest(request.headers.get("Authorization", ""), expected):
        log.warning("vision upload with a bad token from %s (%r)", request.remote, device_id)
        raise web.HTTPUnauthorized()
    photo = b""
    async for part in await request.multipart():
        if part.name == "file":
            photo = await part.read()
    if not photo:
        raise web.HTTPBadRequest()
    services.photos[device_id] = photo
    log.info("[%s] photo received (%d KB)", device_id, len(photo) // 1024)
    return web.json_response({"success": True, "text": "Foto recibida."})


def build_app(services: Services) -> web.Application:
    app = web.Application()
    app["services"] = services
    app.router.add_route("*", "/xiaozhi/ota/{secret}/", ota)
    app.router.add_get("/xiaozhi/v1/", voice)
    app.router.add_post("/xiaozhi/vision/", vision)
    return app
