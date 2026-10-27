"""Print one model-native sample from the mock camera.

Does not import torch or ROS. ACAM_MOCK is forced to 1 so this script never
opens a real camera. The same Pipeline.wait_sample() call is what a training
loop uses; see docs/model.md.
"""

import os
import sys
from pathlib import Path

os.environ["ACAM_MOCK"] = "1"

_CONFIGS = ("RelWithDebInfo", "Release", "Debug", "")


def _find_dir(base: Path, marker: str):
    for cfg in _CONFIGS:
        directory = base / cfg if cfg else base
        if directory.is_dir() and any(directory.glob(marker)):
            return directory
    return None


def _bootstrap() -> None:
    root = Path(__file__).resolve().parents[1]
    pyd_dir = _find_dir(root / "build" / "bindings" / "python", "pyainstec*.pyd")
    dll_dir = _find_dir(root / "build" / "camsdk_c", "ainstec_cam.dll")
    extra = [d for d in (dll_dir, pyd_dir) if d is not None]
    for directory in extra:
        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(str(directory))
    if extra:
        os.environ["PATH"] = os.pathsep.join(str(d) for d in extra) + os.pathsep + os.environ.get("PATH", "")
    if pyd_dir is None:
        sys.exit("pyainstec is not built; run cmake --build --preset msvc-x64 first")
    sys.path.insert(0, str(pyd_dir))


def main() -> None:
    _bootstrap()
    import pyainstec as ac

    ctx = ac.Context()
    devices = ctx.query_devices(1000)
    if not devices:
        sys.exit("no camera discovered")
    device = devices[0]
    device.open()
    try:
        pipe = ac.Pipeline(ctx)
        pipe.open(device)
        pipe.start()
        try:
            pipe.trigger()
            sample = pipe.wait_sample(5000)
        finally:
            pipe.stop()
    finally:
        device.close()

    # The sample owns its frames, so the views stay valid after the device closes.
    depth_k = sample.calibration["depth"]
    rgb_k = sample.calibration["rgb"]
    print(f"index={sample.index} timestamp_s={sample.timestamp_s:.6f}")
    print(
        f"depth shape={sample.depth.shape} dtype={sample.depth.dtype} "
        f"scale_m={sample.depth_scale_m} valid={sample.depth_valid}/{sample.depth_count}"
    )
    print(f"rgb shape={None if sample.rgb is None else sample.rgb.shape}")
    print(f"depth fx={depth_k['fx']} rgb fx={None if rgb_k is None else rgb_k['fx']}")
    print(f"depth[0,0]_mm={float(sample.depth[0, 0]):.4f}")


if __name__ == "__main__":
    main()
