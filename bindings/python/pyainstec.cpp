// pyainstec.cpp - pybind11 bindings ("pyainstec") for the ainstec_cam C ABI.
//
// Pythonic mirror of the C object model:
//   Context -> query_devices() -> [Device] -> .sensors -> [Sensor] (borrowed)
//   Pipeline(context).open(device)/start/trigger/wait_for_frames() -> FrameSet
//   Pipeline.wait_sample() -> Sample (zero-copy planes + calibration + timestamp)
//   FrameSet -> Frame (typed, refcounted); Filter(name).process(fs) -> FrameSet
// Any nonzero C return code raises pyainstec.Error (a RuntimeError subclass)
// with ac_last_error() as the message.

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include <ainstec_cam/ainstec_cam.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace {

// Exception translated to pyainstec.Error (registered in PYBIND11_MODULE).
class AcamError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Raise pyainstec.Error when a C call failed.
void check(int rc) {
    if (rc != AC_OK)
        throw AcamError(ac_last_error());
}

// Parse a JSON document with the stdlib json module.
py::object parse_json(const std::string& text) {
    return py::module_::import("json").attr("loads")(py::str(text));
}

// Fetch a library-owned JSON string, free it, and parse it.
template <typename Fn, typename Arg>
py::object get_json(Fn fn, Arg arg) {
    char* raw = nullptr;
    check(fn(arg, &raw));
    std::string text(raw);
    ac_string_free(raw);
    return parse_json(text);
}

// Release the GIL around one blocking C call.
template <typename Fn>
auto blocking(Fn&& fn) -> decltype(fn()) {
    py::gil_scoped_release release;
    return fn();
}

// Depth arrays stay in millimeters (same unit as the point cloud). Meters are
// depth * kDepthScaleM. The native sample does not allocate a meter copy.
constexpr double kDepthScaleM = 0.001;

// True when `calib` is a calibration dict whose depth camera matches the
// depth-map size. A missing cache, a null depth block, or a size change
// (depth alignment switched) is a miss.
bool calibration_matches(const py::object& calib, bool has_depth, uint32_t w, uint32_t h) {
    if (!calib || calib.is_none())
        return false;
    if (!has_depth)
        return true;
    if (!calib.contains("depth"))
        return false;
    py::object depth = calib["depth"];
    if (depth.is_none())
        return false;
    try {
        return depth["width"].cast<uint32_t>() == w && depth["height"].cast<uint32_t>() == h;
    } catch (const py::cast_error&) {
        return false;
    }
}

// Zero-copy numpy view of one plane. `owner` is the base object that keeps
// the frame buffer alive (the Sample). Absent frames are None.
py::object plane_view(ac_frame_t* frame, py::handle owner) {
    if (frame == nullptr)
        return py::none();
    uint32_t w = 0, h = 0;
    const void* data = nullptr;
    size_t bytes = 0;
    check(ac_frame_get_size(frame, &w, &h));
    check(ac_frame_get_data(frame, &data, &bytes));
    (void)bytes;
    const py::ssize_t H = static_cast<py::ssize_t>(h);
    const py::ssize_t W = static_cast<py::ssize_t>(w);
    const py::ssize_t f32 = static_cast<py::ssize_t>(sizeof(float));
    switch (ac_frame_get_type(frame)) {
    case AC_FRAME_DEPTH:
        return py::array_t<float>(
            py::array::ShapeContainer{H, W},
            py::array::StridesContainer{W * f32, f32},
            static_cast<const float*>(data), owner);
    case AC_FRAME_IR:
        return py::array_t<uint8_t>(
            py::array::ShapeContainer{H, W},
            py::array::StridesContainer{W, py::ssize_t{1}},
            static_cast<const uint8_t*>(data), owner);
    case AC_FRAME_RGB:
        return py::array_t<uint8_t>(
            py::array::ShapeContainer{H, W, py::ssize_t{3}},
            py::array::StridesContainer{W * 3, py::ssize_t{3}, py::ssize_t{1}},
            static_cast<const uint8_t*>(data), owner);
    case AC_FRAME_POINTS:
        return py::array_t<float>(
            py::array::ShapeContainer{W, py::ssize_t{3}},
            py::array::StridesContainer{py::ssize_t{12}, f32},
            static_cast<const float*>(data), owner);
    case AC_FRAME_NORMALS:
        return py::array_t<float>(
            py::array::ShapeContainer{H, W, py::ssize_t{3}},
            py::array::StridesContainer{W * py::ssize_t{12}, py::ssize_t{12}, f32},
            static_cast<const float*>(data), owner);
    default:
        throw AcamError("plane_view: unknown frame type");
    }
}

// ---------------------------------------------------------------- Context ---

class Context {
public:
    Context() : ctx_(ac_context_create()) {
        if (ctx_ == nullptr)
            throw AcamError(ac_last_error());
    }
    ~Context() { ac_context_destroy(ctx_); }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    ac_context_t* handle() const { return ctx_; }

    std::vector<class Device> query_devices(uint32_t timeout_ms);
    Device load_device(const std::string& path);

private:
    ac_context_t* ctx_;
};

// ----------------------------------------------------------------- Device ---

class Sensor; // fwd

class Device {
public:
    explicit Device(ac_device_t* dev) : dev_(dev) {} // adopts one reference
    ~Device() { ac_device_release(dev_); }

    Device(Device&& o) noexcept : dev_(std::exchange(o.dev_, nullptr)) {}
    Device& operator=(Device&& o) noexcept {
        if (this != &o) {
            ac_device_release(dev_);
            dev_ = std::exchange(o.dev_, nullptr);
        }
        return *this;
    }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    ac_device_t* handle() const { return dev_; }

    py::object info() const { return get_json(ac_device_get_info_json, dev_); }

    void open() { blocking([&] { return check(ac_device_open(dev_)); }); }
    void close() { check(ac_device_close(dev_)); }
    bool is_open() const { return ac_device_is_open(dev_) != 0; }

    py::object params() const { return get_json(ac_device_get_attribute_tree_json, dev_); }

    py::object intrinsics() const { return get_json(ac_device_get_intrinsics_json, dev_); }

    py::object calibration() const { return get_json(ac_device_get_calibration_json, dev_); }

    void set(const std::string& key, const py::object& value) {
        // bool must be tested before int: Python bool is a subclass of int.
        if (py::isinstance<py::bool_>(value))
            check(ac_device_set_value_bool(dev_, key.c_str(), value.cast<bool>() ? 1 : 0));
        else if (py::isinstance<py::int_>(value))
            check(ac_device_set_value_int(dev_, key.c_str(), value.cast<int32_t>()));
        else if (py::isinstance<py::float_>(value))
            check(ac_device_set_value_float(dev_, key.c_str(), value.cast<float>()));
        else
            throw py::type_error("set(): value must be bool, int or float");
    }

    void record_to_file(const std::string& path) {
        check(ac_device_record_to_file(dev_, path.c_str()));
    }
    void stop_record() { check(ac_device_stop_record(dev_)); }

    py::object get(const std::string& key) const {
        const std::string type = attribute_type(key);
        if (type == "int") {
            int32_t v = 0;
            check(ac_device_get_value_int(dev_, key.c_str(), &v));
            return py::int_(v);
        }
        if (type == "float") {
            float v = 0.0f;
            check(ac_device_get_value_float(dev_, key.c_str(), &v));
            return py::float_(v);
        }
        if (type == "bool") {
            int v = 0;
            check(ac_device_get_value_bool(dev_, key.c_str(), &v));
            return py::bool_(v != 0);
        }
        // Key not described by the attribute tree: probe the getters.
        int32_t vi = 0;
        if (ac_device_get_value_int(dev_, key.c_str(), &vi) == AC_OK)
            return py::int_(vi);
        float vf = 0.0f;
        if (ac_device_get_value_float(dev_, key.c_str(), &vf) == AC_OK)
            return py::float_(vf);
        int vb = 0;
        if (ac_device_get_value_bool(dev_, key.c_str(), &vb) == AC_OK)
            return py::bool_(vb != 0);
        throw AcamError(ac_last_error());
    }

    std::vector<Sensor> sensors(); // defined after Sensor

    Device& enter() {
        open();
        return *this;
    }
    void exit(const py::args&) { close(); }

private:
    // Look the key up in the attribute tree; "" when unknown/undescribed.
    std::string attribute_type(const std::string& key) const {
        char* raw = nullptr;
        if (ac_device_get_attribute_tree_json(dev_, &raw) != AC_OK)
            return "";
        std::string text(raw);
        ac_string_free(raw);
        py::dict tree = parse_json(text);
        if (!tree.contains("attributes"))
            return "";
        for (const py::handle& item : tree["attributes"]) {
            const py::dict attr = py::reinterpret_borrow<py::dict>(item);
            if (attr.contains("key") && attr["key"].cast<std::string>() == key) {
                if (attr.contains("type"))
                    return attr["type"].cast<std::string>();
                return "";
            }
        }
        return "";
    }

    ac_device_t* dev_;
};

// ----------------------------------------------------------------- Sensor ---

class Sensor {
public:
    // Borrowed handle; `device` (the Python object) is kept alive so the
    // borrowed sensor never outlives its device.
    Sensor(ac_sensor_t* sensor, py::object device)
        : sensor_(sensor), device_(std::move(device)) {}

    std::string name() const { return ac_sensor_get_name(sensor_); }

    uint32_t outputs() const {
        uint32_t mask = 0;
        check(ac_sensor_get_outputs(sensor_, &mask));
        return mask;
    }
    void set_outputs(uint32_t mask) { check(ac_sensor_set_outputs(sensor_, mask)); }

private:
    ac_sensor_t* sensor_;
    py::object device_; // keep-alive reference
};

std::vector<Sensor> Device::sensors() {
    std::vector<Sensor> out;
    const int n = ac_device_get_sensor_count(dev_);
    for (int i = 0; i < n; ++i) {
        ac_sensor_t* s = ac_device_get_sensor(dev_, static_cast<size_t>(i));
        if (s != nullptr)
            out.emplace_back(s, py::cast(this)); // canonical Device py::object
    }
    return out;
}

// ------------------------------------------------------------------ Frame ---

class Frame {
public:
    explicit Frame(ac_frame_t* frame) : frame_(frame) {} // adopts one reference
    ~Frame() { ac_frame_release(frame_); }

    Frame(Frame&& o) noexcept : frame_(std::exchange(o.frame_, nullptr)) {}
    Frame& operator=(Frame&& o) noexcept {
        if (this != &o) {
            ac_frame_release(frame_);
            frame_ = std::exchange(o.frame_, nullptr);
        }
        return *this;
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    int type() const { return static_cast<int>(ac_frame_get_type(frame_)); }

    uint64_t index() const {
        uint64_t idx = 0;
        double ts = 0.0;
        check(ac_frame_get_info(frame_, &idx, &ts));
        return idx;
    }
    double timestamp() const {
        uint64_t idx = 0;
        double ts = 0.0;
        check(ac_frame_get_info(frame_, &idx, &ts));
        return ts;
    }

    py::tuple size() const {
        uint32_t w = 0, h = 0;
        check(ac_frame_get_size(frame_, &w, &h));
        return py::make_tuple(w, h);
    }

    // All array accessors COPY the payload out, so the numpy array is
    // independent of the frame's (and frameset's) lifetime.
    py::array_t<float> depth() const {
        require(AC_FRAME_DEPTH, "depth()");
        uint32_t w = 0, h = 0;
        const void* data = nullptr;
        size_t bytes = 0;
        fetch(&w, &h, &data, &bytes);
        py::array_t<float> arr({static_cast<size_t>(h), static_cast<size_t>(w)});
        std::memcpy(arr.mutable_data(), data, static_cast<size_t>(w) * h * sizeof(float));
        return arr;
    }

    py::array_t<uint8_t> ir() const {
        require(AC_FRAME_IR, "ir()");
        uint32_t w = 0, h = 0;
        const void* data = nullptr;
        size_t bytes = 0;
        fetch(&w, &h, &data, &bytes);
        py::array_t<uint8_t> arr({static_cast<size_t>(h), static_cast<size_t>(w)});
        std::memcpy(arr.mutable_data(), data, static_cast<size_t>(w) * h);
        return arr;
    }

    py::array_t<uint8_t> rgb() const {
        require(AC_FRAME_RGB, "rgb()");
        uint32_t w = 0, h = 0;
        const void* data = nullptr;
        size_t bytes = 0;
        fetch(&w, &h, &data, &bytes);
        py::array_t<uint8_t> arr({static_cast<size_t>(h), static_cast<size_t>(w), static_cast<size_t>(3)});
        std::memcpy(arr.mutable_data(), data, static_cast<size_t>(w) * h * 3);
        return arr;
    }

    py::array_t<float> points() const {
        require(AC_FRAME_POINTS, "points()");
        uint32_t w = 0, h = 0;
        const void* data = nullptr;
        size_t bytes = 0;
        fetch(&w, &h, &data, &bytes); // w = point count, h = 1
        py::array_t<float> arr({static_cast<size_t>(w), static_cast<size_t>(3)});
        std::memcpy(arr.mutable_data(), data, static_cast<size_t>(w) * 12);
        return arr;
    }

    // Natural array per frame type.
    py::object data() const {
        switch (ac_frame_get_type(frame_)) {
        case AC_FRAME_DEPTH:  return depth();
        case AC_FRAME_IR:     return ir();
        case AC_FRAME_RGB:    return rgb();
        case AC_FRAME_POINTS: return points();
        case AC_FRAME_NORMALS: {
            uint32_t w = 0, h = 0;
            const void* p = nullptr;
            size_t bytes = 0;
            fetch(&w, &h, &p, &bytes); // float32 nxnynz interleaved
            py::array_t<float> arr({static_cast<size_t>(h), static_cast<size_t>(w), static_cast<size_t>(3)});
            std::memcpy(arr.mutable_data(), p, static_cast<size_t>(w) * h * 12);
            return arr;
        }
        default:
            throw AcamError("data(): unknown frame type");
        }
    }

    ac_frame_t* handle() const { return frame_; }

private:
    void require(ac_frame_type want, const char* what) const {
        if (ac_frame_get_type(frame_) != want)
            throw AcamError(std::string(what) + " requires frame type " +
                            std::to_string(static_cast<int>(want)) + ", got " +
                            std::to_string(static_cast<int>(ac_frame_get_type(frame_))));
    }

    void fetch(uint32_t* w, uint32_t* h, const void** data, size_t* bytes) const {
        check(ac_frame_get_size(frame_, w, h));
        check(ac_frame_get_data(frame_, data, bytes));
    }

    ac_frame_t* frame_;
};

// --------------------------------------------------------------- FrameSet ---

class FrameSet {
public:
    // Adopts the frameset. Every frame the set exposes is add_ref()'d up
    // front, so Frame objects and their data survive the set; the set handle
    // itself is kept alive because filters need a live frameset as input.
    explicit FrameSet(ac_frameset_t* set) : set_(set) {
        const size_t n = ac_frameset_count(set_);
        frames_.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            ac_frame_t* f = ac_frameset_get(set_, i);
            if (f != nullptr) {
                ac_frame_add_ref(f);
                frames_.push_back(f);
            }
        }
    }
    ~FrameSet() {
        for (ac_frame_t* f : frames_)
            ac_frame_release(f);
        ac_frameset_release(set_);
    }

    FrameSet(FrameSet&& o) noexcept
        : set_(std::exchange(o.set_, nullptr)), frames_(std::move(o.frames_)) {}
    FrameSet& operator=(FrameSet&& o) noexcept {
        if (this != &o) {
            for (ac_frame_t* f : frames_)
                ac_frame_release(f);
            ac_frameset_release(set_);
            set_ = std::exchange(o.set_, nullptr);
            frames_ = std::move(o.frames_);
        }
        return *this;
    }
    FrameSet(const FrameSet&) = delete;
    FrameSet& operator=(const FrameSet&) = delete;

    size_t len() const { return frames_.size(); }

    std::vector<Frame> frames() const {
        std::vector<Frame> out;
        out.reserve(frames_.size());
        for (ac_frame_t* f : frames_) {
            ac_frame_add_ref(f);
            out.emplace_back(f);
        }
        return out;
    }

    py::iterator iter() const { return py::iter(py::cast(frames())); }

    py::object find(int type) const {
        for (ac_frame_t* f : frames_) {
            if (static_cast<int>(ac_frame_get_type(f)) == type) {
                ac_frame_add_ref(f);
                return py::cast(Frame(f));
            }
        }
        return py::none();
    }

    ac_frameset_t* handle() const { return set_; }

private:
    ac_frameset_t* set_ = nullptr;
    std::vector<ac_frame_t*> frames_;
};

// ----------------------------------------------------------------- Sample ---

// One capture, packaged for a model: shared device timestamp, zero-copy
// planes, and a calibration snapshot taken for this depth-map size.
class Sample {
public:
    Sample(FrameSet frameset, py::object calibration)
        : frameset_(std::move(frameset)), calibration_(std::move(calibration)) {
        const size_t n = ac_frameset_count(frameset_.handle());
        for (size_t i = 0; i < n; ++i) {
            ac_frame_t* frame = ac_frameset_get(frameset_.handle(), i);
            uint64_t index = 0;
            double timestamp_s = 0.0;
            check(ac_frame_get_info(frame, &index, &timestamp_s));
            if (i == 0) {
                index_ = index;
                timestamp_s_ = timestamp_s;
            } else if (index != index_ || timestamp_s != timestamp_s_) {
                throw AcamError("Sample: planes do not share one capture timestamp");
            }
        }
        ac_frame_t* depth = ac_frameset_find(frameset_.handle(), AC_FRAME_DEPTH);
        if (depth == nullptr)
            return;
        uint32_t w = 0, h = 0;
        const void* data = nullptr;
        size_t bytes = 0;
        check(ac_frame_get_size(depth, &w, &h));
        check(ac_frame_get_data(depth, &data, &bytes));
        (void)bytes;
        depth_count_ = static_cast<size_t>(w) * static_cast<size_t>(h);
        const float* z = static_cast<const float*>(data);
        for (size_t i = 0; i < depth_count_; ++i) {
            if (std::isfinite(z[i]) && z[i] > 0.0f)
                ++depth_valid_;
        }
    }

    Sample(const Sample&) = delete;
    Sample& operator=(const Sample&) = delete;
    Sample(Sample&&) = default;
    Sample& operator=(Sample&&) = delete;

    uint64_t index() const { return index_; }
    double timestamp_s() const { return timestamp_s_; }
    size_t depth_valid() const { return depth_valid_; }
    size_t depth_count() const { return depth_count_; }
    py::object calibration() const { return calibration_; }

    py::object plane(ac_frame_type type, py::handle owner) const {
        return plane_view(ac_frameset_find(frameset_.handle(), type), owner);
    }

private:
    FrameSet frameset_;
    py::object calibration_;
    uint64_t index_ = 0;
    double timestamp_s_ = 0.0;
    size_t depth_valid_ = 0;
    size_t depth_count_ = 0;
};

// ----------------------------------------------------------------- Filter ---

class Filter {
public:
    explicit Filter(const std::string& name) {
        check(ac_filter_create(name.c_str(), &filter_));
    }
    ~Filter() { ac_filter_destroy(filter_); }

    Filter(const Filter&) = delete;
    Filter& operator=(const Filter&) = delete;

    void set_config(const std::string& key, const py::object& value) {
        const std::string v = py::str(value).cast<std::string>();
        check(ac_filter_set_config(filter_, key.c_str(), v.c_str()));
    }

    FrameSet process(FrameSet& in) {
        ac_frameset_t* out = nullptr;
        blocking([&] { return check(ac_filter_process(filter_, in.handle(), &out)); });
        return FrameSet(out);
    }

private:
    ac_filter_t* filter_ = nullptr;
};

// --------------------------------------------------------------- Pipeline ---

class Pipeline {
public:
    explicit Pipeline(py::object context) : context_(std::move(context)) {
        check(ac_pipeline_create(context_.cast<Context&>().handle(), &pipe_));
    }
    ~Pipeline() { ac_pipeline_destroy(pipe_); }

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void open(py::object device) {
        ac_device_t* dev = device.cast<Device&>().handle();
        blocking([&] { return check(ac_pipeline_open(pipe_, dev)); });
        device_ = std::move(device); // keep the device alive while bound
        calib_ = py::none();         // depth size may differ on the new device
    }
    void start() { check(ac_pipeline_start(pipe_)); }
    void stop() { check(ac_pipeline_stop(pipe_)); }
    void trigger() { check(ac_pipeline_trigger(pipe_)); }

    void set_auto_trigger(double hz) {
        check(ac_pipeline_set_auto_trigger(pipe_, hz));
        auto_trigger_hz_ = hz; // the C API has no getter; remember it
    }
    double auto_trigger() const { return auto_trigger_hz_; }

    FrameSet wait_for_frames(uint32_t timeout_ms) {
        ac_frameset_t* set = nullptr;
        blocking([&] { return check(ac_pipeline_wait_for_frames(pipe_, timeout_ms, &set)); });
        return FrameSet(set);
    }

    // Pop one capture and attach the calibration that matches its depth map.
    // The calibration dict is cached on the pipeline and re-read only when
    // the depth-map size changes.
    Sample wait_sample(uint32_t timeout_ms) {
        FrameSet frames = wait_for_frames(timeout_ms);
        return Sample(std::move(frames), calibration_snapshot(frames));
    }

    Pipeline& enter() { return *this; }
    void exit(const py::args&) { stop(); }

private:
    py::object calibration_snapshot(const FrameSet& frames) {
        ac_frame_t* depth = ac_frameset_find(frames.handle(), AC_FRAME_DEPTH);
        uint32_t w = 0, h = 0;
        if (depth != nullptr)
            check(ac_frame_get_size(depth, &w, &h));
        if (!calibration_matches(calib_, depth != nullptr, w, h)) {
            if (!device_ || device_.is_none())
                throw AcamError("wait_sample: pipeline has no device");
            calib_ = device_.cast<Device&>().calibration();
        }
        return py::module_::import("copy").attr("deepcopy")(calib_);
    }

    ac_pipeline_t* pipe_ = nullptr;
    py::object context_;             // context must outlive the pipeline
    py::object device_ = py::none(); // bound device (if any)
    py::object calib_ = py::none();  // last calibration(); refreshed on size change
    double auto_trigger_hz_ = 0.0;
};

std::vector<Device> Context::query_devices(uint32_t timeout_ms) {
    ac_device_list_t* list = nullptr;
    blocking([&] { return check(ac_context_query_devices(ctx_, timeout_ms, &list)); });
    std::vector<Device> out;
    const size_t n = ac_device_list_count(list);
    for (size_t i = 0; i < n; ++i) {
        ac_device_t* dev = ac_device_list_get(list, i); // NEW reference
        if (dev != nullptr)
            out.emplace_back(dev); // Device adopts it
    }
    ac_device_list_release(list);
    return out;
}

Device Context::load_device(const std::string& path) {
    ac_device_t* dev = nullptr;
    check(ac_context_load_device(ctx_, path.c_str(), &dev));
    return Device(dev);
}

} // namespace

PYBIND11_MODULE(pyainstec, m) {
    m.doc() = "Python bindings for the Ainstec 3D camera C ABI (ainstec_cam)";

    m.attr("__version__") = "0.1.0";

    // Output-plane selection bits (ac_sensor outputs mask).
    m.attr("OUTPUT_POINT3D") = py::int_(AC_OUTPUT_POINT3D);
    m.attr("OUTPUT_DEPTH")   = py::int_(AC_OUTPUT_DEPTH);
    m.attr("OUTPUT_IR")      = py::int_(AC_OUTPUT_IR);
    m.attr("OUTPUT_RGB")     = py::int_(AC_OUTPUT_RGB);
    m.attr("OUTPUT_NORMALS") = py::int_(AC_OUTPUT_NORMALS);

    // Typed frame kinds.
    m.attr("FRAME_DEPTH")   = py::int_(static_cast<int>(AC_FRAME_DEPTH));
    m.attr("FRAME_IR")      = py::int_(static_cast<int>(AC_FRAME_IR));
    m.attr("FRAME_RGB")     = py::int_(static_cast<int>(AC_FRAME_RGB));
    m.attr("FRAME_POINTS")  = py::int_(static_cast<int>(AC_FRAME_POINTS));
    m.attr("FRAME_NORMALS") = py::int_(static_cast<int>(AC_FRAME_NORMALS));

    static py::object error =
        py::register_exception<AcamError>(m, "Error", PyExc_RuntimeError);

    py::class_<Context>(m, "Context")
        .def(py::init<>())
        .def("query_devices", &Context::query_devices, py::arg("timeout_ms") = 1000,
             "Discover devices, waiting up to timeout_ms; returns a list of Device.")
        .def("load_device", &Context::load_device, py::arg("path"),
             "Load a playback Device from an .acam recording.");

    py::class_<Device>(m, "Device")
        .def_property_readonly("info", &Device::info,
                               "Identity dict: name/model/ip/serial/mac.")
        .def("open", &Device::open)
        .def("close", &Device::close)
        .def_property_readonly("is_open", &Device::is_open)
        .def_property_readonly("sensors", &Device::sensors,
                               "Sensors borrowed from this device.")
        .def("params", &Device::params, "Parsed attribute tree (keys/types/ranges).")
        .def("intrinsics", &Device::intrinsics,
             "Pinhole intrinsics dict (width/height/fx/fy/cx/cy); device must be open.")
        .def("calibration", &Device::calibration,
             "Full calibration dict (depth/rgb intrinsics + points_to_rgb 4x4); "
             "device must be open.")
        .def("set", &Device::set, py::arg("key"), py::arg("value"),
             "Set a parameter; bool/int/float dispatch on the Python value type.")
        .def("get", &Device::get, py::arg("key"),
             "Get a parameter; the getter is chosen from the attribute-tree type.")
        .def("record_to_file", &Device::record_to_file, py::arg("path"),
             "Start appending captures to an .acam file (device must be open).")
        .def("stop_record", &Device::stop_record,
             "Finalize the .acam recording (idempotent if not recording).")
        .def("__enter__", &Device::enter)
        .def("__exit__", &Device::exit);

    py::class_<Sensor>(m, "Sensor")
        .def_property_readonly("name", &Sensor::name)
        .def_property("outputs", &Sensor::outputs, &Sensor::set_outputs,
                      "Output-plane mask (OUTPUT_* bits).");

    py::class_<Frame>(m, "Frame")
        .def_property_readonly("type", &Frame::type, "FRAME_* constant.")
        .def_property_readonly("index", &Frame::index, "Per-device capture counter.")
        .def_property_readonly("timestamp", &Frame::timestamp, "Capture time in seconds.")
        .def_property_readonly("size", &Frame::size, "(width, height); POINTS: (count, 1).")
        .def("depth", &Frame::depth, "Depth map as (H, W) float32 mm (copied).")
        .def("ir", &Frame::ir, "IR image as (H, W) uint8 (copied).")
        .def("rgb", &Frame::rgb, "RGB image as (H, W, 3) uint8 (copied).")
        .def("points", &Frame::points, "Point cloud as (N, 3) float32 (copied).")
        .def("data", &Frame::data, "The natural array for this frame's type (copied).");

    py::class_<FrameSet>(m, "FrameSet")
        .def("__len__", &FrameSet::len)
        .def("__iter__", &FrameSet::iter)
        .def_property_readonly("frames", &FrameSet::frames, "All frames as a list.")
        .def("find", &FrameSet::find, py::arg("frame_type"),
             "First frame of the given FRAME_* type, or None.");

    auto plane = [](ac_frame_type type) {
        return [type](py::handle self) {
            return self.cast<const Sample&>().plane(type, self);
        };
    };
    py::class_<Sample>(m, "Sample")
        .def_property_readonly("index", &Sample::index,
                               "Per-device capture counter, shared by every plane.")
        .def_property_readonly("timestamp_s", &Sample::timestamp_s,
                               "Device capture time in seconds, shared by every plane.")
        .def_property_readonly("depth_scale_m", [](const Sample&) { return kDepthScaleM; },
                               "Meters per depth unit. depth_m = depth * depth_scale_m.")
        .def_property_readonly("depth_valid", &Sample::depth_valid,
                               "Depth pixels that are finite and greater than 0.")
        .def_property_readonly("depth_count", &Sample::depth_count,
                               "Depth pixels (H*W); 0 when the capture has no depth.")
        .def_property_readonly("calibration", &Sample::calibration,
                               "Snapshot of Device.calibration() for this depth-map size.")
        .def_property_readonly("depth", plane(AC_FRAME_DEPTH),
                               "(H, W) float32 millimeters, zero-copy, or None.")
        .def_property_readonly("ir", plane(AC_FRAME_IR),
                               "(H, W) uint8, zero-copy, or None.")
        .def_property_readonly("rgb", plane(AC_FRAME_RGB),
                               "(H, W, 3) uint8, zero-copy, or None.")
        .def_property_readonly("points", plane(AC_FRAME_POINTS),
                               "(N, 3) float32 millimeters, zero-copy, or None.")
        .def_property_readonly("normals", plane(AC_FRAME_NORMALS),
                               "(H, W, 3) float32, zero-copy, or None.");

    py::class_<Filter>(m, "Filter")
        .def(py::init<const std::string&>(), py::arg("name"))
        .def("set_config", &Filter::set_config, py::arg("key"), py::arg("value"),
             "Configure with a key/value pair (value is stringified).")
        .def("process", &Filter::process, py::arg("frameset"),
             "Process a frameset into a new frameset.");

    py::class_<Pipeline>(m, "Pipeline")
        .def(py::init<py::object>(), py::arg("context"))
        .def("open", &Pipeline::open, py::arg("device"),
             "Bind an already-open device.")
        .def("start", &Pipeline::start)
        .def("stop", &Pipeline::stop)
        .def("trigger", &Pipeline::trigger, "Enqueue one software capture.")
        .def_property("auto_trigger", &Pipeline::auto_trigger, &Pipeline::set_auto_trigger,
                      "Self-trigger rate in Hz (0 = manual).")
        .def("wait_for_frames", &Pipeline::wait_for_frames, py::arg("timeout_ms"),
             "Pop the next frameset; raises Error('timeout') on timeout.")
        .def("wait_sample", &Pipeline::wait_sample, py::arg("timeout_ms"),
             "Pop the next capture as a Sample (zero-copy planes, device timestamp, "
             "calibration). Raises Error('timeout') on timeout.")
        .def("__enter__", &Pipeline::enter)
        .def("__exit__", &Pipeline::exit);
}
