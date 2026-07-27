// camsdk_c.cpp - extern "C" entry points implementing the public C ABI.
//
// Object model: context -> device list -> device -> sensors; captures yield
// framesets of typed reference-counted frames; filters map frameset ->
// frameset; a pipeline triggers captures on a single worker thread and queues
// the resulting framesets (capacity 2, drop-oldest).
//
// Backend selection: each context reads the ACAM_MOCK environment variable
// once at creation ("1" -> mock backend, otherwise the real vendor SDK).
#include "ainstec_cam/ainstec_cam.h"
#include "camsdk_backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

struct ac_context {
    std::unique_ptr<acam::CamBackend> backend; // selected once at creation
};

struct ac_device_list {
    std::vector<std::shared_ptr<acam::BackendDevice>> devices;
};

struct ac_sensor {
    acam::BackendSensor* impl; // borrowed, owned by the device
};

struct ac_device {
    std::shared_ptr<acam::BackendDevice> impl;
    // Borrowed sensor handles handed out via ac_device_get_sensor(); created
    // lazily, freed with this device handle.
    std::vector<std::unique_ptr<ac_sensor>> sensor_handles;
};

struct ac_frame {
    std::atomic<uint32_t> ref{1};
    std::shared_ptr<acam::CaptureData> owner; // shared by all frames of one capture
    acam::Plane plane;                        // view into the owner's memory
};

struct ac_frameset {
    std::vector<ac_frame_t*> frames; // the set owns one reference per frame
};

struct ac_filter {
    std::string name; // "decimation"
    int factor = 1;
};

struct ac_pipeline {
    ac_context_t* ctx = nullptr;    // borrowed
    ac_device_t* device = nullptr;  // owned reference; the pipeline never closes it
    std::thread worker;
    std::mutex mu;
    std::condition_variable cv_signal; // trigger signals for the worker
    std::condition_variable cv_frames; // frameset arrivals for consumers
    std::deque<ac_frameset_t*> queue;  // capacity 2, drop-oldest
    uint32_t pending = 0;              // queued manual trigger signals
    double auto_hz = 0.0;              // 0 = manual triggering only
    std::chrono::steady_clock::time_point next_fire{};
    bool running = false;
    bool quit = false;
};

namespace {

thread_local std::string g_last_error;

void clear_error() { g_last_error.clear(); }
int fail(int code, const std::string& msg) {
    g_last_error = msg;
    return code;
}

char* dup_cstr(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p != nullptr)
        std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

void json_append_escaped(std::string& out, const std::string& s) {
    for (const char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
}

std::string info_json(const acam::CameraDesc& d) {
    std::string json = "{\"name\":\"";
    json_append_escaped(json, d.name);
    json += "\",\"model\":\"";
    json_append_escaped(json, d.model);
    json += "\",\"ip\":\"";
    json_append_escaped(json, d.ip);
    json += "\",\"serial\":\"";
    json_append_escaped(json, d.serial);
    json += "\",\"mac\":\"";
    json_append_escaped(json, d.mac);
    json += "\"}";
    return json;
}

bool invalid(const ac_device_t* d) { return d == nullptr || d->impl == nullptr; }
bool invalid(const ac_sensor_t* s) { return s == nullptr || s->impl == nullptr; }
bool invalid(const ac_frame_t* f) { return f == nullptr || f->owner == nullptr; }

// Wraps one CaptureData's planes into a frameset (one frame reference each).
ac_frameset_t* make_frameset(const std::shared_ptr<acam::CaptureData>& cap) {
    std::unique_ptr<ac_frameset_t> set(new ac_frameset_t);
    for (const auto& pl : cap->planes()) {
        std::unique_ptr<ac_frame_t> f(new ac_frame_t);
        f->owner = cap;
        f->plane = pl;
        set->frames.push_back(f.release());
    }
    return set.release();
}

// Pipeline worker: waits for manual trigger signals or the auto-trigger
// deadline, performs exactly one device capture per signal, and pushes the
// resulting frameset into the bounded queue (capacity 2, drop-oldest).
void pipeline_worker(ac_pipeline_t* p) {
    using clock = std::chrono::steady_clock;
    std::unique_lock<std::mutex> lk(p->mu);
    for (;;) {
        if (p->quit)
            break;
        bool fire = false;
        if (p->pending > 0) {
            --p->pending;
            fire = true;
        } else if (p->auto_hz > 0.0 && clock::now() >= p->next_fire) {
            // Schedule the next fire; skip missed periods when falling behind.
            const auto period = std::chrono::duration_cast<clock::duration>(
                std::chrono::duration<double>(1.0 / p->auto_hz));
            p->next_fire += period;
            if (p->next_fire <= clock::now())
                p->next_fire = clock::now() + period;
            fire = true;
        }
        if (fire) {
            // Capture without holding the pipeline mutex (the device has its
            // own); a failed capture is dropped silently.
            lk.unlock();
            std::shared_ptr<acam::CaptureData> cap;
            std::string err;
            ac_frameset_t* set = nullptr;
            if (p->device->impl->capture(cap, err) == AC_OK)
                set = make_frameset(cap);
            lk.lock();
            if (set != nullptr) {
                if (p->queue.size() >= 2) {
                    ac_frameset_release(p->queue.front());
                    p->queue.pop_front();
                }
                p->queue.push_back(set);
                p->cv_frames.notify_all();
            }
            continue;
        }
        if (p->auto_hz > 0.0)
            p->cv_signal.wait_until(lk, p->next_fire);
        else
            p->cv_signal.wait(lk);
    }
}

bool pipe_has_device(const ac_pipeline_t* p) { return p != nullptr && p->device != nullptr; }

} // namespace

// Catch-all tail for every public function returning int (the C ABI must
// never let an exception escape).
#define ACAM_CATCH(name)                                                            \
    catch (const std::exception& e) {                                               \
        return fail(acam::kErrUnknown, std::string(name ": ") + e.what());          \
    } catch (...) {                                                                 \
        return fail(acam::kErrUnknown, name ": unknown exception");                 \
    }

/* ---- context ---- */

ac_context_t* ac_context_create(void) {
    clear_error();
    try {
        std::unique_ptr<ac_context_t> ctx(new ac_context_t);
        const char* m = std::getenv("ACAM_MOCK");
        if (m != nullptr && std::strcmp(m, "1") == 0)
            ctx->backend = acam::create_mock_backend();
        else
            ctx->backend = acam::create_ainstec_backend();
        return ctx.release();
    } catch (const std::exception& e) {
        fail(acam::kErrUnknown, std::string("ac_context_create: ") + e.what());
    } catch (...) {
        fail(acam::kErrUnknown, "ac_context_create: unknown exception");
    }
    return nullptr;
}

void ac_context_destroy(ac_context_t* ctx) {
    if (ctx == nullptr)
        return; // NULL-safe
    try {
        delete ctx;
    } catch (...) {
    }
}

int ac_context_query_devices(ac_context_t* ctx, uint32_t timeout_ms, ac_device_list_t** out) {
    clear_error();
    if (ctx == nullptr || ctx->backend == nullptr)
        return fail(acam::kErrNullPtr, "ac_context_query_devices: ctx is NULL");
    if (out == nullptr)
        return fail(acam::kErrNullPtr, "ac_context_query_devices: out is NULL");
    *out = nullptr;
    try {
        std::unique_ptr<ac_device_list_t> list(new ac_device_list_t);
        std::string err;
        const int ret = ctx->backend->discover(timeout_ms, list->devices, err);
        if (ret != AC_OK)
            return fail(ret, err);
        *out = list.release();
        return AC_OK;
    }
    ACAM_CATCH("ac_context_query_devices")
}

/* ---- device list / device ---- */

size_t ac_device_list_count(const ac_device_list_t* list) {
    return list != nullptr ? list->devices.size() : 0;
}

ac_device_t* ac_device_list_get(const ac_device_list_t* list, size_t index) {
    clear_error();
    if (list == nullptr) {
        fail(acam::kErrNullPtr, "ac_device_list_get: list is NULL");
        return nullptr;
    }
    if (index >= list->devices.size()) {
        fail(acam::kErrInvalidParam, "ac_device_list_get: index out of range");
        return nullptr;
    }
    try {
        return new ac_device_t{list->devices[index], {}};
    } catch (...) {
        fail(acam::kErrNoMemory, "ac_device_list_get: out of memory");
        return nullptr;
    }
}

void ac_device_list_release(ac_device_list_t* list) {
    if (list == nullptr)
        return; // NULL-safe
    try {
        delete list;
    } catch (...) {
    }
}

void ac_device_release(ac_device_t* device) {
    if (device == nullptr)
        return; // NULL-safe
    try {
        delete device;
    } catch (...) {
    }
}

int ac_device_get_info_json(const ac_device_t* device, char** out_json) {
    clear_error();
    if (invalid(device))
        return fail(acam::kErrNullPtr, "ac_device_get_info_json: device is NULL");
    if (out_json == nullptr)
        return fail(acam::kErrNullPtr, "ac_device_get_info_json: out_json is NULL");
    *out_json = nullptr;
    try {
        char* p = dup_cstr(info_json(device->impl->info()));
        if (p == nullptr)
            return fail(acam::kErrNoMemory, "ac_device_get_info_json: out of memory");
        *out_json = p;
        return AC_OK;
    }
    ACAM_CATCH("ac_device_get_info_json")
}

int ac_device_open(ac_device_t* device) {
    clear_error();
    if (invalid(device))
        return fail(acam::kErrNullPtr, "ac_device_open: device is NULL");
    try {
        std::string err;
        const int ret = device->impl->open(err);
        if (ret != AC_OK)
            return fail(ret, err);
        return AC_OK;
    }
    ACAM_CATCH("ac_device_open")
}

int ac_device_close(ac_device_t* device) {
    clear_error();
    if (invalid(device))
        return fail(acam::kErrNullPtr, "ac_device_close: device is NULL");
    try {
        std::string err;
        const int ret = device->impl->close(err);
        if (ret != AC_OK)
            return fail(ret, err);
        return AC_OK;
    }
    ACAM_CATCH("ac_device_close")
}

int ac_device_is_open(const ac_device_t* device) {
    if (invalid(device))
        return 0;
    try {
        return device->impl->is_open() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int ac_device_get_attribute_tree_json(ac_device_t* device, char** out_json) {
    clear_error();
    if (invalid(device))
        return fail(acam::kErrNullPtr, "ac_device_get_attribute_tree_json: device is NULL");
    if (out_json == nullptr)
        return fail(acam::kErrNullPtr, "ac_device_get_attribute_tree_json: out_json is NULL");
    *out_json = nullptr;
    try {
        std::string tree, err;
        const int ret = device->impl->get_attribute_tree_json(tree, err);
        if (ret != AC_OK)
            return fail(ret, err);
        char* p = dup_cstr(tree);
        if (p == nullptr)
            return fail(acam::kErrNoMemory, "ac_device_get_attribute_tree_json: out of memory");
        *out_json = p;
        return AC_OK;
    }
    ACAM_CATCH("ac_device_get_attribute_tree_json")
}

// Shared body for the three parameter setters (int value converts to bool
// implicitly for ac_device_set_value_bool).
#define AC_DEVICE_VALUE_FN(c_name, cpp_name, c_type)                          \
    int c_name(ac_device_t* device, const char* key, c_type value) {          \
        clear_error();                                                        \
        if (invalid(device))                                                  \
            return fail(acam::kErrNullPtr, #c_name ": device is NULL");       \
        if (key == nullptr)                                                   \
            return fail(acam::kErrNullPtr, #c_name ": key is NULL");          \
        try {                                                                 \
            std::string err;                                                  \
            const int ret = device->impl->cpp_name(key, value, err);          \
            if (ret != AC_OK)                                                 \
                return fail(ret, err);                                        \
            return AC_OK;                                                     \
        }                                                                     \
        ACAM_CATCH(#c_name)                                                   \
    }

AC_DEVICE_VALUE_FN(ac_device_set_value_int, set_value_int, int32_t)
AC_DEVICE_VALUE_FN(ac_device_set_value_float, set_value_float, float)
AC_DEVICE_VALUE_FN(ac_device_set_value_bool, set_value_bool, int)

int ac_device_get_value_int(ac_device_t* device, const char* key, int32_t* out) {
    clear_error();
    if (invalid(device))
        return fail(acam::kErrNullPtr, "ac_device_get_value_int: device is NULL");
    if (key == nullptr || out == nullptr)
        return fail(acam::kErrNullPtr, "ac_device_get_value_int: key or out is NULL");
    try {
        std::string err;
        const int ret = device->impl->get_value_int(key, *out, err);
        if (ret != AC_OK)
            return fail(ret, err);
        return AC_OK;
    }
    ACAM_CATCH("ac_device_get_value_int")
}

int ac_device_get_value_float(ac_device_t* device, const char* key, float* out) {
    clear_error();
    if (invalid(device))
        return fail(acam::kErrNullPtr, "ac_device_get_value_float: device is NULL");
    if (key == nullptr || out == nullptr)
        return fail(acam::kErrNullPtr, "ac_device_get_value_float: key or out is NULL");
    try {
        std::string err;
        const int ret = device->impl->get_value_float(key, *out, err);
        if (ret != AC_OK)
            return fail(ret, err);
        return AC_OK;
    }
    ACAM_CATCH("ac_device_get_value_float")
}

int ac_device_get_value_bool(ac_device_t* device, const char* key, int* out) {
    clear_error();
    if (invalid(device))
        return fail(acam::kErrNullPtr, "ac_device_get_value_bool: device is NULL");
    if (key == nullptr || out == nullptr)
        return fail(acam::kErrNullPtr, "ac_device_get_value_bool: key or out is NULL");
    try {
        bool v = false;
        std::string err;
        const int ret = device->impl->get_value_bool(key, v, err);
        if (ret != AC_OK)
            return fail(ret, err);
        *out = v ? 1 : 0;
        return AC_OK;
    }
    ACAM_CATCH("ac_device_get_value_bool")
}

int ac_device_get_intrinsics_json(ac_device_t* device, char** out_json) {
    clear_error();
    if (invalid(device))
        return fail(acam::kErrNullPtr, "ac_device_get_intrinsics_json: device is NULL");
    if (out_json == nullptr)
        return fail(acam::kErrNullPtr, "ac_device_get_intrinsics_json: out_json is NULL");
    *out_json = nullptr;
    try {
        std::string json, err;
        const int ret = device->impl->get_intrinsics_json(json, err);
        if (ret != AC_OK)
            return fail(ret, err);
        char* p = dup_cstr(json);
        if (p == nullptr)
            return fail(acam::kErrNoMemory, "ac_device_get_intrinsics_json: out of memory");
        *out_json = p;
        return AC_OK;
    }
    ACAM_CATCH("ac_device_get_intrinsics_json")
}

/* ---- sensors ---- */

int ac_device_get_sensor_count(const ac_device_t* device) {
    if (invalid(device))
        return 0;
    try {
        return device->impl->sensor_count();
    } catch (...) {
        return 0;
    }
}

ac_sensor_t* ac_device_get_sensor(const ac_device_t* device, size_t index) {
    clear_error();
    if (invalid(device)) {
        fail(acam::kErrNullPtr, "ac_device_get_sensor: device is NULL");
        return nullptr;
    }
    try {
        acam::BackendSensor* impl = device->impl->sensor(index);
        if (impl == nullptr) {
            fail(acam::kErrInvalidParam, "ac_device_get_sensor: index out of range");
            return nullptr;
        }
        ac_device_t* dev = const_cast<ac_device_t*>(device);
        if (index >= dev->sensor_handles.size())
            dev->sensor_handles.resize(index + 1);
        if (dev->sensor_handles[index] == nullptr)
            dev->sensor_handles[index].reset(new ac_sensor_t{impl});
        return dev->sensor_handles[index].get();
    } catch (const std::exception& e) {
        fail(acam::kErrUnknown, std::string("ac_device_get_sensor: ") + e.what());
    } catch (...) {
        fail(acam::kErrUnknown, "ac_device_get_sensor: unknown exception");
    }
    return nullptr;
}

const char* ac_sensor_get_name(const ac_sensor_t* sensor) {
    if (invalid(sensor))
        return nullptr;
    return sensor->impl->name(); // static string
}

int ac_sensor_set_outputs(ac_sensor_t* sensor, uint32_t mask) {
    clear_error();
    if (invalid(sensor))
        return fail(acam::kErrNullPtr, "ac_sensor_set_outputs: sensor is NULL");
    try {
        std::string err;
        const int ret = sensor->impl->set_outputs(mask, err);
        if (ret != AC_OK)
            return fail(ret, err);
        return AC_OK;
    }
    ACAM_CATCH("ac_sensor_set_outputs")
}

int ac_sensor_get_outputs(const ac_sensor_t* sensor, uint32_t* mask_out) {
    clear_error();
    if (invalid(sensor))
        return fail(acam::kErrNullPtr, "ac_sensor_get_outputs: sensor is NULL");
    if (mask_out == nullptr)
        return fail(acam::kErrNullPtr, "ac_sensor_get_outputs: mask_out is NULL");
    try {
        std::string err;
        const int ret = sensor->impl->get_outputs(*mask_out, err);
        if (ret != AC_OK)
            return fail(ret, err);
        return AC_OK;
    }
    ACAM_CATCH("ac_sensor_get_outputs")
}

/* ---- frames ---- */

ac_frame_type ac_frame_get_type(const ac_frame_t* frame) {
    if (invalid(frame))
        return static_cast<ac_frame_type>(0);
    return frame->plane.type;
}

int ac_frame_get_info(const ac_frame_t* frame, uint64_t* index, double* timestamp_s) {
    clear_error();
    if (invalid(frame))
        return fail(acam::kErrNullPtr, "ac_frame_get_info: frame is NULL");
    if (index == nullptr || timestamp_s == nullptr)
        return fail(acam::kErrNullPtr, "ac_frame_get_info: out parameter is NULL");
    *index = frame->owner->index;
    *timestamp_s = frame->owner->timestamp_s;
    return AC_OK;
}

int ac_frame_get_size(const ac_frame_t* frame, uint32_t* w, uint32_t* h) {
    clear_error();
    if (invalid(frame))
        return fail(acam::kErrNullPtr, "ac_frame_get_size: frame is NULL");
    if (w == nullptr || h == nullptr)
        return fail(acam::kErrNullPtr, "ac_frame_get_size: out parameter is NULL");
    *w = frame->plane.w;
    *h = frame->plane.h;
    return AC_OK;
}

int ac_frame_get_data(const ac_frame_t* frame, const void** data, size_t* byte_size) {
    clear_error();
    if (invalid(frame))
        return fail(acam::kErrNullPtr, "ac_frame_get_data: frame is NULL");
    if (data == nullptr || byte_size == nullptr)
        return fail(acam::kErrNullPtr, "ac_frame_get_data: out parameter is NULL");
    *data = frame->plane.data;
    *byte_size = frame->plane.byte_size;
    return AC_OK;
}

void ac_frame_add_ref(ac_frame_t* frame) {
    if (frame == nullptr)
        return;
    frame->ref.fetch_add(1, std::memory_order_relaxed);
}

void ac_frame_release(ac_frame_t* frame) {
    if (frame == nullptr)
        return; // NULL-safe
    if (frame->ref.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        try {
            delete frame;
        } catch (...) {
        }
    }
}

/* ---- framesets ---- */

size_t ac_frameset_count(const ac_frameset_t* frameset) {
    return frameset != nullptr ? frameset->frames.size() : 0;
}

ac_frame_t* ac_frameset_get(const ac_frameset_t* frameset, size_t index) {
    if (frameset == nullptr || index >= frameset->frames.size())
        return nullptr; // borrowed; NULL when out of range
    return frameset->frames[index];
}

ac_frame_t* ac_frameset_find(const ac_frameset_t* frameset, ac_frame_type type) {
    if (frameset == nullptr)
        return nullptr;
    for (ac_frame_t* f : frameset->frames)
        if (f->plane.type == type)
            return f; // borrowed
    return nullptr;
}

void ac_frameset_release(ac_frameset_t* frameset) {
    if (frameset == nullptr)
        return; // NULL-safe
    for (ac_frame_t* f : frameset->frames)
        ac_frame_release(f);
    try {
        delete frameset;
    } catch (...) {
    }
}

/* ---- filters ---- */

int ac_filter_create(const char* name, ac_filter_t** out) {
    clear_error();
    if (name == nullptr)
        return fail(acam::kErrNullPtr, "ac_filter_create: name is NULL");
    if (out == nullptr)
        return fail(acam::kErrNullPtr, "ac_filter_create: out is NULL");
    *out = nullptr;
    try {
        if (std::strcmp(name, "decimation") != 0)
            return fail(acam::kErrNotFound, std::string("ac_filter_create: unknown filter '") + name + "'");
        *out = new ac_filter_t{name, 1};
        return AC_OK;
    } catch (const std::bad_alloc&) {
        return fail(acam::kErrNoMemory, "ac_filter_create: out of memory");
    }
    ACAM_CATCH("ac_filter_create")
}

int ac_filter_set_config(ac_filter_t* filter, const char* key, const char* value) {
    clear_error();
    if (filter == nullptr)
        return fail(acam::kErrNullPtr, "ac_filter_set_config: filter is NULL");
    if (key == nullptr || value == nullptr)
        return fail(acam::kErrNullPtr, "ac_filter_set_config: key or value is NULL");
    try {
        if (std::strcmp(key, "factor") != 0)
            return fail(acam::kErrInvalidParam, std::string("ac_filter_set_config: unknown key '") + key + "'");
        char* end = nullptr;
        const long factor = std::strtol(value, &end, 10);
        if (end == value || *end != '\0' || factor < 1)
            return fail(acam::kErrInvalidParam,
                        std::string("ac_filter_set_config: factor must be an integer >= 1, got '") + value + "'");
        filter->factor = static_cast<int>(factor);
        return AC_OK;
    }
    ACAM_CATCH("ac_filter_set_config")
}

int ac_filter_process(ac_filter_t* filter, const ac_frameset_t* in, ac_frameset_t** out) {
    clear_error();
    if (filter == nullptr)
        return fail(acam::kErrNullPtr, "ac_filter_process: filter is NULL");
    if (in == nullptr)
        return fail(acam::kErrNullPtr, "ac_filter_process: in is NULL");
    if (out == nullptr)
        return fail(acam::kErrNullPtr, "ac_filter_process: out is NULL");
    *out = nullptr;
    try {
        std::unique_ptr<ac_frameset_t> result(new ac_frameset_t);
        // All frames produced by the filter share one owning BufferCapture.
        std::shared_ptr<acam::BufferCapture> cap;
        const int factor = filter->factor;
        for (ac_frame_t* f : in->frames) {
            const acam::Plane& pl = f->plane;
            const bool handled = factor > 1 &&
                (pl.type == AC_FRAME_POINTS || pl.type == AC_FRAME_DEPTH ||
                 pl.type == AC_FRAME_IR || pl.type == AC_FRAME_RGB);
            if (!handled) {
                // Framework contract: unhandled frames pass through (add_ref'd).
                ac_frame_add_ref(f);
                result->frames.push_back(f);
                continue;
            }
            if (cap == nullptr) {
                cap = std::make_shared<acam::BufferCapture>();
                cap->index = f->owner->index;
                cap->timestamp_s = f->owner->timestamp_s;
            }
            if (pl.type == AC_FRAME_POINTS) {
                const size_t n = pl.w; // POINTS: w = point count
                const size_t out_n = n / static_cast<size_t>(factor);
                float* dst = reinterpret_cast<float*>(
                    cap->add_plane(AC_FRAME_POINTS, static_cast<uint32_t>(out_n), 1, out_n * 12));
                const float* src = static_cast<const float*>(pl.data);
                for (size_t i = 0; i < out_n; ++i) {
                    dst[3 * i + 0] = src[3 * (i * static_cast<size_t>(factor)) + 0];
                    dst[3 * i + 1] = src[3 * (i * static_cast<size_t>(factor)) + 1];
                    dst[3 * i + 2] = src[3 * (i * static_cast<size_t>(factor)) + 2];
                }
            } else {
                // Stride-subsample DEPTH/IR/RGB to (w/factor, h/factor).
                const size_t bpp = pl.type == AC_FRAME_DEPTH ? 4 : pl.type == AC_FRAME_IR ? 1 : 3;
                uint32_t ow = pl.w / static_cast<uint32_t>(factor);
                uint32_t oh = pl.h / static_cast<uint32_t>(factor);
                if (ow == 0) ow = 1; // never produce an empty image
                if (oh == 0) oh = 1;
                uint8_t* dst = cap->add_plane(pl.type, ow, oh, static_cast<size_t>(ow) * oh * bpp);
                const uint8_t* src = static_cast<const uint8_t*>(pl.data);
                const size_t src_row = static_cast<size_t>(pl.w) * bpp;
                const size_t dst_row = static_cast<size_t>(ow) * bpp;
                for (uint32_t y = 0; y < oh; ++y) {
                    const uint8_t* srow = src + static_cast<size_t>(y) * factor * src_row;
                    uint8_t* drow = dst + static_cast<size_t>(y) * dst_row;
                    for (uint32_t x = 0; x < ow; ++x)
                        std::memcpy(drow + static_cast<size_t>(x) * bpp,
                                    srow + static_cast<size_t>(x) * factor * bpp, bpp);
                }
            }
        }
        if (cap != nullptr) {
            for (const auto& pl : cap->planes()) {
                std::unique_ptr<ac_frame_t> f(new ac_frame_t);
                f->owner = cap;
                f->plane = pl;
                result->frames.push_back(f.release());
            }
        }
        *out = result.release();
        return AC_OK;
    } catch (const std::bad_alloc&) {
        return fail(acam::kErrNoMemory, "ac_filter_process: out of memory");
    }
    ACAM_CATCH("ac_filter_process")
}

void ac_filter_destroy(ac_filter_t* filter) {
    if (filter == nullptr)
        return; // NULL-safe
    try {
        delete filter;
    } catch (...) {
    }
}

/* ---- pipeline ---- */

int ac_pipeline_create(ac_context_t* ctx, ac_pipeline_t** out) {
    clear_error();
    if (ctx == nullptr)
        return fail(acam::kErrNullPtr, "ac_pipeline_create: ctx is NULL");
    if (out == nullptr)
        return fail(acam::kErrNullPtr, "ac_pipeline_create: out is NULL");
    *out = nullptr;
    try {
        std::unique_ptr<ac_pipeline_t> p(new ac_pipeline_t);
        p->ctx = ctx;
        *out = p.release();
        return AC_OK;
    } catch (const std::bad_alloc&) {
        return fail(acam::kErrNoMemory, "ac_pipeline_create: out of memory");
    }
    ACAM_CATCH("ac_pipeline_create")
}

int ac_pipeline_open(ac_pipeline_t* pipeline, ac_device_t* device) {
    clear_error();
    if (pipeline == nullptr)
        return fail(acam::kErrNullPtr, "ac_pipeline_open: pipeline is NULL");
    if (invalid(device))
        return fail(acam::kErrNullPtr, "ac_pipeline_open: device is NULL");
    try {
        std::lock_guard<std::mutex> lk(pipeline->mu);
        if (pipeline->running)
            return fail(acam::kErrUnknown, "ac_pipeline_open: pipeline already started");
        if (!device->impl->is_open())
            return fail(acam::kErrNotOpened, "ac_pipeline_open: device is not open");
        // The pipeline keeps its own device reference (so the device object
        // stays alive) but never closes it; lifecycle stays with the caller.
        delete pipeline->device;
        pipeline->device = new ac_device_t{device->impl, {}};
        return AC_OK;
    } catch (const std::bad_alloc&) {
        return fail(acam::kErrNoMemory, "ac_pipeline_open: out of memory");
    }
    ACAM_CATCH("ac_pipeline_open")
}

int ac_pipeline_start(ac_pipeline_t* pipeline) {
    clear_error();
    if (pipeline == nullptr)
        return fail(acam::kErrNullPtr, "ac_pipeline_start: pipeline is NULL");
    try {
        std::lock_guard<std::mutex> lk(pipeline->mu);
        if (!pipe_has_device(pipeline))
            return fail(acam::kErrNotOpened, "ac_pipeline_start: pipeline is not open");
        if (pipeline->running)
            return AC_OK; // idempotent
        pipeline->quit = false;
        pipeline->worker = std::thread(pipeline_worker, pipeline);
        pipeline->running = true;
        return AC_OK;
    }
    ACAM_CATCH("ac_pipeline_start")
}

int ac_pipeline_trigger(ac_pipeline_t* pipeline) {
    clear_error();
    if (pipeline == nullptr)
        return fail(acam::kErrNullPtr, "ac_pipeline_trigger: pipeline is NULL");
    try {
        std::lock_guard<std::mutex> lk(pipeline->mu);
        if (!pipe_has_device(pipeline))
            return fail(acam::kErrNotOpened, "ac_pipeline_trigger: pipeline is not open");
        ++pipeline->pending; // one capture per signal; returns immediately
        pipeline->cv_signal.notify_all();
        return AC_OK;
    }
    ACAM_CATCH("ac_pipeline_trigger")
}

int ac_pipeline_set_auto_trigger(ac_pipeline_t* pipeline, double hz) {
    clear_error();
    if (pipeline == nullptr)
        return fail(acam::kErrNullPtr, "ac_pipeline_set_auto_trigger: pipeline is NULL");
    if (hz < 0.0)
        return fail(acam::kErrInvalidParam, "ac_pipeline_set_auto_trigger: hz must be >= 0");
    try {
        std::lock_guard<std::mutex> lk(pipeline->mu);
        if (!pipe_has_device(pipeline))
            return fail(acam::kErrNotOpened, "ac_pipeline_set_auto_trigger: pipeline is not open");
        pipeline->auto_hz = hz;
        if (hz > 0.0) {
            pipeline->next_fire = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / hz));
        }
        pipeline->cv_signal.notify_all();
        return AC_OK;
    }
    ACAM_CATCH("ac_pipeline_set_auto_trigger")
}

int ac_pipeline_wait_for_frames(ac_pipeline_t* pipeline, uint32_t timeout_ms, ac_frameset_t** out) {
    clear_error();
    if (pipeline == nullptr)
        return fail(acam::kErrNullPtr, "ac_pipeline_wait_for_frames: pipeline is NULL");
    if (out == nullptr)
        return fail(acam::kErrNullPtr, "ac_pipeline_wait_for_frames: out is NULL");
    *out = nullptr;
    try {
        std::unique_lock<std::mutex> lk(pipeline->mu);
        if (!pipe_has_device(pipeline))
            return fail(acam::kErrNotOpened, "ac_pipeline_wait_for_frames: pipeline is not open");
        const bool got = pipeline->cv_frames.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                                      [pipeline] { return !pipeline->queue.empty(); });
        if (!got)
            return fail(acam::kErrTimeout, "timeout waiting for frames");
        *out = pipeline->queue.front();
        pipeline->queue.pop_front();
        return AC_OK;
    }
    ACAM_CATCH("ac_pipeline_wait_for_frames")
}

int ac_pipeline_stop(ac_pipeline_t* pipeline) {
    clear_error();
    if (pipeline == nullptr)
        return fail(acam::kErrNullPtr, "ac_pipeline_stop: pipeline is NULL");
    try {
        {
            std::lock_guard<std::mutex> lk(pipeline->mu);
            if (!pipeline->running)
                return AC_OK; // idempotent
            pipeline->quit = true;
            pipeline->pending = 0;
            pipeline->cv_signal.notify_all();
        }
        if (pipeline->worker.joinable())
            pipeline->worker.join();
        pipeline->running = false;
        return AC_OK;
    }
    ACAM_CATCH("ac_pipeline_stop")
}

void ac_pipeline_destroy(ac_pipeline_t* pipeline) {
    if (pipeline == nullptr)
        return; // NULL-safe
    try {
        ac_pipeline_stop(pipeline);
        delete pipeline->device; // drops the pipeline's reference; never closes
        for (ac_frameset_t* set : pipeline->queue)
            ac_frameset_release(set);
        delete pipeline;
    } catch (...) {
    }
}

/* ---- misc ---- */

void ac_string_free(char* s) {
    std::free(s); // free(NULL) is a no-op
}

const char* ac_last_error(void) {
    return g_last_error.c_str(); // never NULL
}
