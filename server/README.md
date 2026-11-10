# El servidor de Bixo

El cerebro de Bixo. Habla el protocolo de xiaozhi (OTA por HTTP y voz por WebSocket con
audio Opus) y genera los sprites y fondos de la partición `assets`. Los comandos de este
README se lanzan desde `server/`.

```
Watcher ──Opus 16 kHz──▶ VAD (webrtcvad) ──▶ Parakeet TDT v3 (MLX) ──▶ Hermes, perfil bixo (streaming)
   ▲                                                                       │
   └──── Opus 24 kHz ◀── edge-tts, frase a frase ◀── [emoción] + frases ◀──┘
```

El cerebro es un perfil propio de [Hermes Agent](https://hermes-agent.nousresearch.com),
`bixo`: su personalidad, su memoria, búsqueda web y su sprite como pet de Hermes. El
servidor solo pone la voz.

| Fichero | Qué hace |
|---|---|
| `hermes/SOUL.md` | Quién es Bixo, su tono y sus límites (en cualquier canal). El perfil lo enlaza con un symlink. |
| `voice.md` | Reglas para cuando habla por el altavoz (longitud, formato para voz). Se lee en cada respuesta, así que se puede editar con el servidor en marcha. |
| `pet/server.py` | Endpoint OTA (`/xiaozhi/ota/<secreto>/`) y sesión de voz (`/xiaozhi/v1/`). |
| `pet/tools.py` | El cuerpo de Bixo para Hermes: un servidor MCP local (`127.0.0.1:8004`) con `mirar`, `volumen`, `brillo`, `fondo`, `nueva_charla` y `estado`, que llama a las herramientas MCP del Watcher. |
| `pet/brain.py` | Pide la respuesta a Hermes (una sesión por Watcher) o, con `PET_BRAIN=claude`, a Claude directamente. Cada respuesta empieza con una emoción (`[happy]`) que cambia la cara. |
| `pet/asr.py` | Parakeet TDT 0.6b v3 (multilingüe) en la GPU con MLX. |
| `pet/tts.py` | edge-tts (voces neuronales es-ES), recodificado a Opus de 24 kHz. |
| `pet/opus.py` | Binding mínimo de libopus con ctypes. |
| `sprites/bixo.py` | Las animaciones de Bixo: un GIF transparente por emoción y nivel de batería. |
| `sprites/backgrounds.py` | Los fondos, en la misma rejilla que los sprites (`--mockups` los enseña con Bixo encima). |
| `sprites/pack_assets.py` | Mete sprites y fondos en la partición `assets` del firmware. |
| `sprites/hermes_pet.py` | Hace con las animaciones el pet de Hermes. |
| `tools/pixel_font.py` | Convierte Press Start 2P en `font_pixel_16.c`. |

## Arrancar el servidor

Necesita `ffmpeg` y `opus` (`brew install ffmpeg opus`), [uv](https://docs.astral.sh/uv/),
Tailscale y el gateway de Hermes en marcha. La configuración va en `server/.env` (copia
`.env.example`; no se sube) o en el entorno. Solo es obligatorio `PET_SECRET`, el secreto del
servidor (`openssl rand -hex 24`); si no pones la clave de la API local de Hermes, se lee del
`.env` del perfil `bixo`, y si no pones el nombre público, se usa el de Tailscale:

```bash
tools/run_server.sh
```

Escucha solo en `127.0.0.1:8003`. Para que el Watcher llegue desde cualquier red se publica
con Tailscale Funnel, en el nombre de la máquina en la tailnet (`<máquina>.<tailnet>.ts.net`,
que el script lee de `tailscale status`):

```bash
tailscale funnel --bg 8003
```

La primera vez descarga el modelo de Parakeet (~1,2 GB) de Hugging Face.

Para afinar el tono sin el Watcher, `tools/try_prompt.py` hace preguntas al cerebro y
cuenta palabras, frases y latencia. El cliente simulado envía un audio como si fuera el
micro y guarda la respuesta (necesita el secreto en `PET_SECRET`):

```bash
uv run python tools/fake_device.py pregunta.wav --out respuesta.wav
```

## Hermes

El perfil se creó con `hermes profile create bixo --clone` y luego:

- `SOUL.md` → symlink a `hermes/SOUL.md`.
- Modelo `claude-haiku-5-5`, con la clave de Anthropic en el `.env` del perfil (el
  gateway de launchd no puede usar el login de Claude Code).
- Herramientas: solo `web`, `memory` y `session_search`
  (`agent.disabled_toolsets`). Al Watcher se llega desde internet, así que nada de
  terminal, ficheros ni ejecución de código.
- Backend de terminal `docker` con `terminal.docker_shared_container_key: bixo`: todas las
  sesiones (cada Watcher, la CLI) comparten un único contenedor, que sobrevive a los
  reinicios del gateway. Sin la clave sería uno por sesión. Aparte, al arrancar el gateway
  Hermes crea un contenedor de usar y tirar para sondear el sistema (`uname`) y lo borra en
  el acto; eso no se puede desactivar.
- Conversación: cada Watcher tiene su sesión de Hermes, con el id en
  `data/hermes-session-<mac>.txt`. Tras una hora sin hablar (`PET_SESSION_GAP_MIN`) empieza
  una nueva, para que un tema no se le quede pegado durante días; la memoria a largo plazo se
  queda. Para empezar de cero antes: «Nueva charla» en el menú del Watcher, pedírselo a Bixo
  (herramienta `nueva_charla`) o borrar ese fichero. No borres la sesión en Hermes: deja ese
  id retirado para siempre.
- MCP `watcher` (`mcp_servers` en el `config.yaml` del perfil) → `http://127.0.0.1:8004/mcp`,
  y `tools.tool_search.enabled: "off"`: con tan pocas herramientas no compensa que Hermes las
  busque antes de usarlas (ahorra unos 4 s al mirar).
- API local activada (`API_SERVER_ENABLED` en `~/.hermes/.env`); el perfil se sirve en
  `http://127.0.0.1:8642/p/bixo/v1` con su propia `API_SERVER_KEY`.
- Pet: `uv run python sprites/hermes_pet.py --install ~/.hermes/profiles/bixo` y
  `hermes -p bixo pets select bixo`. Se ve en `hermes -p bixo` (terminal con gráficos) y
  en la app de escritorio.

## La cámara

Bixo mira con la cámara del Watcher cuando se lo pides por voz («¿qué ves?», «mira esto»).
El servidor le da al Watcher la URL `/xiaozhi/vision/` (protegida con su token); Hermes
llama a `mirar`, el Watcher hace la foto, la sube ahí y Hermes la recibe como imagen, así que
la ve el propio modelo de Bixo. No hace falta ninguna frase concreta: el modelo decide cuándo
usar la cámara.

El Watcher también puede abrir la conversación por su cuenta, enviando un texto como si fuera
la palabra de activación (la detección de personas de la cámara manda
«<detect>1 person detected </detect>»). El servidor responde a cualquier texto de activación
que no sea una palabra de activación conocida como si lo hubieras dicho tú.

## Sin palabra de activación

No hay palabra de activación: la conversación se abre con la rueda. `sprites/pack_assets.py`
quita los modelos de voz (WakeNet/MultiNet) del paquete de assets, así que el Watcher no
escucha en reposo ni muestra el aviso de «di Hi ESP».

## Seguridad

- La URL OTA lleva un secreto aleatorio en la ruta; sin él, 404.
- La respuesta OTA entrega un token por dispositivo (HMAC de su MAC con el secreto) que el
  WebSocket exige; sin él, 401.
- El servidor no escribe access log (la ruta lleva el secreto).
- El secreto va compilado en el firmware: quien tenga el Watcher en la mano puede sacarlo.
  Pon un límite de gasto a la API key y, si pierdes el dispositivo, cambia `PET_SECRET` y
  recompila.

## Configuración (variables de entorno)

| Variable | Por defecto | Qué hace |
|---|---|---|
| `PET_PUBLIC_URL` | (la pone `run_server.sh`) | Base `wss://` que se entrega al dispositivo |
| `PET_HOST` / `PET_PORT` | `127.0.0.1` / `8003` | Dónde escucha |
| `PET_TOOLS_PORT` | `8004` | MCP para Hermes (siempre en `127.0.0.1`, no se publica) |
| `PET_BRAIN` | `hermes` | `hermes` o `claude` (Claude directo, sin Hermes) |
| `PET_HERMES_URL` | `http://127.0.0.1:8642/p/bixo/v1` | API del perfil de Hermes |
| `PET_HERMES_KEY` | (la pone `run_server.sh`) | `API_SERVER_KEY` del perfil |
| `PET_VOICE_RULES` | `voice.md` | Reglas para hablar en voz alta |
| `PET_MAX_SENTENCES` | `4` | Frases como máximo; si se pasa, se corta |
| `PET_SOUL` / `PET_MODEL` / `PET_MAX_TOKENS` | `hermes/SOUL.md` / `claude-haiku-5-5` / `300` | Solo con `PET_BRAIN=claude` |
| `PET_VOICE` | `es-ES-XimenaNeural` | Voz de edge-tts (`uv run edge-tts --list-voices`) |
| `PET_VOICE_RATE` / `PET_VOICE_PITCH` | `+8%` / `+20Hz` | Velocidad y tono de la voz |
| `PET_SPEECH_END_MS` | `900` | Silencio que cierra una frase del usuario |
| `PET_IDLE_TIMEOUT_S` | `60` | Segundos sin voz antes de cerrar la sesión |
| `PET_ALLOWED_DEVICES` | (todos) | MACs permitidas, separadas por comas |
| `PET_TIMEZONE` | `Europe/Madrid` | Zona horaria (reloj del dispositivo y del prompt) |
| `PET_SESSION_GAP_MIN` | `60` | Minutos sin hablar tras los que empieza una sesión de Hermes nueva |
| `PET_DATA` | `data` | Sesiones de Hermes e historial, por Watcher |

## Firmware, sprites y fondos

```bash
tools/build_firmware.sh                 # compila el firmware (la raíz del repo) en Docker, ESP-IDF 6.1
tools/build_firmware.sh --check         # solo comprueba que compila (sin el secreto; no se graba)
uv run python sprites/bixo.py           # genera sprites/out/*.gif y sheet.png
uv run python sprites/backgrounds.py    # vista previa de los fondos (sprites/out/backgrounds)
uv run python sprites/pack_assets.py ../build/generated_assets.bin
uv run python tools/pixel_font.py PressStart2P-Regular.ttf \
  --out ../main/boards/sensecap-watcher/font_pixel_16.c
```

El firmware se compila en español con la URL OTA de Funnel, que entra al contenedor por el
entorno. Se graban dos piezas: `build/xiaozhi.bin` en `0x200000` y `server/sprites/assets.bin`
en `0xa00000` (el comando está en el README principal).

## Pendiente

- Acciones de mascota en el menú (darle de comer, jugar, dormir).
- Iconos para los avisos del sistema (`cloud_off`, `robot_2`).
- Bajar la latencia de la voz: edge-tts en streaming o una voz local.
- Desplegar en una VPS: Parakeet necesita Apple Silicon, así que allí habría que cambiar el ASR.
