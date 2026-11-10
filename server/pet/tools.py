"""Bixo's body for Hermes: an MCP server (local only) whose tools act on the connected Watcher.

Hermes calls `mirar`; we ask the Watcher to run its own MCP tool `self.camera.take_photo`; the
Watcher uploads the JPEG to /xiaozhi/vision/ (see server.vision) and only then answers the tool
call, so the photo is waiting here when the call returns. Hermes gets it as an image.

Minimal Streamable HTTP transport: JSON responses only, no SSE stream, no sessions.
"""

import base64
import json
import logging

from aiohttp import web

log = logging.getLogger(__name__)

PROTOCOL_VERSION = "2025-06-18"

# The camera faces the speaker, so the photo's left is their right. Saying it in every call
# keeps left and right straight; the pixels stay as they are so text in the photo reads right.
CAMERA_SIDES = ("lo que sale a la izquierda de la foto está a su derecha, y al revés. "
                "Di izquierda y derecha desde su punto de vista (su mano derecha, a su izquierda…).")

# The names in sprites/backgrounds.py, which the assets partition carries.
BACKGROUNDS = {
    "noche": "azul oscuro, el de siempre", "blanco": "blanco liso", "prado": "un prado con mariposa",
    "mazmorra": "una mazmorra con antorchas", "synthwave": "atardecer synthwave con neones",
    "navidad": "nieve, árbol y casita con luces", "verano": "playa con palmera", "otono": "árboles y hojas cayendo",
}

TOOLS = [
    {
        "name": "mirar",
        "description": "Haz una foto con tu cámara (la del Watcher) y mírala. Úsala siempre que te "
                       "pidan que mires algo, qué ves, quién hay, qué tienen en la mano, etc. "
                       "Describe lo que ves con tus palabras; no hables de la foto como un archivo. "
                       + "Tu cámara está en tu cara y mira de frente a quien te habla: "
                       + CAMERA_SIDES,
        "inputSchema": {
            "type": "object",
            "properties": {"pregunta": {"type": "string", "description": "Qué quieres averiguar en la foto"}},
            "required": ["pregunta"],
        },
    },
    {
        "name": "volumen",
        "description": "Cambia el volumen de tu altavoz (0 a 100).",
        "inputSchema": {"type": "object", "properties": {"nivel": {"type": "integer", "minimum": 0, "maximum": 100}},
                        "required": ["nivel"]},
    },
    {
        "name": "brillo",
        "description": "Cambia el brillo de tu pantalla (10 a 100).",
        "inputSchema": {"type": "object", "properties": {"nivel": {"type": "integer", "minimum": 10, "maximum": 100}},
                        "required": ["nivel"]},
    },
    {
        "name": "fondo",
        "description": "Cambia el fondo de tu pantalla, lo que se ve detrás de ti. Se queda puesto. Opciones: "
                       + "; ".join(f"{name} ({what})" for name, what in BACKGROUNDS.items()) + ".",
        "inputSchema": {"type": "object", "properties": {"nombre": {"type": "string", "enum": list(BACKGROUNDS)}},
                        "required": ["nombre"]},
    },
    {
        "name": "nueva_charla",
        "description": "Empieza una charla nueva: a partir de tu próxima respuesta no recordarás esta "
                       "conversación (lo que guardaste en tu memoria se queda). Úsala solo si te piden "
                       "olvidar la conversación o empezar de cero.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "estado",
        "description": "Cómo está tu cuerpo: batería, volumen, brillo y red.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


class ToolError(Exception):
    """Reported to the model as a tool error (isError), with this text."""


def text(value: str) -> dict:
    return {"type": "text", "text": value}


async def call(services, name: str, args: dict) -> list[dict]:
    session = services.current_session()
    if session is None:
        raise ToolError("El Watcher no está conectado ahora mismo.")
    if name == "mirar":
        services.photos.pop(session.device_id, None)
        await session.call_tool("self.camera.take_photo", {"question": args.get("pregunta", "")}, timeout=45)
        photo = services.photos.pop(session.device_id, None)
        if photo is None:
            raise ToolError("La cámara no ha mandado ninguna foto.")
        log.info("[%s] photo for Hermes: %d KB", session.device_id, len(photo) // 1024)
        return [{"type": "image", "data": base64.b64encode(photo).decode(), "mimeType": "image/jpeg"},
                text("Esto es lo que ve tu cámara ahora mismo, de frente a quien te habla: " + CAMERA_SIDES)]
    if name == "volumen":
        level = max(0, min(100, int(args["nivel"])))
        await session.call_tool("self.audio_speaker.set_volume", {"volume": level})
        return [text(f"Volumen al {level} %.")]
    if name == "brillo":
        level = max(10, min(100, int(args["nivel"])))
        await session.call_tool("self.screen.set_brightness", {"brightness": level})
        return [text(f"Brillo al {level} %.")]
    if name == "fondo":
        background = args["nombre"]
        if background not in BACKGROUNDS:
            raise ToolError(f"No tengo el fondo {background}.")
        await session.call_tool("self.screen.set_background", {"name": background})
        return [text(f"Fondo cambiado a {background}.")]
    if name == "nueva_charla":
        services.brain.forget(session.device_id)
        return [text("Hecho: tu próxima respuesta ya empieza una charla nueva.")]
    if name == "estado":
        return [text(await session.call_tool("self.get_device_status", {}))]
    raise ToolError(f"No conozco la herramienta {name}.")


async def mcp(request: web.Request) -> web.Response:
    services = request.app["services"]
    msg = await request.json()
    method, msg_id = msg.get("method"), msg.get("id")
    if msg_id is None:  # notifications need no answer
        return web.Response(status=202)

    def reply(result: dict) -> web.Response:
        return web.json_response({"jsonrpc": "2.0", "id": msg_id, "result": result})

    if method == "initialize":
        version = msg.get("params", {}).get("protocolVersion", PROTOCOL_VERSION)
        return reply({"protocolVersion": version, "capabilities": {"tools": {}},
                      "serverInfo": {"name": "bixo-watcher", "version": "1.0"}})
    if method == "ping":
        return reply({})
    if method == "tools/list":
        return reply({"tools": TOOLS})
    if method == "tools/call":
        params = msg.get("params", {})
        name, args = params.get("name", ""), params.get("arguments") or {}
        log.info("Hermes calls %s %s", name, json.dumps(args, ensure_ascii=False))
        try:
            return reply({"content": await call(services, name, args), "isError": False})
        except (ToolError, TimeoutError, RuntimeError) as e:
            log.warning("tool %s failed: %s", name, e)
            return reply({"content": [text(str(e) or "La herramienta ha tardado demasiado.")], "isError": True})
    return web.json_response({"jsonrpc": "2.0", "id": msg_id,
                              "error": {"code": -32601, "message": f"Method not found: {method}"}})


async def no_stream(request: web.Request) -> web.Response:
    return web.Response(status=405)


def build_tools_app(services) -> web.Application:
    app = web.Application()
    app["services"] = services
    app.router.add_post("/mcp", mcp)
    app.router.add_get("/mcp", no_stream)
    app.router.add_delete("/mcp", no_stream)
    return app
