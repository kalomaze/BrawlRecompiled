"""Translate the launcher's queue/Direct handoff into PPOnline's LOCAL invite fields."""

import json
import os
import re
import time
from datetime import datetime, timezone
from pathlib import Path


class LaunchHandoff:
    def __init__(self, memory, block, directory):
        self.memory = memory
        self.block = block
        self.directory = Path(directory)
        self.request = self.directory / "join-room.json"
        self.status = self.directory / "game-status.json"
        self.last_request = None
        self.last_status_at = 0
        self.ticks = 0

    def _status(self, state, screen, force=False):
        now = time.monotonic()
        if not force and now - self.last_status_at < 1:
            return
        payload = {
            "version": 1,
            "state": state,
            "screen": screen,
            "updatedAt": datetime.now(timezone.utc).isoformat(),
            "pid": os.getpid(),
            "lastRequest": self.last_request,
        }
        self.directory.mkdir(parents=True, exist_ok=True)
        temporary = self.status.with_name(f".game-status-{os.getpid()}.tmp")
        temporary.write_text(json.dumps(payload))
        os.replace(temporary, self.status)
        self.last_status_at = now

    def tick(self, bridge):
        self.ticks += 1
        scene = self.memory.status()[1]
        screen = "menus" if scene == "muMenuMain" else "online-css" if scene == "scSelctCharacter" else "other"
        busy = screen == "other" or bridge.state in (1, 2, 4) or bridge.wire is not None or bridge.in_match
        self._status("busy" if busy else "idle", screen)
        # The plugin initializes its observed directJoin value on its first menu tick.
        if self.ticks < 20 or not self.request.is_file():
            return
        req = None
        try:
            req = json.loads(self.request.read_text())
            if not isinstance(req, dict):
                return
            rid = req.get("id")
            if not isinstance(rid, str) or not rid or req.get("version") != 1:
                return
            if busy:
                raise ValueError("Finish the current game first.")
            if req.get("kind") == "queue":
                if req.get("queue") != "unranked":
                    raise ValueError("BrawlRecompiled currently supports Unranked 1v1 and Direct.")
                suffix = bytes((0, 0, 0, 0, 0, 1))
            elif req.get("kind") == "direct":
                code = req.get("connectCode", "")
                match = re.fullmatch(r"([A-Z]{2,4})#([1-9][0-9]{0,3})", code)
                if not match or len(code) > 8 or int(match[2]) > 9999:
                    raise ValueError("Invalid Direct connect code.")
                suffix = match[1].encode().ljust(4, b"\0") + int(match[2]).to_bytes(2, "big")
            else:
                raise ValueError("BrawlRecompiled currently supports Unranked 1v1 and Direct.")
            if self.block.version < 5 or self.block.local_size < 0x40:
                raise ValueError("PPOnline does not support launcher handoffs.")
            previous = self.memory.read(self.block.local + 0x39, 1)[0]
            # Put the mode/code first, then bump directJoin. The guest watches that byte.
            self.memory.write(self.block.local + 0x3A, suffix)
            self.memory.write(self.block.local + 0x39, bytes(((previous + 1) & 255,)))
            answer = {"id": rid, "result": "accepted"}
        except (ValueError, OSError, json.JSONDecodeError) as error:
            rid = req.get("id") if isinstance(req, dict) else None
            if not isinstance(rid, str) or not rid:
                return
            answer = {"id": rid, "result": "refused", "message": str(error)}
        try:
            current = json.loads(self.request.read_text())
            if current.get("id") == answer["id"]:
                self.request.unlink(missing_ok=True)
        except (OSError, ValueError):
            pass
        self.last_request = answer
        self._status("busy" if busy else "idle", screen, force=True)

    def close(self):
        try:
            current = json.loads(self.status.read_text())
            if current.get("pid") == os.getpid():
                self.status.unlink(missing_ok=True)
        except (OSError, ValueError):
            pass
