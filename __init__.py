import ctypes
import sys
from pathlib import Path

import binaryninja
from binaryninja import Architecture, log_error, log_warn

_lib = None


def _load():
    global _lib
    if sys.platform != "win32":
        return
    try:
        Architecture["WASM-native"]
        return  # already installed as a native plugin
    except KeyError:
        pass

    # Native plugins must match the core's API revision, so one DLL ships per BN major.minor.
    version = ".".join(binaryninja.core_version().split(".")[:2])
    dll = Path(__file__).parent / "bin" / version / "binja_wasm.dll"
    if not dll.exists():
        log_warn(f"binja_wasm: no build for Binary Ninja {version}", "binja_wasm")
        return

    lib = ctypes.CDLL(str(dll))
    lib.CorePluginInit.restype = ctypes.c_bool
    if not lib.CorePluginInit():
        log_error(f"binja_wasm: CorePluginInit failed for {dll}", "binja_wasm")
        return
    _lib = lib


_load()
