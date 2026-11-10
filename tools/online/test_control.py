"""Run against the compiled runtime/tests/local_control_socket_tests.cpp server."""
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time
import unittest
from online_protocol import Memory


class ControlTests(unittest.TestCase):
    def start(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        endpoint = Path(temporary.name) / 'control.port'
        token = secrets.token_hex(32)
        process = subprocess.Popen([os.environ['BRAWL_CONTROL_TEST_SERVER'], str(endpoint), token],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        def cleanup():
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)
        self.addCleanup(cleanup)
        deadline = time.monotonic() + 5
        while not endpoint.exists():
            if process.poll() is not None or time.monotonic() > deadline:
                self.fail('Control test server did not publish its endpoint')
            time.sleep(.01)
        return endpoint, token, process

    def test_authenticated_client_and_large_reply(self):
        endpoint, token, process = self.start()
        client = Memory(endpoint, token)
        self.addCleanup(client.close)
        self.assertEqual(client.call('status'), 'status')
        self.assertEqual(client.call('large'), 'x' * 131072)
        self.assertEqual(client.call('quit'), 'ok')
        self.assertEqual(process.wait(timeout=5), 0)
        self.assertFalse(endpoint.exists())

    def test_fragmented_auth_and_commands(self):
        endpoint, token, process = self.start()
        client = socket.create_connection(('127.0.0.1', int(endpoint.read_text())), timeout=5)
        self.addCleanup(client.close)
        reader = client.makefile('rb'); self.addCleanup(reader.close)
        for chunk in [b'au', b'th ', token[:17].encode(), token[17:].encode(), b'\nhel', b'lo\nquit\n']:
            client.sendall(chunk)
        self.assertEqual([reader.readline() for _ in range(3)], [b'ok\n', b'hello\n', b'ok\n'])
        self.assertEqual(process.wait(timeout=5), 0)

    def test_commands_without_valid_auth_are_rejected(self):
        endpoint, token, process = self.start()
        client = socket.create_connection(('127.0.0.1', int(endpoint.read_text())), timeout=5)
        self.addCleanup(client.close)
        client.sendall(b'read 80000000 4\n')
        self.assertEqual(process.wait(timeout=5), 2)
        self.assertIn(b'authentication failed', process.stderr.read())


if __name__ == '__main__':
    unittest.main()
