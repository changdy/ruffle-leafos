"""Apply a white-balance adjustment to the AisOS built-in DRM output."""
import ctypes as c
import fcntl
from contextlib import contextmanager
from functools import lru_cache
import os
from pathlib import Path
import sys
import tempfile
import time


MAX_LEVEL = 20
CONFIG = Path("/storage/.config/rgbox-screen-color")
LEVEL = CONFIG / "level"
RUNTIME = Path("/run/rgbox-screen-color")
PREVIEW = RUNTIME / "preview"
DRM_MODE_CONNECTOR_DSI = 16

U32, Int = c.c_uint32, c.c_int
Gamma = c.POINTER(c.c_uint16)


# Leading fields of the libdrm result structures used below.
class Resources(c.Structure):
    _fields_ = [("count_fbs", Int), ("fbs", c.POINTER(U32)),
                ("count_crtcs", Int), ("crtcs", c.POINTER(U32)),
                ("count_connectors", Int), ("connectors", c.POINTER(U32))]


class Connector(c.Structure):
    _fields_ = [("id", U32), ("encoder", U32), ("type", U32), ("index", U32)]


class Encoder(c.Structure):
    _fields_ = [("id", U32), ("type", U32), ("crtc", U32)]


class Crtc(c.Structure):
    _fields_ = [("id", U32), ("buffer", U32), ("x", U32), ("y", U32),
                ("width", U32), ("height", U32), ("mode_valid", Int),
                ("mode", c.c_ubyte * 68), ("gamma_size", Int)]  # drmModeModeInfo is 68 bytes.


@lru_cache(maxsize=1)
def drm_api():
    drm = c.CDLL("libdrm.so.2", use_errno=True)
    signatures = {
        "drmModeGetResources": (c.POINTER(Resources), Int),
        "drmModeFreeResources": (None, c.POINTER(Resources)),
        "drmModeGetConnectorCurrent": (c.POINTER(Connector), Int, U32),
        "drmModeFreeConnector": (None, c.POINTER(Connector)),
        "drmModeGetEncoder": (c.POINTER(Encoder), Int, U32),
        "drmModeFreeEncoder": (None, c.POINTER(Encoder)),
        "drmModeGetCrtc": (c.POINTER(Crtc), Int, U32),
        "drmModeFreeCrtc": (None, c.POINTER(Crtc)),
        "drmModeCrtcGetGamma": (Int, Int, U32, U32, Gamma, Gamma, Gamma),
        "drmModeCrtcSetGamma": (Int, Int, U32, U32, Gamma, Gamma, Gamma),
    }
    for name, (result, *arguments) in signatures.items():
        function = getattr(drm, name)
        function.argtypes = arguments
        function.restype = result
    return drm


def check(result, operation):
    if result < 0:
        error = c.get_errno() if result == -1 else -result
        raise OSError(error, f"{operation}: {os.strerror(error)}")


@contextmanager
def owned(pointer, release, operation):
    if not pointer:
        raise OSError(f"{operation} failed")
    try:
        yield pointer
    finally:
        release(pointer)


@lru_cache(maxsize=MAX_LEVEL + 1)
def gamma_table(size, level):
    channel = c.c_uint16 * size
    red = channel(*((i * 65535 + (size - 1) // 2) // (size - 1) for i in range(size)))
    green = channel(*((value * (250 - level) + 125) // 250 for value in red))
    blue = channel(*((value * (100 - level) + 50) // 100 for value in red))
    return red, green, blue


class Display:
    def __init__(self, drm, fd):
        self.drm, self.fd = drm, fd
        self.connector = self.find_panel()

    def find_panel(self):
        api, fd = self.drm, self.fd
        with owned(api.drmModeGetResources(fd), api.drmModeFreeResources, "Read DRM resources") as ptr:
            resources = ptr.contents
            for index in range(resources.count_connectors):
                connector = api.drmModeGetConnectorCurrent(fd, resources.connectors[index])
                if connector:
                    with owned(connector, api.drmModeFreeConnector, "Read connector"):
                        info = connector.contents
                        if (info.type, info.index) == (DRM_MODE_CONNECTOR_DSI, 1):
                            return info.id
        raise OSError("Built-in DSI-1 output unavailable")

    def output(self):
        api, fd = self.drm, self.fd
        with owned(api.drmModeGetConnectorCurrent(fd, self.connector),
                   api.drmModeFreeConnector, "Read screen connector") as connector:
            encoder = connector.contents.encoder
        if not encoder:
            raise OSError("Built-in screen is inactive")
        with owned(api.drmModeGetEncoder(fd, encoder), api.drmModeFreeEncoder,
                   "Read screen encoder") as ptr:
            crtc = ptr.contents.crtc
        with owned(api.drmModeGetCrtc(fd, crtc), api.drmModeFreeCrtc, "Read screen CRTC") as ptr:
            output = ptr.contents
            if not output.mode_valid or not 2 <= output.gamma_size <= 4096:
                raise OSError("Built-in screen gamma table is unavailable")
            return crtc, output.gamma_size, bytes(output.mode)

    def apply(self, level, previous_output=None):
        output = self.output()
        crtc, size, _ = output
        api, fd = self.drm, self.fd
        table = gamma_table(size, level)
        channel = c.c_uint16 * size
        current = channel(), channel(), channel()
        check(api.drmModeCrtcGetGamma(fd, crtc, size, *current), "Read screen color")
        if output != previous_output or any(bytes(a) != bytes(b) for a, b in zip(current, table)):
            check(api.drmModeCrtcSetGamma(fd, crtc, size, *table), "Apply screen color")
        return output


@contextmanager
def panel():
    endpoints = sorted(Path("/sys/class/drm").glob("card[0-9]*-DSI-1"))
    if len(endpoints) != 1:
        raise OSError("Expected a single built-in DSI-1 output")
    card = endpoints[0].name.removesuffix("-DSI-1")
    fd = os.open(f"/dev/dri/{card}", os.O_RDWR | os.O_CLOEXEC)
    try:
        yield Display(drm_api(), fd)
    finally:
        os.close(fd)


@contextmanager
def exclusive(nonblocking=False):
    RUNTIME.mkdir(mode=0o700, exist_ok=True)
    with (RUNTIME / "lock").open("a") as lock:
        mode = fcntl.LOCK_EX | (fcntl.LOCK_NB if nonblocking else 0)
        try:
            fcntl.flock(lock, mode)
        except BlockingIOError:
            yield False
        else:
            yield True


def saved_level():
    try:
        level = int(LEVEL.read_text(encoding="ascii"))
    except FileNotFoundError:
        return None
    if not 0 <= level <= MAX_LEVEL:
        raise ValueError("Invalid screen color level")
    return level


def save_level(level):
    output = tempfile.NamedTemporaryFile(dir=CONFIG, mode="w", encoding="ascii", delete=False)
    staged = Path(output.name)
    try:
        with output:
            output.write(f"{level}\n")
            output.flush()
            os.fsync(output.fileno())
        staged.replace(LEVEL)
    finally:
        staged.unlink(missing_ok=True)


def watch():
    last_error = None
    previous_output = None
    boot_offset = time.clock_gettime(time.CLOCK_BOOTTIME) - time.monotonic()
    while True:
        new_offset = time.clock_gettime(time.CLOCK_BOOTTIME) - time.monotonic()
        if new_offset - boot_offset > 0.1:
            previous_output = None
        boot_offset = new_offset
        try:
            with exclusive(nonblocking=True) as acquired:
                if acquired:
                    level = saved_level()
                    if level is not None or PREVIEW.exists():
                        with panel() as display:
                            previous_output = display.apply(level or 0, previous_output)
                        PREVIEW.unlink(missing_ok=True)
            last_error = None
        except (OSError, ValueError) as error:
            previous_output = None
            if str(error) != last_error:
                print(error, file=sys.stderr, flush=True)
                last_error = str(error)
        time.sleep(2)


if __name__ == "__main__":
    if sys.argv[1:] != ["--watch"]:
        sys.exit("usage: screen_color.py --watch")
    watch()
