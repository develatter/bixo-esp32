#!/usr/bin/env python3
"""Build Bixo's firmware for the SenseCAP Watcher.

    python3 scripts/build.py [--language es-ES|en-US]

Run it inside an ESP-IDF environment. The server address comes from the environment:
OTA_URL is the device's first contact (it hands over the WebSocket URL and token). It is
written only to build/, never to a tracked file. The image is build/xiaozhi.bin.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

BOARD = "sensecap-watcher"
BOARD_CONFIG = "CONFIG_BOARD_TYPE_SEEED_STUDIO_SENSECAP_WATCHER"
LANGUAGES = {"es-ES": "CONFIG_LANGUAGE_ES_ES", "en-US": "CONFIG_LANGUAGE_EN_US"}
# The AFE front end stays on (voice activity, noise handling); the assets carry no wake
# word model, so nothing listens for one.
AUDIO = [
    "CONFIG_WAKE_WORD_DISABLED=n",
    "CONFIG_USE_ESP_WAKE_WORD=n",
    "CONFIG_USE_AFE_WAKE_WORD=y",
    "CONFIG_USE_CUSTOM_WAKE_WORD=n",
    "CONFIG_SR_WN_WN9_HIESP=y",
]


def idf(*args: str) -> None:
    command = [sys.executable, str(Path(os.environ["IDF_PATH"]) / "tools" / "idf.py"), *args]
    if subprocess.run(command).returncode != 0:
        sys.exit(f"{' '.join(args)} failed")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--language", choices=LANGUAGES, default="es-ES")
    args = parser.parse_args()
    if "IDF_PATH" not in os.environ:
        sys.exit("Run this inside an ESP-IDF environment (. $IDF_PATH/export.sh)")
    ota_url = os.environ.get("OTA_URL")
    if not ota_url:
        sys.exit("Set OTA_URL to the server's OTA address")

    config = json.loads(Path(f"main/boards/{BOARD}/config.json").read_text())
    build = config["builds"][0]
    options = [f"{BOARD_CONFIG}=y", *build["sdkconfig_append"], f'CONFIG_OTA_URL="{ota_url}"',
               f"{LANGUAGES[args.language]}=y", *AUDIO]

    fragment = Path("build/bixo.sdkconfig.defaults")
    fragment.parent.mkdir(exist_ok=True)
    fragment.write_text("\n".join(options) + "\n")
    Path("sdkconfig").unlink(missing_ok=True)  # regenerate it from the defaults every time
    idf(f"-DIDF_TARGET={config['target']}", f"-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;{fragment}",
        f"-DBOARD_NAME={BOARD}", "build")


if __name__ == "__main__":
    main()
