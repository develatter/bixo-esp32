import asyncio
import logging
import os
from pathlib import Path

from aiohttp import web

from .asr import Transcriber
from .brain import Brain, ClaudeBrain, HermesBrain
from .server import Config, Services, build_app
from .tools import build_tools_app
from .tts import Speaker

ROOT = Path(__file__).resolve().parent.parent


def env(name: str, default: str) -> str:
    return os.environ.get(name, default)


def require(name: str) -> str:
    if not os.environ.get(name):
        raise SystemExit(f"{name} is not set")
    return os.environ[name]


def make_brain(timezone: str) -> Brain:
    voice = Path(env("PET_VOICE_RULES", str(ROOT / "voice.md")))
    max_sentences = int(env("PET_MAX_SENTENCES", "4"))
    data_dir = Path(env("PET_DATA", str(ROOT / "data")))
    kind = env("PET_BRAIN", "hermes")
    if kind == "hermes":
        return HermesBrain(url=env("PET_HERMES_URL", "http://127.0.0.1:8642/p/bixo/v1"), key=require("PET_HERMES_KEY"),
                           voice_path=voice, data_dir=data_dir, timezone=timezone, max_sentences=max_sentences,
                           session_gap_s=float(env("PET_SESSION_GAP_MIN", "60")) * 60)
    if kind == "claude":
        require("ANTHROPIC_API_KEY")
        return ClaudeBrain(soul_path=Path(env("PET_SOUL", str(ROOT / "hermes" / "SOUL.md"))), voice_path=voice,
                           data_dir=data_dir, model=env("PET_MODEL", "claude-haiku-5-5"),
                           timezone=timezone, max_tokens=int(env("PET_MAX_TOKENS", "300")), max_sentences=max_sentences)
    raise SystemExit(f"PET_BRAIN must be hermes or claude, not {kind!r}")


def main() -> None:
    logging.basicConfig(level=env("PET_LOG", "INFO"), format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    config = Config(
        secret=require("PET_SECRET"),
        public_url=env("PET_PUBLIC_URL", "").rstrip("/"),
        allowed_devices=frozenset(d.strip().lower() for d in env("PET_ALLOWED_DEVICES", "").split(",") if d.strip()),
        timezone=env("PET_TIMEZONE", "Europe/Madrid"),
        vad_aggressiveness=int(env("PET_VAD_AGGRESSIVENESS", "2")),
        speech_start_ms=int(env("PET_SPEECH_START_MS", "120")),
        speech_end_ms=int(env("PET_SPEECH_END_MS", "900")),
        max_utterance_s=int(env("PET_MAX_UTTERANCE_S", "30")),
        idle_timeout_s=int(env("PET_IDLE_TIMEOUT_S", "60")),
    )
    services = Services(
        config=config,
        asr=Transcriber(),
        brain=make_brain(config.timezone),
        speaker=Speaker(
            voice=env("PET_VOICE", "es-ES-XimenaNeural"),
            rate=env("PET_VOICE_RATE", "+8%"),
            pitch=env("PET_VOICE_PITCH", "+20Hz"),
        ),
    )
    asyncio.run(serve(services))


async def serve(services: Services) -> None:
    """The device server (published with Funnel) and Bixo's tools for Hermes (local only)."""
    runners = [web.AppRunner(build_app(services), access_log=None),
               web.AppRunner(build_tools_app(services), access_log=None)]
    for runner in runners:
        await runner.setup()
    await web.TCPSite(runners[0], env("PET_HOST", "127.0.0.1"), int(env("PET_PORT", "8003"))).start()
    await web.TCPSite(runners[1], "127.0.0.1", int(env("PET_TOOLS_PORT", "8004"))).start()
    logging.getLogger("pet").info("listening on %s:%s, tools for Hermes on 127.0.0.1:%s",
                                  env("PET_HOST", "127.0.0.1"), env("PET_PORT", "8003"), env("PET_TOOLS_PORT", "8004"))
    try:
        await asyncio.Event().wait()
    finally:
        for runner in runners:
            await runner.cleanup()


if __name__ == "__main__":
    main()
