"""Protocol regression tests without accounts or game processes."""

import json
import socket
import struct
import unittest

from host_setup import direct_setup
from online_protocol import Wire, setup_hash


class ProtocolTests(unittest.TestCase):
    def test_setup_hash_matches_native_xxh3_byte_order(self):
        self.assertEqual(setup_hash(bytes(range(256)), 120), "a38bfc3f985f336f")

    def test_fragment_sizes_match_dolphin_reassembler(self):
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver.bind(("127.0.0.1", 0))
        receiver.settimeout(1)
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(receiver.close)
        self.addCleanup(sender.close)
        wire = Wire.__new__(Wire)
        wire.sock, wire.remote, wire.message_id = sender, receiver.getsockname(), 0
        state = dict(t="st", tasks=["task-name"] * 600)
        wire.send_control(state)
        parts = {}
        while True:
            data = receiver.recv(65536)
            self.assertLessEqual(len(data), 1200)
            self.assertEqual(data[0:1], b"F")
            message, index, count = struct.unpack_from("<IBB", data, 1)
            self.assertEqual(message, 1)
            if index + 1 < count:
                self.assertEqual(len(data) - 7, 1193)
            parts[index] = data[7:]
            if len(parts) == count:
                break
        self.assertEqual(json.loads(b"".join(parts[i] for i in range(count))), state)

    def test_old_match_state_cannot_reopen_previous_barrier(self):
        wire = Wire.__new__(Wire)
        wire.state = dict(slot=0)
        wire.peer = dict(match=2, at_s=False)
        wire.history = []
        wire.control(
            json.dumps(dict(t="st", v=1, slot=1, host=False, match=1, at_s=True))
        )
        self.assertEqual(wire.peer, dict(match=2, at_s=False))

    def test_peer_role_must_match_ticket(self):
        wire = Wire.__new__(Wire)
        wire.state = dict(slot=0)
        wire.peer = {}
        wire.history = []
        with self.assertRaises(RuntimeError):
            wire.control(json.dumps(dict(t="st", v=1, slot=0, host=True, match=0)))

    def test_direct_waits_for_pick_and_preserves_player_order(self):
        locks = [
            dict(ready=True, game=1, kind=2, costume=0, pv="00" * 60, stage=65534),
            dict(ready=True, game=1, kind=3, costume=1, pv="00" * 60, stage=65535),
        ]
        self.assertIsNone(direct_setup(1, locks, True))
        locks[1].update(stage=45, asl=2)
        setup = direct_setup(1, locks, True)
        self.assertEqual(setup["stage"], 45)
        self.assertEqual(setup["asl"], 2)
        self.assertEqual([p[0] for p in setup["players"]], [2, 3])
        self.assertIsNone(direct_setup(2, locks, True))


if __name__ == "__main__":
    unittest.main()
