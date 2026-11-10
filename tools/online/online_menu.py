"""Connect the PPOnline menu mailbox to Brawl Online matchmaking.

The guest PPOnline plugin owns the UI, character lock-in, and training room.
This module answers its requests with the matchmaking client's state.
"""

import collections
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

from online_config import (
    ACCOUNT_FILE,
    ALLOWED_MODES,
    BRAWL_ONLINE_ROOT,
    CONFIG,
    LAN_IP,
    MMCLIENT,
)

sys.path.insert(0, str(BRAWL_ONLINE_ROOT / "tools/gamecode"))
import ppom


class MenuBridge:
    def __init__(self, memory, block, user, server, out, port=29800):
        self.m, self.block, self.user = memory, block, user
        self.server, self.out, self.port = server, Path(out), port
        self.process = None
        self.result = None
        self.state = 0
        self.error = ""
        self.mode = 1
        self.commands = collections.Counter()
        self.events = []
        self.searches = 0
        self.published = {}
        self.local_slot = 1

    def cancel(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.cancel_file.touch()
                try:
                    self.process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait()
            self.process = None
        self.state, self.result, self.error = 0, None, ""

    def search(self, payload):
        self.cancel()
        self.mode = payload[0]
        if self.mode not in ALLOWED_MODES:
            self.state, self.error = (
                5,
                "This configuration does not enable that matchmaking mode",
            )
            return
        self.searches += 1
        code = ppom.from_u16s(payload[4:22])
        args = [
            str(MMCLIENT),
            "search",
            "--app-version",
            CONFIG["network"]["app_version"],
            "--server",
            self.server,
            "--user-json",
            str(ACCOUNT_FILE),
            "--mode",
            "unranked" if self.mode == 1 else "direct",
            "--code",
            code,
            "--port",
            str(self.port),
            "--punch",
        ]
        if LAN_IP:
            args.extend(["--lan-ip", LAN_IP])
        self.output = self.out / f"matchmaking-{self.searches}.json"
        self.cancel_file = self.out / f"cancel-{self.searches}"
        with (
            self.output.open("w") as stdout,
            (self.out / f"matchmaking-{self.searches}.log").open("w") as stderr,
        ):
            self.process = subprocess.Popen(
                args,
                stdout=stdout,
                stderr=stderr,
                env=dict(
                    os.environ,
                    BRAWL_PRIVATE_GAMEPLAY_PUNCH="1",
                    BRAWL_PRIVATE_CANCEL_FILE=str(self.cancel_file),
                ),
            )
        self.state = 1
        self.events.append(
            {
                "event": "search",
                "mode": self.mode,
                "css": payload[1],
                "costume": payload[2],
            }
        )

    def poll_matchmaking(self):
        if self.process is None or self.result is not None:
            return
        if self.process.poll() is None:
            # Wait for the authenticated ticket acknowledgement.
            # mmclient logs that step before waiting for an opponent.
            if (
                "private-gameplay: ticket accepted"
                in (self.out / f"matchmaking-{self.searches}.log").read_text()
            ):
                self.state = 2
            return
        self.result = json.loads(self.output.read_text())
        self.state = 4 if self.result.get("status") == "matched" else 5
        self.error = self.result.get("error", "")
        self.events.append(
            {"event": "matchmaking_result", "status": self.result.get("status")}
        )

    def match_response(self):
        peer = {}
        ticket = (self.result or {}).get("ticketResponse", {})
        for p in ticket.get("players", []):
            if not p.get("isLocalPlayer"):
                peer = p
                break
        return ppom.match_state_payload(
            self.state,
            peer.get("displayName", ""),
            peer.get("connectCode", ""),
            self.error,
            role=(1 if ticket.get("isHost") else 2) if self.state == 4 else 0,
        )

    def tick(self):
        self.poll_matchmaking()
        m, b = self.m, self.block
        (wr, rd, rc, rs), slots = ppom.read_requests(m, b)
        if wr - rd > ppom.REQ_SLOTS:
            raise RuntimeError("PPOM request ring overflow")
        while rd != wr:
            seq = rd + 1
            actual, cmd, payload = slots[(seq - 1) % ppom.REQ_SLOTS]
            if actual != seq:
                break
            if cmd != 0xBA and rc != rs:
                break
            response_cmd, response = cmd, None
            self.commands[ppom.CMD.get(cmd, hex(cmd))] += 1
            if cmd == 0xB9:
                response = (
                    b"\x01\0"
                    + ppom.u16s(self.user.display_name, 16)
                    + ppom.u16s(self.user.connect_code, 9)
                )
            elif cmd == 0xB4:
                self.search(payload)
                response_cmd, response = 0xB3, self.match_response()
            elif cmd == 0xB3:
                response = self.match_response()
            elif cmd == 0xBA:
                self.cancel()
                self.events.append({"event": "cancel"})
            elif cmd == 0xBE:
                q = ppom.code_suggestion_request(payload)
                response = ppom.code_suggestion_payload(False, q["input"], q["index"])
            else:
                raise RuntimeError(f"Unsupported PPOnline command: {cmd:#x}")
            if response is not None:
                ppom.write_response(m, b, seq, response_cmd, response)
                rc += 1
            rd = seq
            ppom.consume(m, b, rd)

    def publish(self, address, data):
        """Publish host-owned PPOM bytes, with the sequence word written last."""
        data = bytes(data)
        if self.published.get(address) == data[4:]:
            return
        seq = self.m.u32(address) + 1
        self.m.write(address + 4, data[4:])
        self.m.w32(address, seq)
        self.published[address] = data[4:]

    def local_lock(self):
        raw = self.m.read(self.block.local, self.block.local_size)
        lock = raw[0x28:0x34]
        return dict(
            ready=bool(lock[4]),
            css=lock[5],
            kind=lock[6],
            costume=lock[7],
            stage=int.from_bytes(lock[8:10], "big"),
            asl=lock[10],
            game=lock[11],
            team=raw[0x34],
            pv=raw[0x40:0x7C].hex(),
        )

    def sync_session(
        self, wire=None, match_index=0, in_match=False, disconnected=False
    ):
        """Port the 1v1 Unranked/Direct LOCAL and SESSION contract from GameBridge.

        The guest's lock-in and controller fields are never overwritten. SESSION
        stays untouched while a rollback match runs.
        """
        b, m = self.block, self.m
        local = bytearray(m.read(b.local, b.local_size))
        ticket = (self.result or {}).get("ticketResponse", {})
        active = wire is not None
        host = (wire.state if wire.state["host"] else wire.peer) if active else {}
        local[4] = (3 if in_match else 2 if host else 1) if active else 0
        local[5] = self.local_slot if active else 0xFF
        lock = wire.peer.get("lock", {}) if active else {}
        local[6] = int(bool(lock.get("ready")) and lock.get("game") == match_index + 1)
        local[7] = int(disconnected)
        names = ticket.get("players", [])
        peer_name = next(
            (p["displayName"] for p in names if not p["isLocalPlayer"]), ""
        )
        local[8:0x28] = ppom.u16s(peer_name, 16)
        # Only the host-owned prefix; all fields after +0x28 belong to the plugin
        # or optional features this prototype does not implement.
        self.publish(b.local, local[:0x28])
        if in_match:
            self.published.pop(b.session, None)
            return
        se = bytearray(b.session_size)
        setup = host.get("match_setup", {})
        ready = active and setup.get("game") == match_index + 1
        se[4] = (2 if ready else 1) if active else 0
        se[5] = self.mode
        se[6] = (match_index + 1) & 255 if active else 0
        se[7] = host.get("winner", 255)
        se[8:10] = struct.pack(">H", setup["stage"] if ready else 0xFFFF)
        se[10] = setup.get("asl", 0) if ready else 0
        se[11] = 2 if active else 0
        players = setup.get("players", []) if ready else []
        for i in range(4):
            off = 0x0C + 0x80 * i
            se[off + 1], se[off + 3] = 255, 255
            if i < len(names):
                se[off + 4 : off + 0x24] = ppom.u16s(names[i]["displayName"], 16)
                se[off + 0x24 : off + 0x36] = ppom.u16s(names[i]["connectCode"], 9)
            if i < len(players):
                pl = players[i]
                if (
                    len(pl) != 3
                    or not 0 <= pl[0] < 0x3E
                    or len(bytes.fromhex(pl[2])) != 60
                ):
                    raise RuntimeError("unsupported host player setup")
                se[off : off + 3] = bytes([1, pl[0], pl[1]])
                se[off + 0x40 : off + 0x7C] = bytes.fromhex(pl[2])
        if active and ticket.get("anyonePicks", False):
            se[0x276] = 1
            for i in range(2):
                se[0x0C + 0x80 * i + 0x36] = 1
            for stage in ticket["stages"]:
                if not 0 <= stage < 256:
                    raise ValueError("Stage outside PPOM stage mask")
                se[0x280 + stage // 8] |= 1 << (stage % 8)
        rules = ticket.get("rules")
        if active and rules:
            ru = bytearray(32)
            ru[:8] = bytes(
                [
                    1,
                    {"time": 0, "stock": 1, "coin": 2}[rules["style"]],
                    rules["stocks"],
                    rules["timeMinutes"],
                    round(rules["damageRatio"] * 10),
                    int(rules["teamAttack"]),
                    int(rules["pause"] == "normal"),
                    rules["itemFrequency"],
                ]
            )
            ru[8:16] = bytes(rules["itemSwitch"])
            ru[16:20] = bytes(
                [
                    int(rules["hazards"]),
                    int(rules["autoLCancel"]),
                    int(rules["inputAssist"]),
                    int(rules.get("stockControl", False)),
                ]
            )
            hazard = rules.get("hazardStages", [])[:8]
            ru[24 : 24 + len(hazard)] = bytes(hazard)
            se[0x220:0x240] = ru
        self.publish(b.session, se)

    def close(self):
        self.cancel()
        (self.out / "menu-bridge.json").write_text(
            json.dumps(
                {"commands": dict(self.commands), "events": self.events}, indent=2
            )
        )
