# AGENTS.md

Bixo, an AI virtual pet on a SenseCAP Watcher. Read `README.md` (the device) and
`server/README.md` (the brain) first.

- Repository root: the firmware, a fork of xiaozhi-esp32 trimmed to this board (ESP-IDF 6.1,
  LVGL 9). Our code is in `main/boards/sensecap-watcher/`; outside it only
  `Application::ToggleConversation()` changes. Keep it that way so upstream merges stay easy.
- `server/`: the voice server (Python, uv), the Hermes Agent profile, and the sprites and
  backgrounds packed into the `assets` partition.

## Conventions

- Code, comments, docs for developers and commit messages in English; everything the user reads
  or hears (screen, menu, voice, tool descriptions for Hermes, READMEs) in Spanish (Spain).
- Comments say what the code does or why, never how it came to be.
- Python: type hints, one module per concern under `server/pet/`. C++: the board's style
  (4 spaces, `snake_case_` members, `kConstants`).
- Faces are sprites, never status text: a new device state gets an animation in
  `server/sprites/bixo.py` (`ANIMATIONS`) and a mapping in `CustomLcdDisplay::ShowEmotion()`.
  Every animation also comes in grey battery tiers (`_g1`..`_g3`), generated automatically.
- Sprites are a 22x20 logical canvas; glyphs off the body get a dark outline so they read on
  every background. Backgrounds share a 13 px cell grid with the sprite (`backgrounds.py`);
  the firmware paints them by cells (`pet_background.cc`). Bixo's shadow is a separate layer,
  only drawn under his sprite.
- The menu (`pet_ui.cc`) is driven from the main task (`Application::Schedule`), by the wheel
  and the touch screen.
- Hermes tools live in `server/pet/tools.py`; device actions they need are MCP tools on the
  Watcher (`self.*`), registered in `SensecapWatcher::InitializeTools()`.

## Secrets

- Never write a secret to a tracked file, a log you keep, or a commit. The OTA URL carries the
  server secret: it only exists in the environment and in `build/` (never print `build/sdkconfig`
  or the build log unredacted).
- Settings go in `server/.env` (ignored; see `server/.env.example`) or the environment. Don't
  create or read `server/.env` yourself: ask the user to run a real build or start the server.
- Serial logs include tokens and URLs: filter and redact them in memory, don't save them raw.
- No real hostnames, IPs or MACs in the repository.

## Commands

From `server/`:

```bash
uv run python sprites/bixo.py                              # sprites -> sprites/out
uv run python sprites/backgrounds.py [--mockups]           # background previews
uv run python sprites/pack_assets.py ../build/generated_assets.bin   # -> sprites/assets.bin
tools/build_firmware.sh --check                            # compile check, no secrets needed
tools/build_firmware.sh                                    # real build (needs the secret)
tools/run_server.sh                                        # the server (needs the secret)
```

Run `tools/build_firmware.sh --check` after every firmware change, and regenerate the assets
after touching sprites or backgrounds.

Flash only the app and the assets, never a merged image (it would wipe `nvsfactory` at
`0x9000`, the Watcher's factory data):

```bash
esptool --chip esp32s3 -p PORT -b 230400 write-flash --flash-mode dio --flash-freq 80m \
  --flash-size 32MB 0x200000 build/xiaozhi.bin 0xa00000 server/sprites/assets.bin
```
