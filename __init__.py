import ctypes
import sys
from pathlib import Path

import binaryninja
from binaryninja import Architecture, log_error, log_warn

_lib = None
_EXT = {"win32": "dll", "linux": "so", "darwin": "dylib"}


def _load():
    global _lib
    ext = _EXT.get(sys.platform)
    if ext is None:
        return
    try:
        Architecture["WASM-native"]
        return  # already installed as a native plugin
    except KeyError:
        pass

    # Native plugins must match the core's API revision, so one library ships per BN major.minor.
    version = ".".join(binaryninja.core_version().split(".")[:2])
    lib_path = Path(__file__).parent / "bin" / version / f"binja_wasm.{ext}"
    if not lib_path.exists():
        log_warn(f"binja_wasm: no build for Binary Ninja {version}", "binja_wasm")
        return

    lib = ctypes.CDLL(str(lib_path))
    lib.CorePluginInit.restype = ctypes.c_bool
    if not lib.CorePluginInit():
        log_error(f"binja_wasm: CorePluginInit failed for {lib_path}", "binja_wasm")
        return
    _lib = lib


_load()
