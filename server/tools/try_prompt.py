"""Ask the brain some questions without the device, to tune SOUL.md and voice.md (length, tone).

    PET_HERMES_KEY=... uv run python tools/try_prompt.py ["question" ...]
    PET_BRAIN=claude ANTHROPIC_API_KEY=... uv run python tools/try_prompt.py

With Hermes the questions go to a test session of the bixo profile, so whatever Bixo decides
to save in its memory stays there; keep test questions harmless.
"""

import asyncio
import logging
import os
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from pet.__main__ import make_brain  # noqa: E402

QUESTIONS = [
    "Hola Bicho, ¿cómo te llamas y qué eres?",
    "Explícame cómo funciona un agujero negro.",
    "¿Qué tal estás hoy?",
    "Cuéntame un chiste.",
    "¿Qué hora es?",
]


async def main() -> None:
    logging.basicConfig(level=logging.INFO, format="    %(name)s: %(message)s")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    questions = sys.argv[1:] or QUESTIONS
    with tempfile.TemporaryDirectory() as data:
        os.environ.setdefault("PET_DATA", data)  # Claude: a throwaway history
        brain = make_brain("Europe/Madrid")
        for question in questions:
            print(f"\n> {question}")
            t0 = time.monotonic()
            first = None
            text = ""
            async for kind, value in brain.reply("02:00:00:00:00:99", question):
                if kind == "emotion":
                    print(f"  [{value}]")
                    continue
                first = first or time.monotonic() - t0
                text += value + " "
            words = len(text.split())
            sentences = sum(text.count(c) for c in ".!?…")
            print(f"  {text.strip()}")
            print(f"  ({words} words, {sentences} sentences, first chunk {first or 0:.2f}s)")


if __name__ == "__main__":
    asyncio.run(main())
