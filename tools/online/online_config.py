"""Read the explicitly selected online client configuration."""

import ipaddress
import os
import tomllib
from pathlib import Path

CONFIG_PATH = Path(os.environ["BRAWL_ONLINE_CONFIG"]).resolve()
with CONFIG_PATH.open("rb") as stream:
    CONFIG = tomllib.load(stream)


def path(key):
    if key == "account" and os.environ.get("BRAWL_ONLINE_ACCOUNT"):
        return Path(os.environ["BRAWL_ONLINE_ACCOUNT"]).expanduser().resolve()
    if key == "output" and os.environ.get("BRAWL_ONLINE_OUTPUT"):
        return Path(os.environ["BRAWL_ONLINE_OUTPUT"]).expanduser().resolve()
    value = Path(CONFIG["paths"][key]).expanduser()
    return (CONFIG_PATH.parent / value).resolve()


BRAWL_ONLINE_ROOT = path("brawl_online")
MMCLIENT = path("mmclient")
ACCOUNT_FILE = path("account")
LAN_IP = CONFIG["network"].get("lan_ip")
if LAN_IP:
    ipaddress.IPv4Address(LAN_IP)
ALLOWED_MODES = {2}
if CONFIG["network"].get("unranked", False):
    ALLOWED_MODES.add(1)
