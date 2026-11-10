"""Desktop display and executable conventions for recorded-match tests."""
import ctypes
import ctypes.util
import os
import sys
from pathlib import Path


def executable_name():
    return 'WiiCompiled.exe' if sys.platform == 'win32' else 'WiiCompiled'


def require_display(env=None):
    env = os.environ if env is None else env
    driver = env.get('SDL_VIDEODRIVER')
    if sys.platform == 'darwin':
        if driver not in (None, 'cocoa'):
            raise ValueError('macOS replay runs require the Cocoa display backend')
    elif sys.platform == 'win32':
        if driver not in (None, 'windows'):
            raise ValueError('Windows replay runs require the Windows display backend')
    elif sys.platform.startswith('linux'):
        if driver == 'wayland' or (driver is None and env.get('WAYLAND_DISPLAY')):
            from check_wayland import require_wayland
            require_wayland(dict(env))
        elif driver in (None, 'x11') and env.get('DISPLAY'):
            library = ctypes.util.find_library('X11')
            if not library:
                raise RuntimeError('X11 client library is missing')
            x = ctypes.CDLL(library)
            x.XOpenDisplay.argtypes = [ctypes.c_char_p]
            x.XOpenDisplay.restype = ctypes.c_void_p
            x.XCloseDisplay.argtypes = [ctypes.c_void_p]
            display = x.XOpenDisplay(env['DISPLAY'].encode())
            if not display:
                raise RuntimeError('Cannot connect to the configured X11 display')
            x.XCloseDisplay(display)
        else:
            raise RuntimeError('Select a working Wayland or X11 display for replay rendering')
    else:
        raise RuntimeError(f'Unsupported desktop platform: {sys.platform}')


def link_sd(image, target):
    # Windows file hard links need no symlink privilege. Never duplicate an SD image.
    image = Path(image).resolve(strict=True)
    if sys.platform == 'win32':
        os.link(image, target)
    else:
        target.symlink_to(image)


if __name__ == '__main__':
    require_display()
