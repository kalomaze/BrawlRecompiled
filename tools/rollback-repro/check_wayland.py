#!/usr/bin/env python3
"""Verify a responsive Wayland compositor before creating test clients."""
import ctypes
import ctypes.util
import os
import subprocess
import sys

def check():
    library = ctypes.util.find_library('wayland-client')
    if not library:raise RuntimeError('Wayland client library is missing')
    lib = ctypes.CDLL(library)
    lib.wl_display_connect.argtypes = [ctypes.c_char_p]
    lib.wl_display_connect.restype = ctypes.c_void_p
    lib.wl_display_roundtrip.argtypes = [ctypes.c_void_p]
    lib.wl_display_roundtrip.restype = ctypes.c_int
    lib.wl_display_disconnect.argtypes = [ctypes.c_void_p]
    display = lib.wl_display_connect(None)
    if not display:
        raise RuntimeError('cannot connect to Wayland compositor')
    try:
        if lib.wl_display_roundtrip(display) < 0:
            raise RuntimeError('Wayland compositor roundtrip failed')
    finally:
        lib.wl_display_disconnect(display)

def require_wayland(env=None):
    try:
        subprocess.run([sys.executable, __file__], env=env, check=True,
                       timeout=5, capture_output=True, text=True)
    except (subprocess.SubprocessError, OSError) as exc:
        raise RuntimeError('Display prerequisite failed: no responsive Wayland compositor; game test not started') from exc

if __name__ == '__main__':
    try:
        check()
    except Exception as exc:
        print(f'Display prerequisite failed: {exc}', file=sys.stderr)
        raise SystemExit(2)
