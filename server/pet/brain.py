"""The pet's mind: an emotion per reply and speakable chunks, from Hermes (the bixo profile) or Claude."""

import json
import logging
import re
import time
from collections.abc import AsyncGenerator, AsyncIterator
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

import aiohttp
from anthropic import AsyncAnthropic

log = logging.getLogger(__name__)

# Expressions the firmware knows how to draw (see the emoji collection in xiaozhi-esp32).
EMOTIONS = (
    "neutral", "happy", "laughing", "funny", "sad", "angry", "crying", "loving", "embarrassed",
    "surprised", "shocked", "thinking", "winking", "cool", "relaxed", "delicious", "kissy",
    "confident", "sleepy", "silly", "confused",
)

# The only instruction that lives in code: the server parses this tag to drive the face.
_EMOTION_RULE = f"""
# Formato técnico (obligatorio)

Empieza SIEMPRE la respuesta con tu emoción entre corchetes, por ejemplo [happy], y después
el texto. Solo una etiqueta, al principio. Elige una de: {", ".join(EMOTIONS)}.
""".strip()

_DAYS = ("lunes", "martes", "miércoles", "jueves", "viernes", "sábado", "domingo")
_MONTHS = ("enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto",
           "septiembre", "octubre", "noviembre", "diciembre")

_TAG = re.compile(r"^\s*\[([a-z_]+)\]\s*")
# Emotion tags out of place, and Hermes' MEDIA:<path> file references (after looking at a photo).
_STRAY_TAG = re.compile(r"\[[a-z_]+\]|MEDIA:\S+")
_SENTENCE_END = re.compile(r"(?<=[.!?…])\s+")
_CLAUSE_END = re.compile(r"(?<=[,;:])\s+")
_FIRST_CHUNK_MIN = 12
_TERMINAL = ".!?…"

# What a text source yields: ("text", delta) or ("tool", name) when it starts using a tool.
Event = tuple[str, str]


class Brain:
    """Turns a source's text stream into ("emotion", name) and ("sentence", text) events."""

    def __init__(self, voice_path: Path, timezone: str, max_sentences: int) -> None:
        self._voice_path = voice_path
        self._tz = ZoneInfo(timezone)
        self._max_sentences = max_sentences

    def _voice_rules(self) -> str:
        t = datetime.now(self._tz)
        now = f"{_DAYS[t.weekday()]} {t.day} de {_MONTHS[t.month - 1]} de {t.year}, {t:%H:%M}"
        rules = self._voice_path.read_text().strip()  # read on every turn: edits apply live
        return f"{rules}\n\n{_EMOTION_RULE}\n\nAhora mismo es {now} (hora local)."

    def _events(self, device_id: str, user_text: str) -> AsyncGenerator[Event, None]:
        raise NotImplementedError

    async def _remember(self, device_id: str, user_text: str, reply: str) -> None:
        """Called with the complete reply; sources that keep their own history do nothing."""

    def forget(self, device_id: str) -> None:
        """Ends the conversation: the next reply starts a new one. Long-term memory stays."""
        raise NotImplementedError

    async def reply(self, device_id: str, user_text: str) -> AsyncIterator[tuple[str, str]]:
        """Stream ("emotion", name) once, then ("sentence", text) chunks ready to be spoken."""
        buffer = ""
        full = ""
        emotion_sent = False
        sentences = 0

        events = self._events(device_id, user_text)
        try:
            async for kind, value in events:
                if kind == "tool":
                    log.info("[%s] using %s", device_id, value)
                    yield "emotion", "thinking"
                    continue
                buffer += value
                if not emotion_sent:
                    match = _TAG.match(buffer)
                    if match:
                        emotion = match.group(1) if match.group(1) in EMOTIONS else "neutral"
                        buffer = buffer[match.end():]
                        emotion_sent = True
                        yield "emotion", emotion
                    elif len(buffer) > 24 or not buffer.lstrip().startswith("["):
                        emotion_sent = True
                        yield "emotion", "neutral"
                    else:
                        continue
                *done, buffer = _SENTENCE_END.split(buffer)
                if not full and not done and len(buffer) >= _FIRST_CHUNK_MIN:
                    # Speak the first clause as soon as possible: it hides the TTS latency.
                    *done, buffer = _CLAUSE_END.split(buffer, maxsplit=1)
                for sentence in done:
                    sentence = _STRAY_TAG.sub("", sentence).strip()
                    if sentence:
                        full += sentence + " "
                        yield "sentence", sentence
                        sentences += sentence[-1] in _TERMINAL
                if sentences >= self._max_sentences:
                    # Hard cap in case the model rambles: closing the stream stops generation.
                    log.info("reply cut at %d sentences", sentences)
                    buffer = ""
                    break
        finally:
            await events.aclose()

        tail = _STRAY_TAG.sub("", buffer).strip()
        if tail:
            full += tail
            yield "sentence", tail
        if not full.strip():
            log.warning("empty reply")
            return
        await self._remember(device_id, user_text, full.strip())


class HermesBrain(Brain):
    """The bixo profile of Hermes Agent: its SOUL, memory and tools; one session per device.

    The session id lives in data/hermes-session-<mac>.txt. After `session_gap_s` without a turn
    the next one starts a fresh session, so an old topic doesn't follow him around for days; what
    matters is in Hermes' long-term memory. Hermes never reuses the id of a retired session.
    """

    def __init__(self, url: str, key: str, voice_path: Path, data_dir: Path, timezone: str,
                 max_sentences: int, session_gap_s: float) -> None:
        super().__init__(voice_path, timezone, max_sentences)
        self._url = url.rstrip("/") + "/chat/completions"
        self._key = key
        self._data_dir = data_dir
        self._session_gap_s = session_gap_s
        self._http: aiohttp.ClientSession | None = None
        data_dir.mkdir(parents=True, exist_ok=True)

    def _session_path(self, device_id: str) -> Path:
        return self._data_dir / f"hermes-session-{device_id.replace(':', '').lower()}.txt"

    def forget(self, device_id: str) -> None:
        self._session_path(device_id).unlink(missing_ok=True)
        log.info("[%s] Hermes session retired", device_id)

    def _session_id(self, device_id: str) -> str:
        path = self._session_path(device_id)
        if not path.exists() or time.time() - path.stat().st_mtime > self._session_gap_s:
            path.write_text(f"bixo-{device_id.replace(':', '').lower()}-{int(time.time())}")
            log.info("[%s] new Hermes session %s", device_id, path.read_text())
        else:
            path.touch()
        return path.read_text().strip()

    async def _events(self, device_id: str, user_text: str) -> AsyncGenerator[Event, None]:
        if self._http is None:
            self._http = aiohttp.ClientSession(timeout=aiohttp.ClientTimeout(total=120, sock_read=60))
        mac = device_id.replace(":", "").lower()
        headers = {
            "Authorization": f"Bearer {self._key}",
            "X-Hermes-Session-Id": self._session_id(device_id),
            "X-Hermes-Session-Key": f"bixo:watcher:{mac}",
        }
        body = {
            "model": "bixo",
            "stream": True,
            "model_options": {"reasoning": {"enabled": False}},
            "messages": [{"role": "system", "content": self._voice_rules()},
                         {"role": "user", "content": user_text}],
        }
        async with self._http.post(self._url, json=body, headers=headers) as resp:
            resp.raise_for_status()
            event = None
            async for raw in resp.content:
                line = raw.decode().strip()
                if line.startswith("event:"):
                    event = line[6:].strip()
                    continue
                if not line.startswith("data:") or line == "data: [DONE]":
                    continue
                data = json.loads(line[5:])
                if event:  # named Hermes events: only tool starts matter here
                    if event == "hermes.tool.progress" and data.get("status") == "running":
                        yield "tool", data.get("tool", "?")
                    event = None
                    continue
                for choice in data.get("choices", []):
                    if text := choice.get("delta", {}).get("content"):
                        yield "text", text
                    if reason := choice.get("finish_reason"):
                        log.info("reply done: %s", reason)
                        if reason != "stop":
                            raise RuntimeError(f"Hermes ended with {reason}: {data.get('error')}")


class ClaudeBrain(Brain):
    """Claude called directly, with the SOUL and a local history per device (fallback without Hermes)."""

    def __init__(self, soul_path: Path, voice_path: Path, data_dir: Path, model: str, timezone: str,
                 max_tokens: int, max_sentences: int, max_turns: int = 20) -> None:
        super().__init__(voice_path, timezone, max_sentences)
        self._client = AsyncAnthropic()
        self._soul_path = soul_path
        self._data_dir = data_dir
        self._model = model
        self._max_tokens = max_tokens
        self._max_turns = max_turns
        data_dir.mkdir(parents=True, exist_ok=True)

    def _history_path(self, device_id: str) -> Path:
        return self._data_dir / f"history-{device_id.replace(':', '')}.json"

    def forget(self, device_id: str) -> None:
        self._history_path(device_id).unlink(missing_ok=True)

    def _load_history(self, device_id: str) -> list[dict]:
        path = self._history_path(device_id)
        return json.loads(path.read_text()) if path.exists() else []

    async def _events(self, device_id: str, user_text: str) -> AsyncGenerator[Event, None]:
        history = self._load_history(device_id) + [{"role": "user", "content": user_text}]
        system = f"{self._soul_path.read_text().strip()}\n\n{self._voice_rules()}"
        # No thinking: it adds latency and spends the small max_tokens budget before any speech.
        async with self._client.messages.stream(
            model=self._model, max_tokens=self._max_tokens, system=system, messages=history,
            thinking={"type": "disabled"},
        ) as stream:
            async for delta in stream.text_stream:
                yield "text", delta
            final = await stream.get_final_message()
            log.info("reply done: %s, %d tokens", final.stop_reason, final.usage.output_tokens)

    async def _remember(self, device_id: str, user_text: str, reply: str) -> None:
        history = self._load_history(device_id)
        history += [{"role": "user", "content": user_text}, {"role": "assistant", "content": reply}]
        self._history_path(device_id).write_text(
            json.dumps(history[-2 * self._max_turns:], ensure_ascii=False, indent=1))
