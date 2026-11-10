"""Persistent 1v1 online connection driven by the guest PPOnline UI."""

import json
import secrets

from host_setup import direct_setup, read_sync, read_tasks
from online_menu import MenuBridge
from online_protocol import Wire, apply_sync, apply_tasks, setup_hash, setup_key


class OnlineLoop(MenuBridge):
    def __init__(self, *args, delay_ms=0, **kwargs):
        self.wire = None
        self.armed = False
        self.match_index = 0
        self.gate = None
        self.in_match = False
        self.delay_ms = delay_ms
        self.matches = []
        self.connections = 0
        super().__init__(*args, **kwargs)

    def cancel(self):
        if self.wire:
            self.m.call("disarm")
            self.wire.close()
            (self.out / "wire.json").replace(
                self.out / f"wire.connection-{self.connections}.json"
            )
            self.wire = None
        self.armed = self.in_match = False
        self.gate = None
        self.match_index = 0
        super().cancel()

    def tick(self):
        super().tick()
        if self.state == 4 and self.wire is None:
            ticket = self.result["ticketResponse"]
            self.local_slot = 0 if ticket["isHost"] else 1
            self.m.call(f"slot {self.local_slot}")
            if len(ticket["players"]) != 2:
                raise RuntimeError("Only 1v1 sessions are supported")
            if not self.result.get("p2p", {}).get("connected"):
                raise RuntimeError("Peer connection was not established")
            peer = self.result["p2p"]["peerAddress"]
            ip, port = peer.rsplit(":", 1)
            self.wire = Wire(
                self.port,
                (ip, int(port)),
                self.port + 1,
                self.port + 2,
                self.out,
                self.delay_ms,
                self.local_slot,
            )
            self.wire.state["name"] = self.user.display_name
            if self.local_slot == 0:
                self.wire.state["seed"] = secrets.randbits(32)
            self.connections += 1
            self.events.append(
                {
                    "event": "connected",
                    "connection": self.connections,
                    "match_id": ticket["matchId"],
                }
            )
        wire = self.wire
        if wire is None:
            self.sync_session()
            return
        if wire.error:
            self.events.append({"event": "peer_error", "detail": wire.error})
            self.m.call("disarm")
            self.sync_session(
                wire, self.match_index, in_match=self.in_match, disconnected=True
            )
            # The guest shows DISCONNECTED, exits the game and requests cleanup.
            self.state = 0
            return
        wire.state["lock"] = self.local_lock()
        wire.state["sel"] = {
            "character": wire.state["lock"]["css"],
            "costume": wire.state["lock"]["costume"],
        }
        native = self.m.call("rollback").split()
        if self.armed and native[0] == "ended":
            self.matches.append(
                dict(
                    connection=self.connections,
                    game=self.match_index + 1,
                    frames=int(native[1]),
                    rollbacks=int(native[2]),
                    advances=int(native[3]),
                    desyncs=int(native[4]),
                )
            )
            if int(native[4]):
                raise RuntimeError("native rollback detected a desync")
            self.events.append({"event": "match_ended", **self.matches[-1]})
            self.match_index += 1
            self.armed = self.in_match = False
            self.m.call("disarm")
            wire.state.update(
                match=self.match_index,
                at_s=False,
                applied=False,
                at_start=False,
                go=False,
                setup="0000000000000000",
            )
            for key in (
                "match_setup",
                "init",
                "init_match",
                "sync",
                "tasks",
                "holds",
                "setup_key",
                "setup_start",
                "setup_cv",
            ):
                wire.state.pop(key, None)
            self.gate = None
            (self.out / "matches.json").write_text(json.dumps(self.matches, indent=2))
        if self.local_slot == 0 and not self.armed and wire.peer:
            ticket = self.result["ticketResponse"]
            if "match_setup" not in wire.state:
                locks = [wire.state["lock"].copy(), wire.peer.get("lock", {}).copy()]
                if self.mode == 1:
                    stages = ticket["stages"]
                    if not stages:
                        raise RuntimeError("Unranked ticket has no stages")
                    locks[0]["stage"] = secrets.choice(stages)
                    locks[0]["asl"] = 0
                proposal = direct_setup(
                    self.match_index + 1,
                    locks,
                    ticket.get("alternateStages", False),
                )
                if proposal:
                    wire.state["match_setup"] = proposal
        host = wire.state if self.local_slot == 0 else wire.peer
        setup = host.get("match_setup", {})
        if (
            not self.armed
            and setup.get("game") == self.match_index + 1
            and "seed" in host
        ):
            # Arm before publishing SS_MATCH_READY: the guest can now leave training
            # and construct its stage without racing the seed/barrier setup.
            self.m.call("seed " + str(host["seed"]))
            local = self.m.read(self.block.local, self.block.local_size)
            self.m.call(
                f"arm {self.match_index} {local[0x35]} {self.block.mailbox:x} "
                f"{self.block.mailbox_size + self.block.local_size:x}"
            )
            self.armed = True
            self.events.append(
                {"event": "armed", "game": setup["game"], "setup": setup}
            )
        self.sync_session(wire, self.match_index, self.in_match)
        if not self.armed:
            return
        phase = self.m.status()[0]
        if self.local_slot == 0:
            self.host_gate(phase)
            return
        if phase == "stage" and self.gate != "stage":
            if "init" not in host:
                return
            init = bytes.fromhex(host["init"])
            if len(init) != 32:
                raise RuntimeError("invalid host init")
            addr = self.m.u32(self.m.u32(0x805A00E0) + 8) + 8
            mine = bytearray(self.m.read(addr, 32))
            if (
                all(mine[i] == init[i] for i in range(32) if i != 0x14)
                and init[0x14] <= 15
            ):
                mine[0x14] = init[0x14]
                self.m.write(addr, mine)
            self.gate = "stage"
            self.m.call("release")
        elif phase == "barrier" and self.gate != "barrier":
            if not wire.state["at_s"]:
                key = setup_key(self.m)
                hold = self.sample_hold()
                wire.state.update(
                    at_s=True,
                    hold=hold,
                    setup=setup_hash(key, 120),
                    setup_key=key.hex(),
                    setup_start=120,
                    setup_cv=1,
                )
            if not all(
                k in host for k in ("sync", "tasks", "setup_key", "holds")
            ) or not host.get("at_s"):
                return
            key = setup_key(self.m)
            hostkey = bytes.fromhex(host["setup_key"])
            evidence = {
                "recomp": key.hex(),
                "dolphin": hostkey.hex(),
                "equal": key == hostkey,
            }
            (self.out / f"setup-game-{self.match_index + 1}.json").write_text(
                json.dumps(evidence, indent=2)
            )
            if key != hostkey:
                raise RuntimeError("setup differs; see setup-game evidence")
            self.m.call("countdown " + " ".join(str(h) for h in host["holds"]))
            start = host.get("setup_start", 120)
            if start != 120 or host.get("setup_cv", 1) != 1:
                raise RuntimeError("unsupported countdown contract")
            h = setup_hash(key, start)
            if h != host["setup"]:
                raise RuntimeError("setup checksum differs")
            reordered = apply_tasks(self.m, host["tasks"])
            apply_sync(self.m, host["sync"])
            wire.state.update(at_s=True, applied=True, setup=h)
            self.in_match = True
            self.sync_session(wire, self.match_index, True)
            self.events.append(
                {
                    "event": "setup_verified",
                    "game": self.match_index + 1,
                    "reordered": reordered,
                }
            )
            self.gate = "barrier"
            self.m.call("release")
        elif phase == "start" and self.gate != "start":
            wire.state["at_start"] = True
            self.gate = "start"
            self.m.call("release")
        if self.gate == "start" and host.get("go"):
            self.m.call("go")
            self.gate = "running"
            self.events.append({"event": "go", "game": self.match_index + 1})

    def sample_hold(self):
        return int(self.m.call("hold"))

    def host_gate(self, phase):
        wire = self.wire
        peer = wire.peer
        if phase == "stage" and self.gate != "stage":
            address = self.m.u32(self.m.u32(0x805A00E0) + 8) + 8
            wire.state.update(
                init=self.m.read(address, 32).hex(), init_match=self.match_index
            )
            self.gate = "stage"
            self.m.call("release")
        elif phase == "barrier" and self.gate != "barrier":
            if not wire.state["at_s"]:
                key = setup_key(self.m)
                wire.state.update(
                    at_s=True,
                    applied=True,
                    hold=self.sample_hold(),
                    setup=setup_hash(key, 120),
                    setup_key=key.hex(),
                    setup_start=120,
                    setup_cv=1,
                    sync=read_sync(self.m),
                    tasks=read_tasks(self.m),
                    ports=3,
                )
            if peer.get("match") != self.match_index or not peer.get("at_s"):
                return
            if peer.get("setup") != wire.state["setup"]:
                raise RuntimeError("Peer setup checksum differs")
            if "hold" not in peer:
                return
            holds = [wire.state["hold"], peer["hold"], 0, 0]
            wire.state["holds"] = holds
            if not peer.get("applied"):
                return
            self.m.call("countdown " + " ".join(str(h) for h in holds))
            self.in_match = True
            self.gate = "barrier"
            self.m.call("release")
        elif phase == "start" and self.gate != "start":
            wire.state["at_start"] = True
            self.gate = "start"
            self.m.call("release")
        if (
            self.gate == "start"
            and peer.get("at_start")
            and peer.get("match") == self.match_index
        ):
            wire.state["go"] = True
            self.m.call("go")
            self.gate = "running"
