"""Client for Brawl Online's production UDP session protocol.

The native recomp owns all game simulation, snapshots and Gekko actors. This
sidecar owns connection/control messages and applies the host's setup at native
safe-point gates over an authenticated local connection. It never simulates either fighter.
"""

import collections
import xxhash
import json
import socket
import struct
import threading
import time
from pathlib import Path


class Memory:
    def __init__(self, path, token=None):
        if token is None:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(15)
            self.sock.connect(str(path))
        else:
            port = int(Path(path).read_text().strip())
            if not 0 < port <= 65535:
                raise ValueError("Invalid control endpoint port")
            self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("rb")
        if token is not None:
            self.call("auth " + token)

    def call(self, line):
        self.sock.sendall((line + "\n").encode())
        s = self.file.readline().decode().strip()
        if not s or s.startswith("error"):
            raise RuntimeError(s or "recomp IPC closed")
        return s

    def read(self, a, n):
        return b"".join(
            bytes.fromhex(self.call(f"read {a + i:x} {min(n - i, 16384):x}"))
            for i in range(0, n, 16384)
        )

    def write(self, a, b):
        self.call(f"write {a:x} {b.hex()}")

    def u32(self, a):
        return struct.unpack(">I", self.read(a, 4))[0]

    def w32(self, a, v):
        self.write(a, struct.pack(">I", v))

    def status(self):
        return self.call("status").split()

    def close(self):
        self.file.close()
        self.sock.close()

    # Adapter for the upstream PPOM inspection helpers.
    read_mem = read
    write_mem = write
    read_u32 = u32


class Wire:
    def __init__(
        self, local, remote, raw_port, native_port, out, delay_ms=0, local_slot=1
    ):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", local))
        self.sock.setblocking(False)
        self.raw = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.raw.bind(("127.0.0.1", raw_port))
        self.raw.setblocking(False)
        self.remote = remote
        self.native = ("127.0.0.1", native_port)
        self.out = Path(out)
        self.delay = delay_ms / 1000
        self.state = dict(
            t="st",
            v=1,
            gb=1,
            host=local_slot == 0,
            slot=local_slot,
            name="BrawlRecompiled",
            game="RSBE01",
            sel={"character": 50},
            match=0,
            at_s=False,
            applied=False,
            at_start=False,
            go=False,
            setup="0000000000000000",
            hold=0,
        )
        self.peer = {}
        self.last_received = time.monotonic()
        self.message_id = 0
        self.history = []
        self.fragments = {}
        self.error = None
        self.stop = False
        self.queue = collections.deque()
        self.count = collections.Counter()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def control(self, text):
        if len(text) > 16384:
            return
        o = json.loads(text)
        if o.get("t") in ("leave", "desync"):
            self.error = o
        if o.get("t") == "st":
            if (
                o.get("v") != 1
                or o.get("slot") != 1 - self.state["slot"]
                or o.get("host") != (self.state["slot"] == 1)
            ):
                raise RuntimeError("peer role or protocol version differs")
            self.last_received = time.monotonic()
            if self.peer and o.get("match", -1) < self.peer.get("match", -1):
                return
            if self.peer and o.get("match") != self.peer.get("match"):
                # Previous barrier data must not satisfy the next match.
                self.peer.clear()
            self.peer.update(o)
            mark = {
                k: o.get(k)
                for k in ("at_s", "applied", "at_start", "go", "setup", "seed", "match")
            }
            if not self.history or self.history[-1]["state"] != mark:
                self.history.append({"time": time.time(), "state": mark})

    def send_control(self, state):
        data = json.dumps(state, separators=(",", ":")).encode()
        if len(data) > 16384:
            raise ValueError("control message exceeds protocol limit")
        if len(data) + 1 <= 1200:
            self.sock.sendto(b"C" + data, self.remote)
        else:
            self.message_id = (self.message_id + 1) & 0xFFFFFFFF
            count = (len(data) + 1192) // 1193
            for i in range(count):
                self.sock.sendto(
                    b"F"
                    + struct.pack("<IBB", self.message_id, i, count)
                    + data[i * 1193 : (i + 1) * 1193],
                    self.remote,
                )

    def run(self):
        sent = 0
        try:
            while not self.stop:
                now = time.monotonic()
                if now - sent >= 0.05:
                    self.send_control(self.state)
                    sent = now
                if now - self.last_received > 60:
                    raise TimeoutError("peer stopped responding")
                while self.queue and self.queue[0][0] <= now:
                    _, s, data, to = self.queue.popleft()
                    s.sendto(data, to)
                for s in (self.sock, self.raw):
                    for _ in range(256):
                        try:
                            b, src = s.recvfrom(65536)
                        except BlockingIOError:
                            break
                        if s is self.raw:
                            if src == self.native:
                                self.queue.append(
                                    (now + self.delay, self.sock, b"G" + b, self.remote)
                                )
                                self.count["recomp_gekko"] += 1
                            continue
                        if src != self.remote or not b:
                            continue
                        self.last_received = now
                        kind = chr(b[0])
                        self.count[kind] += 1
                        if kind == "C":
                            self.control(b[1:])
                        elif kind == "F" and len(b) > 7:
                            mid, idx, count = struct.unpack_from("<IBB", b, 1)
                            if not 0 < count <= 14 or idx >= count:
                                continue
                            if mid not in self.fragments:
                                self.fragments = {mid: {}}
                            parts = self.fragments[mid]
                            parts[idx] = b[7:]
                            if len(parts) == count:
                                self.control(b"".join(parts[i] for i in range(count)))
                                self.fragments.clear()
                        elif kind == "P" and len(b) == 9:
                            self.sock.sendto(b"Q" + b[1:], self.remote)
                        elif kind == "G":
                            self.queue.append(
                                (now + self.delay, self.raw, b[1:], self.native)
                            )
                        elif kind == "B":
                            at = 1
                            msgs = []
                            while at < len(b):
                                if at + 2 > len(b):
                                    raise RuntimeError("truncated Gekko bundle")
                                n = struct.unpack_from("<H", b, at)[0]
                                at += 2
                                if not n or at + n > len(b) or len(msgs) >= 64:
                                    raise RuntimeError("invalid Gekko bundle")
                                msgs.append(b[at : at + n])
                                at += n
                            for msg in msgs:
                                self.queue.append(
                                    (now + self.delay, self.raw, msg, self.native)
                                )
                time.sleep(0.001)
        except Exception as e:
            self.error = str(e)

    def close(self):
        self.stop = True
        self.thread.join(2)
        self.sock.sendto(
            b"C"
            + json.dumps(
                {"t": "leave", "slot": self.state["slot"], "why": "client closed"}
            ).encode(),
            self.remote,
        )
        self.sock.close()
        self.raw.close()
        (self.out / "wire.json").write_text(
            json.dumps(
                {
                    "events": self.history,
                    "packet_counts": dict(self.count),
                    "error": self.error,
                },
                indent=2,
            )
        )


def setup_key(m):
    gg = m.u32(0x805A00E0)
    mm = m.u32(gg + 8)
    p = m.read(mm, 0x320)
    key = bytearray(p[8:40])
    for i in range(4):
        pl = p[0x98 + i * 0x5C :]
        key.extend(pl[k] for k in (0, 1, 4, 5))
        if p[0x13]:
            key.append(pl[0xB])
    cfg = m.read(0x805B7480, 0xB9)
    if any(x >= 4 and x < 8 for x in cfg[0xB5:0xB9]):
        raise RuntimeError("online client currently requires GameCube layouts")
    key.extend(cfg[:48])
    key.extend(cfg[0xB5:0xB9])
    rule = m.read(m.u32(gg + 0x1C), 16)
    key.extend(rule)
    if rule[5] == 2:
        for i in range(4):
            key.extend(p[0x98 + i * 0x5C + 0x26 : 0x98 + i * 0x5C + 0x28])
    key.append(p[0x29] & 0x20)
    key.extend(p[0x30:0x40])
    menu = m.read(0x804E0000, 0x2520)
    for o in range(0, len(menu) - 0x20 + 1, 4):
        size = int.from_bytes(menu[o : o + 2], "big")
        typ = menu[o + 2]
        text = menu[o + 6]
        if (
            size % 4
            or not 0x20 <= size <= 0x400
            or o + size > len(menu)
            or typ > 2
            or not 0x1C <= text < size
        ):
            continue
        options = int.from_bytes(menu[o + 0x18 : o + 0x1C], "big")
        if (typ == 0 and not 0x804E0000 <= options < 0x804E2520) or (
            typ != 0 and text != 0x20
        ):
            continue
        t = menu[o + text : o + size]
        percent = t.find(b"%")
        if (
            not t[:1].isalpha()
            or percent < 0
            or any(x < 32 or x >= 127 for x in t[:percent])
            or b"Character Select" in t[:percent]
        ):
            continue
        key.extend(menu[o + 8 : o + 12])
    # Special modes require additional setup-key fields.
    special = m.read(m.u32(gg + 0x1C) + 0x18, 8)
    if any(special):
        raise RuntimeError("special modes are not supported by this client")
    return bytes(key)


def setup_hash(key, start):
    b = key + struct.pack(">IB", start, 1)
    return xxhash.xxh3_64_hexdigest(b)


def apply_sync(m, b):
    raw = bytes.fromhex(b["game_frame"])
    if len(raw) != 24 or len(b["rng"]) != 3 or sorted(b["start_pos"]) != list(range(4)):
        raise RuntimeError("invalid host sync block")
    for off in (4, 12, 20):
        m.write(0x901812A0 + off, raw[off : off + 4])
    m.w32(m.u32(0x8059FFAC) + 0x100, b["app"])
    for a, v in zip((0x805A00BC, 0x805A0424, 0x8059FF58), b["rng"]):
        m.w32(a, v)
    m.w32(0x8059C668, b["serial"])
    stage = m.u32(0x80B8A428)
    for i, v in enumerate(b["start_pos"]):
        m.w32(stage + 0x1B4 + 4 * i, v)


def apply_tasks(m, want):
    if len(want) != 71:
        raise RuntimeError("invalid task list count")
    sched = m.u32(0x805A0068)
    li = 0
    changed = 0
    for base, prev, nxt, count in (
        (0x14, 4, 8, 17),
        (0x58, 12, 16, 40),
        (0xF8, 20, 24, 14),
    ):
        for i in range(count):
            seq = []
            t = m.u32(sched + base + 4 * i)
            seen = set()
            while t:
                if t in seen or len(seq) >= 4096:
                    raise RuntimeError("invalid native task chain")
                seen.add(t)
                name = m.read(m.u32(t), 48).split(b"\0")[0].decode()
                seq.append((name, t))
                t = m.u32(t + nxt)
            names = [n for n, _ in seq]
            if collections.Counter(names) != collections.Counter(want[li]):
                raise RuntimeError(
                    f"task set differs in list {li}: {names} vs {want[li]}"
                )
            if names != want[li]:
                pool = collections.defaultdict(collections.deque)
                for n, a in seq:
                    pool[n].append(a)
                order = [pool[n].popleft() for n in want[li]]
                m.w32(sched + base + 4 * i, order[0] if order else 0)
                for j, a in enumerate(order):
                    m.w32(a + prev, order[j - 1] if j else 0)
                    m.w32(a + nxt, order[j + 1] if j + 1 < len(order) else 0)
                changed += 1
            li += 1
    return changed
