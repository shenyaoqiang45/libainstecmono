// mock_backend.cpp - synthetic backend selected with ACAM_MOCK=1; no hardware needed.
#include "camsdk_backend.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

namespace acam {
namespace {

// All sensors share the binned 640x512 resolution of the current product
// generation (left/right mono and RGB alike).
constexpr uint32_t kIrW  = 640;
constexpr uint32_t kIrH  = 512;
constexpr uint32_t kRgbW = 640;
constexpr uint32_t kRgbH = 512;

// Small deterministic "noise" in [0, 1) mm derived from pixel coordinates.
float pseudo_noise(uint32_t x, uint32_t y) {
    uint32_t h = x * 1664525u + y * 1013904223u + 0x9E3779B9u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFu) * (1.0f / 256.0f);
}

struct MockParamDef {
    const char* key;
    char type;   // 'i' = int, 'b' = bool
    double def;  // default value returned before any set
    double mn, mx, step;
};

const MockParamDef kParams[] = {
    {"Exposure", 'i', 10, 1, 100, 1},
    {"Gain",     'i', 3,  0, 15,  1},
    {"HDR",      'b', 0,  0, 1,   1},
    {"FillHole", 'b', 1,  0, 1,   1},
    {"Smooth",   'i', 2,  0, 10,  1},
};

const MockParamDef* find_param(const std::string& key) {
    for (const auto& p : kParams)
        if (key == p.key)
            return &p;
    return nullptr;
}

// The one fake camera: two sensors, params in a map, synthetic frames.
class MockDevice final : public BackendDevice {
public:
    MockDevice() {
        desc_.name   = "AinstecMock";
        desc_.model  = "MOCK-3D";
        desc_.ip     = "10.0.0.1";
        desc_.serial = "MOCK0001";
        desc_.mac    = "00:00:00:00:00:01";
    }

    const CameraDesc& info() const override { return desc_; }

    int open(std::string& /*err*/) override {
        std::lock_guard<std::mutex> lk(mu_);
        opened_ = true; // idempotent; the mock never fails to open
        return AC_OK;
    }
    int close(std::string& /*err*/) override {
        std::lock_guard<std::mutex> lk(mu_);
        opened_ = false;
        return AC_OK;
    }
    bool is_open() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return opened_;
    }

    int sensor_count() const override { return 2; }
    BackendSensor* sensor(size_t index) override {
        if (index == 0)
            return &sensor_3d_;
        if (index == 1)
            return &sensor_rgb_;
        return nullptr;
    }

    int capture(std::shared_ptr<CaptureData>& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_) {
            err = "device is not open";
            return kErrNotOpened;
        }
        const uint32_t mask = masks_[0] | masks_[1]; // union of all sensors
        std::shared_ptr<BufferCapture> cap = std::make_shared<BufferCapture>();
        cap->index = next_index_++;
        cap->timestamp_s = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        const bool want_points = (mask & AC_OUTPUT_POINT3D) != 0;
        const bool want_depth  = (mask & AC_OUTPUT_DEPTH) != 0;
        if (want_points || want_depth) {
            const size_t n = static_cast<size_t>(kIrW) * kIrH;
            float* depth = want_depth
                ? reinterpret_cast<float*>(cap->add_plane(AC_FRAME_DEPTH, kIrW, kIrH, n * 4))
                : nullptr;
            float* points = want_points
                ? reinterpret_cast<float*>(cap->add_plane(AC_FRAME_POINTS, static_cast<uint32_t>(n), 1, n * 12))
                : nullptr;
            for (uint32_t y = 0; y < kIrH; ++y) {
                for (uint32_t x = 0; x < kIrW; ++x) {
                    const float z = 1000.0f + static_cast<float>(x + y) * 0.05f + pseudo_noise(x, y);
                    const size_t i = static_cast<size_t>(y) * kIrW + x;
                    if (depth != nullptr)
                        depth[i] = z;
                    if (points != nullptr) {
                        // Back-projection with fx=fy=500, cx=320, cy=256.
                        points[3 * i + 0] = (static_cast<float>(x) - 320.0f) * z / 500.0f;
                        points[3 * i + 1] = (static_cast<float>(y) - 256.0f) * z / 500.0f;
                        points[3 * i + 2] = z;
                    }
                }
            }
        }
        if ((mask & AC_OUTPUT_IR) != 0) {
            uint8_t* ir = cap->add_plane(AC_FRAME_IR, kIrW, kIrH, static_cast<size_t>(kIrW) * kIrH);
            for (uint32_t y = 0; y < kIrH; ++y)
                for (uint32_t x = 0; x < kIrW; ++x)
                    ir[static_cast<size_t>(y) * kIrW + x] =
                        static_cast<uint8_t>(x * 255u / (kIrW - 1)); // horizontal gradient
        }
        if ((mask & AC_OUTPUT_RGB) != 0) {
            uint8_t* rgb = cap->add_plane(AC_FRAME_RGB, kRgbW, kRgbH, static_cast<size_t>(kRgbW) * kRgbH * 3);
            for (uint32_t y = 0; y < kRgbH; ++y)
                for (uint32_t x = 0; x < kRgbW; ++x) {
                    const uint8_t v = (((x / 8u) + (y / 8u)) & 1u) ? 255 : 0; // 8px checker
                    const size_t i = (static_cast<size_t>(y) * kRgbW + x) * 3;
                    rgb[i] = rgb[i + 1] = rgb[i + 2] = v;
                }
        }
        // Note: the mock never produces a NORMALS frame.
        out = std::move(cap);
        return AC_OK;
    }

    int get_attribute_tree_json(std::string& out, std::string& /*err*/) override {
        std::lock_guard<std::mutex> lk(mu_);
        out = "{\"camera\":\"AinstecMock\",\"attributes\":[";
        for (size_t i = 0; i < sizeof(kParams) / sizeof(kParams[0]); ++i) {
            const MockParamDef& p = kParams[i];
            char buf[256];
            if (p.type == 'i')
                std::snprintf(buf, sizeof(buf),
                              "%s{\"key\":\"%s\",\"type\":\"int\",\"min\":%g,\"max\":%g,\"step\":%g,\"default\":%g}",
                              i != 0 ? "," : "", p.key, p.mn, p.mx, p.step, p.def);
            else
                std::snprintf(buf, sizeof(buf), "%s{\"key\":\"%s\",\"type\":\"bool\",\"default\":%s}",
                              i != 0 ? "," : "", p.key, p.def != 0.0 ? "true" : "false");
            out += buf;
        }
        out += "]}";
        return AC_OK;
    }

    int get_intrinsics_json(std::string& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_) {
            err = "device is not open";
            return kErrNotOpened;
        }
        // Matches the synthetic back-projection (fx=fy=500, cx=320, cy=256).
        out = "{\"width\":640,\"height\":512,\"fx\":500.0,\"fy\":500.0,\"cx\":320.0,\"cy\":256.0}";
        return AC_OK;
    }

    int get_calibration_json(std::string& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_) {
            err = "device is not open";
            return kErrNotOpened;
        }
        // Synthetic calibration: "depth" matches the back-projection above;
        // the RGB intrinsics differ on purpose (fx=fy=520) so mix-ups are
        // detectable; the extrinsic is a pure 25 mm X shift.
        out = "{\"depth\":{\"width\":640,\"height\":512,\"fx\":500.0,\"fy\":500.0,\"cx\":320.0,\"cy\":256.0},"
              "\"rgb\":{\"width\":640,\"height\":512,\"fx\":520.0,\"fy\":520.0,\"cx\":320.0,\"cy\":256.0},"
              "\"points_to_rgb\":[1,0,0,25,0,1,0,0,0,0,1,0,0,0,0,1]}";
        return AC_OK;
    }

    int set_value_int(const std::string& key, int32_t value, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        const MockParamDef* p = find_param(key);
        if (p == nullptr || p->type != 'i') {
            err = "unknown int parameter: " + key;
            return kErrNotFound;
        }
        values_[key] = static_cast<double>(value);
        return AC_OK;
    }
    int set_value_float(const std::string& key, float /*value*/, std::string& err) override {
        err = "unknown float parameter: " + key; // the mock has no float parameters
        return kErrNotFound;
    }
    int set_value_bool(const std::string& key, bool value, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        const MockParamDef* p = find_param(key);
        if (p == nullptr || p->type != 'b') {
            err = "unknown bool parameter: " + key;
            return kErrNotFound;
        }
        values_[key] = value ? 1.0 : 0.0;
        return AC_OK;
    }
    int get_value_int(const std::string& key, int32_t& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        const MockParamDef* p = find_param(key);
        if (p == nullptr || p->type != 'i') {
            err = "unknown int parameter: " + key;
            return kErrNotFound;
        }
        const auto it = values_.find(key);
        out = static_cast<int32_t>(it != values_.end() ? it->second : p->def);
        return AC_OK;
    }
    int get_value_float(const std::string& key, float& /*out*/, std::string& err) override {
        err = "unknown float parameter: " + key; // the mock has no float parameters
        return kErrNotFound;
    }
    int get_value_bool(const std::string& key, bool& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        const MockParamDef* p = find_param(key);
        if (p == nullptr || p->type != 'b') {
            err = "unknown bool parameter: " + key;
            return kErrNotFound;
        }
        const auto it = values_.find(key);
        out = (it != values_.end() ? it->second : p->def) != 0.0;
        return AC_OK;
    }

private:
    // Sensor exposed to the C layer; masks live on the device (slot 0 = 3D
    // module, slot 1 = RGB camera) and are guarded by the device mutex.
    class Sensor final : public BackendSensor {
    public:
        Sensor(MockDevice* dev, int slot, const char* name) : dev_(dev), slot_(slot), name_(name) {}
        const char* name() const override { return name_; }
        int set_outputs(uint32_t mask, std::string& /*err*/) override {
            std::lock_guard<std::mutex> lk(dev_->mu_);
            dev_->masks_[slot_] = mask;
            return AC_OK;
        }
        int get_outputs(uint32_t& mask_out, std::string& /*err*/) override {
            std::lock_guard<std::mutex> lk(dev_->mu_);
            mask_out = dev_->masks_[slot_];
            return AC_OK;
        }

    private:
        MockDevice* dev_;
        int slot_;
        const char* name_;
    };

    CameraDesc desc_;
    mutable std::mutex mu_;
    bool opened_ = false;
    uint32_t masks_[2] = {AC_OUTPUT_POINT3D | AC_OUTPUT_DEPTH | AC_OUTPUT_IR, AC_OUTPUT_RGB};
    uint64_t next_index_ = 0;
    std::map<std::string, double> values_;
    Sensor sensor_3d_{this, 0, "3D Module"};
    Sensor sensor_rgb_{this, 1, "RGB Camera"};
};

class MockBackend final : public CamBackend {
public:
    int discover(uint32_t /*timeout_ms*/,
                 std::vector<std::shared_ptr<BackendDevice>>& out,
                 std::string& /*err*/) override {
        out.assign(1, std::make_shared<MockDevice>());
        return AC_OK;
    }
};

} // namespace

std::unique_ptr<CamBackend> create_mock_backend() {
    return std::unique_ptr<CamBackend>(new MockBackend());
}

} // namespace acam
