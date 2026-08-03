"""Tests for the pyainstec bindings, run against the mock backend (ACAM_MOCK=1)."""

import gc

import numpy as np
import pytest

import pyainstec as ac

ALL_OUTPUTS = ac.OUTPUT_POINT3D | ac.OUTPUT_DEPTH | ac.OUTPUT_IR | ac.OUTPUT_RGB


@pytest.fixture()
def ctx():
    return ac.Context()


@pytest.fixture()
def device(ctx):
    devices = ctx.query_devices()
    assert len(devices) == 1
    return devices[0]


@pytest.fixture()
def open_device(device):
    device.open()
    yield device
    device.close()


@pytest.fixture()
def pipeline(ctx, open_device):
    pipe = ac.Pipeline(ctx)
    pipe.open(open_device)
    pipe.start()
    yield pipe
    pipe.stop()


@pytest.fixture()
def frameset(pipeline):
    pipeline.trigger()
    return pipeline.wait_for_frames(5000)


# ------------------------------------------------------------ context/device


def test_module_metadata():
    assert isinstance(ac.__version__, str)
    assert issubclass(ac.Error, RuntimeError)
    assert ac.OUTPUT_POINT3D == 1
    assert (ac.OUTPUT_DEPTH, ac.OUTPUT_IR, ac.OUTPUT_RGB, ac.OUTPUT_NORMALS) == (2, 4, 8, 16)
    assert (ac.FRAME_DEPTH, ac.FRAME_IR, ac.FRAME_RGB, ac.FRAME_POINTS, ac.FRAME_NORMALS) == (1, 2, 3, 4, 5)


def test_context_query_one_mock_device(ctx):
    devices = ctx.query_devices()
    assert len(devices) == 1
    info = devices[0].info
    assert info["ip"] == "10.0.0.1"
    assert info["name"] == "AinstecMock"
    assert set(info) >= {"name", "model", "ip", "serial", "mac"}


def test_open_close_is_open(device):
    assert not device.is_open
    device.open()
    assert device.is_open
    device.close()
    assert not device.is_open


def test_sensors_names_and_outputs_roundtrip(open_device):
    sensors = open_device.sensors
    assert [s.name for s in sensors] == ["3D Module", "RGB Camera"]

    s3d = sensors[0]
    original = s3d.outputs
    try:
        s3d.outputs = ac.OUTPUT_DEPTH | ac.OUTPUT_IR
        assert s3d.outputs == ac.OUTPUT_DEPTH | ac.OUTPUT_IR
    finally:
        s3d.outputs = original
    assert s3d.outputs == original


def test_sensors_keep_device_alive(ctx):
    device = ctx.query_devices()[0]
    sensors = device.sensors
    del device
    gc.collect()
    assert sensors[0].name == "3D Module"


def test_params_tree_contains_exposure(open_device):
    tree = open_device.params()
    keys = [a["key"] for a in tree["attributes"]]
    assert "Exposure" in keys


def test_param_set_get_int(open_device):
    open_device.set("Exposure", 25)
    assert open_device.get("Exposure") == 25
    assert isinstance(open_device.get("Exposure"), int)


def test_param_get_default_gain(open_device):
    assert open_device.get("Gain") == 3


def test_param_set_get_bool(open_device):
    open_device.set("HDR", True)
    value = open_device.get("HDR")
    assert value is True
    open_device.set("HDR", False)
    assert open_device.get("HDR") is False


def test_set_unknown_param_raises(open_device):
    with pytest.raises(ac.Error):
        open_device.set("NoSuchKey", 1)


def test_intrinsics_mock_values(open_device):
    intr = open_device.intrinsics()
    assert intr["width"] == 1280
    assert intr["height"] == 1024
    assert intr["fx"] == 1000.0
    assert intr["fy"] == 1000.0
    assert intr["cx"] == 640.0
    assert intr["cy"] == 512.0


def test_intrinsics_closed_device_raises(device):
    with pytest.raises(ac.Error):
        device.intrinsics()


# --------------------------------------------------------------- pipeline


def test_trigger_wait_frameset_of_four(frameset):
    assert len(frameset) == 4
    for ftype in (ac.FRAME_DEPTH, ac.FRAME_IR, ac.FRAME_RGB, ac.FRAME_POINTS):
        assert frameset.find(ftype) is not None
    assert frameset.find(ac.FRAME_NORMALS) is None
    assert len(frameset.frames) == 4
    assert [f.type for f in frameset] == [f.type for f in frameset.frames]


def test_frame_shapes_and_dtypes(frameset):
    depth = frameset.find(ac.FRAME_DEPTH)
    assert depth.size == (1280, 1024)
    d = depth.depth()
    assert d.shape == (1024, 1280)  # row-major (h, w)
    assert d.dtype == np.float32
    assert depth.data().shape == (1024, 1280)

    ir = frameset.find(ac.FRAME_IR)
    assert ir.ir().shape == (1024, 1280)
    assert ir.ir().dtype == np.uint8

    rgb = frameset.find(ac.FRAME_RGB)
    r = rgb.rgb()
    assert r.shape == (480, 640, 3)
    assert r.dtype == np.uint8

    points = frameset.find(ac.FRAME_POINTS)
    assert points.size == (1310720, 1)
    p = points.points()
    assert p.shape == (1310720, 3)
    assert p.dtype == np.float32

    assert isinstance(depth.index, int)
    assert depth.timestamp > 0.0


def test_wrong_type_accessor_raises(frameset):
    with pytest.raises(ac.Error):
        frameset.find(ac.FRAME_RGB).depth()


def test_two_triggers_increasing_index(pipeline):
    pipeline.trigger()
    first = pipeline.wait_for_frames(5000)
    pipeline.trigger()
    second = pipeline.wait_for_frames(5000)
    assert second.find(ac.FRAME_DEPTH).index > first.find(ac.FRAME_DEPTH).index


def test_wait_timeout_raises(pipeline):
    # Nothing triggered -> the wait must time out with a distinct error.
    with pytest.raises(ac.Error, match="timeout"):
        pipeline.wait_for_frames(200)


def test_auto_trigger(pipeline):
    pipeline.auto_trigger = 5.0
    assert pipeline.auto_trigger == 5.0
    try:
        assert len(pipeline.wait_for_frames(5000)) == 4
        assert len(pipeline.wait_for_frames(5000)) == 4
    finally:
        pipeline.auto_trigger = 0.0


def test_open_closed_device_fails(ctx, device):
    pipe = ac.Pipeline(ctx)
    with pytest.raises(ac.Error):
        pipe.open(device)  # device not open yet


def test_context_manager_usage(ctx):
    with ctx.query_devices()[0] as dev:
        assert dev.is_open
        with ac.Pipeline(ctx) as pipe:
            pipe.open(dev)
            pipe.start()
            pipe.trigger()
            fs = pipe.wait_for_frames(5000)
            assert len(fs) == 4
        # pipeline stopped on __exit__; frameset stays valid
        assert fs.find(ac.FRAME_DEPTH).depth().shape == (1024, 1280)
    assert not dev.is_open


# ------------------------------------------------------------------ filter


def test_decimation_filter(frameset):
    dec = ac.Filter("decimation")
    dec.set_config("factor", 8)
    out = dec.process(frameset)
    assert len(out) == 4  # unhandled frames pass through
    assert out.find(ac.FRAME_POINTS).points().shape == (1310720 // 8, 3)
    assert out.find(ac.FRAME_DEPTH).depth().shape == (128, 160)
    assert out.find(ac.FRAME_RGB).rgb().shape == (60, 80, 3)
    # input is untouched
    assert frameset.find(ac.FRAME_POINTS).points().shape == (1310720, 3)


def test_unknown_filter_raises():
    with pytest.raises(ac.Error):
        ac.Filter("nope")


def test_acam_record_playback(tmp_path, ctx, open_device):
    path = str(tmp_path / "clip.acam")
    sensors = open_device.sensors
    sensors[0].outputs = ac.OUTPUT_IR
    sensors[1].outputs = 0
    open_device.set("Exposure", 25)
    open_device.record_to_file(path)

    samples = []
    with ac.Pipeline(ctx) as pipe:
        pipe.open(open_device)
        pipe.start()
        for _ in range(2):
            pipe.trigger()
            fs = pipe.wait_for_frames(5000)
            ir = fs.find(ac.FRAME_IR).ir()
            assert ir.shape == (1024, 1280)
            samples.append(int(ir.flat[100]))
    open_device.stop_record()
    open_device.stop_record()  # idempotent

    play = ctx.load_device(path)
    play.open()
    with pytest.raises(ac.Error):
        play.set("Exposure", 1)
    assert play.get("Exposure") == 25
    assert play.intrinsics()["fx"] == 1000.0
    play.sensors[0].outputs = ac.OUTPUT_IR
    play.sensors[1].outputs = 0
    with ac.Pipeline(ctx) as pipe:
        pipe.open(play)
        pipe.start()
        for expected in samples:
            pipe.trigger()
            fs = pipe.wait_for_frames(5000)
            assert int(fs.find(ac.FRAME_IR).ir().flat[100]) == expected
        pipe.trigger()
        with pytest.raises(ac.Error):
            pipe.wait_for_frames(500)
    play.close()


def test_filter_default_is_passthrough(frameset):
    passthrough = ac.Filter("decimation")
    out = passthrough.process(frameset)
    assert out.find(ac.FRAME_POINTS).points().shape == (1310720, 3)


# ---------------------------------------------------------------- lifetime


def test_numpy_copy_survives_frameset_release(pipeline):
    pipeline.trigger()
    fs = pipeline.wait_for_frames(5000)
    depth = fs.find(ac.FRAME_DEPTH).depth()
    points = fs.find(ac.FRAME_POINTS).points()
    depth_sum = float(depth.sum())
    points_sum = float(points.sum())

    del fs
    gc.collect()

    assert float(depth.sum()) == depth_sum
    assert float(points.sum()) == points_sum
    assert depth.flags.owndata and points.flags.owndata


def test_frame_survives_frameset_release(pipeline):
    pipeline.trigger()
    fs = pipeline.wait_for_frames(5000)
    frame = fs.find(ac.FRAME_RGB)
    del fs
    gc.collect()
    assert frame.rgb().shape == (480, 640, 3)
