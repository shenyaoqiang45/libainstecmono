"""Pytest bootstrap for pyainstec.

Makes the freshly-built extension importable and its DLL dependencies
resolvable, without requiring the caller to set PATH/PYTHONPATH:

- locates build/bindings/python/<config>/ (the .pyd) and
  build/camsdk_c/<config>/ (ainstec_cam.dll + vendor DLLs) relative to the
  repo root, tolerating RelWithDebInfo/Release/Debug or no config subdir;
- registers them with os.add_dll_directory (py3.8+ on Windows) and PATH;
- puts the .pyd dir on sys.path;
- defaults ACAM_MOCK=1 so the mock backend is selected.
"""

import os
import sys
from pathlib import Path

_CONFIGS = ("RelWithDebInfo", "Release", "Debug", "")


def _find_dir(base: Path, marker: str) -> Path | None:
    for cfg in _CONFIGS:
        d = base / cfg if cfg else base
        if d.is_dir() and any(d.glob(marker)):
            return d
    return None


def _setup() -> None:
    root = Path(__file__).resolve().parents[3]
    pyd_dir = _find_dir(root / "build" / "bindings" / "python", "pyainstec*.pyd")
    dll_dir = _find_dir(root / "build" / "camsdk_c", "ainstec_cam.dll")

    extra = [d for d in (dll_dir, pyd_dir) if d is not None]
    for d in extra:
        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(str(d))
    if extra:
        os.environ["PATH"] = os.pathsep.join(str(d) for d in extra) + os.pathsep + os.environ.get("PATH", "")
    if pyd_dir is not None:
        sys.path.insert(0, str(pyd_dir))

    os.environ.setdefault("ACAM_MOCK", "1")


_setup()
