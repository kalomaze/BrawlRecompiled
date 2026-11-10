"""Replay display selection must preserve the native desktop backend."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

path = Path(__file__).resolve().parents[1] / 'rollback-repro/host_platform.py'
spec = importlib.util.spec_from_file_location('replay_host_platform', path)
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)


class ReplayPlatformTests(unittest.TestCase):
    def test_native_desktops_do_not_require_wayland(self):
        for platform, driver in [('darwin', 'cocoa'), ('win32', 'windows')]:
            with self.subTest(platform=platform), patch.object(host.sys, 'platform', platform):
                host.require_display({})
                host.require_display({'SDL_VIDEODRIVER': driver})
                with self.assertRaises(ValueError):
                    host.require_display({'SDL_VIDEODRIVER': 'wayland'})

    def test_linux_requires_a_display(self):
        with patch.object(host.sys, 'platform', 'linux'):
            with self.assertRaises(RuntimeError):
                host.require_display({})

    def test_windows_sd_link_does_not_copy_the_image(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'shared.raw'; source.write_bytes(b'card')
            target = Path(directory) / 'sd.raw'
            with patch.object(host.sys, 'platform', 'win32'):
                host.link_sd(source, target)
            self.assertTrue(source.samefile(target))


if __name__ == '__main__':
    unittest.main()
