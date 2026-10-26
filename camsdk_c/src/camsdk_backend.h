// camsdk_backend.h - internal C++ backend interface shared by the real
// (vendor SDK) and mock backends. Not part of the public ABI.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ainstec_cam/ainstec_cam.h"

namespace acam {

// Error codes for the C layer and the mock backend; values mirror the vendor
// GenericError.h codes. The real backend returns genuine vendor codes.
constexpr int kErrUnknown      = 1;  // AC_E_UNKNOWN
constexpr int kErrTimeout      = 2;  // AC_E_TIMEOUT
constexpr int kErrNotFound     = 13; // AC_E_NOT_FOUND
constexpr int kErrNotOpened    = 17; // AC_E_NOT_OPENED
constexpr int kErrInvalidParam = 31; // AC_E_INVALID_PARAM
constexpr int kErrNullPtr      = 33; // AC_E_NULL_PTR
constexpr int kErrNoData       = 40; // AC_E_DATA_IS_EMPTY
constexpr int kErrNoMemory     = 72; // AC_E_NOT_ENOUGH_MEMORY

// Basic identity of one discovered camera; serialized to JSON by the C layer.
struct CameraDesc {
    std::string name;
    std::string model;
    std::string ip;
    std::string serial;
    std::string mac;
};

// One typed plane of a capture. data points into memory owned by the
// CaptureData the plane came from and stays valid while that CaptureData lives.
struct Plane {
    ac_frame_type type = AC_FRAME_DEPTH;
    uint32_t w = 0; // pixels; POINTS: point count
    uint32_t h = 0; // pixels; POINTS: 1
    const void* data = nullptr;
    size_t byte_size = 0;
};

// Result of one device capture: typed planes plus per-capture metadata.
// All frames split from one capture share one CaptureData via std::shared_ptr,
// so the underlying vendor FrameData (which frees all its planes together in
// its destructor) is released only when the last frame handle dies.
class CaptureData {
public:
    virtual ~CaptureData() = default;
    virtual const std::vector<Plane>& planes() const = 0;
    uint64_t index = 0;
    double timestamp_s = 0.0;
};

// CaptureData that owns its plane bytes itself (mock backend, filters).
// Plane buffers stay valid for life: the blocks may be moved but their heap
// storage is transferred, not reallocated.
class BufferCapture final : public CaptureData {
public:
    // Adds a zeroed plane, returns its writable buffer of byte_size bytes.
    uint8_t* add_plane(ac_frame_type type, uint32_t w, uint32_t h, size_t byte_size) {
        blocks_.emplace_back(byte_size, static_cast<uint8_t>(0));
        Plane p;
        p.type = type;
        p.w = w;
        p.h = h;
        p.byte_size = byte_size;
        p.data = blocks_.back().data();
        plane_list_.push_back(p);
        return blocks_.back().data();
    }
    const std::vector<Plane>& planes() const override { return plane_list_; }

private:
    std::vector<std::vector<uint8_t>> blocks_;
    std::vector<Plane> plane_list_;
};

// One sensor of a device (borrowed; valid while its device lives).
class BackendSensor {
public:
    virtual ~BackendSensor() = default;
    virtual const char* name() const = 0; // static string
    virtual int set_outputs(uint32_t mask, std::string& err) = 0;
    virtual int get_outputs(uint32_t& mask_out, std::string& err) = 0;
};

// One camera known to a backend. Reference-counted by the C layer through
// std::shared_ptr. All methods return AC_OK on success and fill err on failure.
class BackendDevice {
public:
    virtual ~BackendDevice() = default;
    virtual const CameraDesc& info() const = 0;
    virtual int open(std::string& err) = 0;  // idempotent
    virtual int close(std::string& err) = 0; // idempotent
    virtual bool is_open() const = 0;
    virtual int sensor_count() const = 0;
    virtual BackendSensor* sensor(size_t index) = 0; // borrowed; NULL when absent
    // One capture covering the union of all sensor output masks; every call
    // is serialized through the device mutex.
    virtual int capture(std::shared_ptr<CaptureData>& out, std::string& err) = 0;
    virtual int get_attribute_tree_json(std::string& out, std::string& err) = 0;
    // Pinhole intrinsics JSON; requires the device to be open.
    virtual int get_intrinsics_json(std::string& out, std::string& err) = 0;
    // Full calibration JSON: depth + RGB intrinsics and the points->RGB
    // 4x4 extrinsic (null fields when no RGB module). Requires open.
    virtual int get_calibration_json(std::string& out, std::string& err) = 0;
    virtual int set_value_int(const std::string& key, int32_t value, std::string& err) = 0;
    virtual int set_value_float(const std::string& key, float value, std::string& err) = 0;
    virtual int set_value_bool(const std::string& key, bool value, std::string& err) = 0;
    virtual int get_value_int(const std::string& key, int32_t& out, std::string& err) = 0;
    virtual int get_value_float(const std::string& key, float& out, std::string& err) = 0;
    virtual int get_value_bool(const std::string& key, bool& out, std::string& err) = 0;
};

// Backend session: discovery; the returned device objects own their open state.
class CamBackend {
public:
    virtual ~CamBackend() = default;
    virtual int discover(uint32_t timeout_ms,
                         std::vector<std::shared_ptr<BackendDevice>>& out,
                         std::string& err) = 0;
};

std::unique_ptr<CamBackend> create_ainstec_backend();
std::unique_ptr<CamBackend> create_mock_backend();

} // namespace acam
