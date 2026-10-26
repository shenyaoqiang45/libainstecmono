#include "record_playback.hpp"

#include "acam_file.hpp"
#include "attribute_tree.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace acam {
namespace {

void json_escape(std::string& out, const std::string& s) {
    for (const char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:   out += c; break;
        }
    }
}

std::string camera_desc_json(const CameraDesc& d) {
    std::string out = "{";
    out += "\"name\":\"";
    json_escape(out, d.name);
    out += "\",\"model\":\"";
    json_escape(out, d.model);
    out += "\",\"ip\":\"";
    json_escape(out, d.ip);
    out += "\",\"serial\":\"";
    json_escape(out, d.serial);
    out += "\",\"mac\":\"";
    json_escape(out, d.mac);
    out += "\"}";
    return out;
}

bool parse_camera_desc(const std::string& json, CameraDesc& out) {
    auto field = [&](const char* key) -> std::string {
        const std::string needle = std::string("\"") + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos)
            return {};
        pos = json.find(':', pos + needle.size());
        if (pos == std::string::npos)
            return {};
        pos = json.find('"', pos + 1);
        if (pos == std::string::npos)
            return {};
        size_t end = pos + 1;
        std::string val;
        while (end < json.size()) {
            if (json[end] == '\\' && end + 1 < json.size()) {
                val += json[end + 1];
                end += 2;
                continue;
            }
            if (json[end] == '"')
                break;
            val += json[end];
            ++end;
        }
        return val;
    };
    out.name = field("name");
    out.model = field("model");
    out.ip = field("ip");
    out.serial = field("serial");
    out.mac = field("mac");
    return !out.name.empty() || !out.ip.empty();
}

uint32_t plane_output_bit(ac_frame_type t) {
    switch (t) {
    case AC_FRAME_POINTS:  return AC_OUTPUT_POINT3D;
    case AC_FRAME_DEPTH:   return AC_OUTPUT_DEPTH;
    case AC_FRAME_IR:      return AC_OUTPUT_IR;
    case AC_FRAME_RGB:     return AC_OUTPUT_RGB;
    case AC_FRAME_NORMALS: return AC_OUTPUT_NORMALS;
    default:               return 0;
    }
}

// Snapshot current parameter values into a JSON object using the flat tree.
std::string snapshot_values_json(BackendDevice& dev, const std::string& flat_tree) {
    std::string out = "{";
    bool first = true;
    size_t pos = 0;
    for (;;) {
        pos = flat_tree.find("\"key\":\"", pos);
        if (pos == std::string::npos)
            break;
        pos += 7;
        size_t end = flat_tree.find('"', pos);
        if (end == std::string::npos)
            break;
        const std::string key = flat_tree.substr(pos, end - pos);
        pos = end + 1;
        size_t tpos = flat_tree.find("\"type\":\"", pos);
        if (tpos == std::string::npos || tpos > pos + 80)
            continue;
        tpos += 8;
        size_t tend = flat_tree.find('"', tpos);
        if (tend == std::string::npos)
            continue;
        const std::string type = flat_tree.substr(tpos, tend - tpos);
        std::string err;
        if (type == "int") {
            int32_t v = 0;
            if (dev.get_value_int(key, v, err) != AC_OK)
                continue;
            if (!first)
                out += ',';
            first = false;
            out += '"';
            json_escape(out, key);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "\":%d", static_cast<int>(v));
            out += buf;
        } else if (type == "float") {
            float v = 0.0f;
            if (dev.get_value_float(key, v, err) != AC_OK)
                continue;
            if (!first)
                out += ',';
            first = false;
            out += '"';
            json_escape(out, key);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "\":%.9g", static_cast<double>(v));
            out += buf;
        } else if (type == "bool") {
            bool v = false;
            if (dev.get_value_bool(key, v, err) != AC_OK)
                continue;
            if (!first)
                out += ',';
            first = false;
            out += '"';
            json_escape(out, key);
            out += v ? "\":true" : "\":false";
        }
    }
    out += '}';
    return out;
}

struct ParamValue {
    char type = 'i'; // i / f / b
    int32_t i = 0;
    float f = 0.0f;
    bool b = false;
};

void parse_values_map(const std::string& values_json, const std::string& attrs_json,
                      std::map<std::string, ParamValue>& out) {
    // Type hints from attributes.
    std::map<std::string, char> types;
    size_t pos = 0;
    for (;;) {
        pos = attrs_json.find("\"key\":\"", pos);
        if (pos == std::string::npos)
            break;
        pos += 7;
        size_t end = attrs_json.find('"', pos);
        if (end == std::string::npos)
            break;
        const std::string key = attrs_json.substr(pos, end - pos);
        pos = end + 1;
        size_t tpos = attrs_json.find("\"type\":\"", pos);
        if (tpos == std::string::npos || tpos > pos + 80)
            continue;
        tpos += 8;
        size_t tend = attrs_json.find('"', tpos);
        if (tend == std::string::npos)
            continue;
        const std::string type = attrs_json.substr(tpos, tend - tpos);
        if (type == "int")
            types[key] = 'i';
        else if (type == "float")
            types[key] = 'f';
        else if (type == "bool")
            types[key] = 'b';
    }

    // Scan values object: "Key": number|true|false
    pos = 0;
    while (pos < values_json.size()) {
        pos = values_json.find('"', pos);
        if (pos == std::string::npos)
            break;
        size_t kend = values_json.find('"', pos + 1);
        if (kend == std::string::npos)
            break;
        const std::string key = values_json.substr(pos + 1, kend - pos - 1);
        pos = values_json.find(':', kend + 1);
        if (pos == std::string::npos)
            break;
        ++pos;
        while (pos < values_json.size() &&
               (values_json[pos] == ' ' || values_json[pos] == '\t'))
            ++pos;
        ParamValue pv;
        const auto tit = types.find(key);
        if (tit != types.end())
            pv.type = tit->second;
        if (pos < values_json.size() && values_json.compare(pos, 4, "true") == 0) {
            pv.type = 'b';
            pv.b = true;
            pos += 4;
        } else if (pos < values_json.size() && values_json.compare(pos, 5, "false") == 0) {
            pv.type = 'b';
            pv.b = false;
            pos += 5;
        } else {
            char* endp = nullptr;
            const double n = std::strtod(values_json.c_str() + pos, &endp);
            if (endp == values_json.c_str() + pos) {
                pos = kend + 1;
                continue;
            }
            pos = static_cast<size_t>(endp - values_json.c_str());
            if (pv.type == 'f')
                pv.f = static_cast<float>(n);
            else if (pv.type == 'b')
                pv.b = n != 0.0;
            else {
                pv.type = 'i';
                pv.i = static_cast<int32_t>(n);
            }
        }
        out[key] = pv;
    }
}

class RecordingDevice final : public BackendDevice {
public:
    explicit RecordingDevice(std::shared_ptr<BackendDevice> inner) : inner_(std::move(inner)) {}

    std::shared_ptr<BackendDevice> inner() const { return inner_; }

    int start(const std::string& path, std::string& err) {
        std::lock_guard<std::mutex> lk(mu_);
        if (writer_.is_open()) {
            err = "already recording";
            return kErrInvalidParam;
        }
        if (!inner_->is_open()) {
            err = "device is not open";
            return kErrNotOpened;
        }
        AcamHeader hdr;
        hdr.info_json = camera_desc_json(inner_->info());
        std::string tree;
        const int tr = inner_->get_attribute_tree_json(tree, err);
        if (tr != AC_OK)
            return tr;
        hdr.attributes_json = flatten_attribute_tree_json(tree);
        hdr.values_json = snapshot_values_json(*inner_, hdr.attributes_json);
        const int ir = inner_->get_intrinsics_json(hdr.intrinsics_json, err);
        if (ir != AC_OK)
            return ir;
        const int cr = inner_->get_calibration_json(hdr.calibration_json, err);
        if (cr != AC_OK)
            return cr;
        return writer_.open(path, hdr, err);
    }

    int stop(std::string& err) {
        std::lock_guard<std::mutex> lk(mu_);
        return writer_.close(err);
    }

    const CameraDesc& info() const override { return inner_->info(); }
    int open(std::string& err) override { return inner_->open(err); }
    int close(std::string& err) override {
        std::string stop_err;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (writer_.is_open())
                writer_.close(stop_err);
        }
        return inner_->close(err);
    }
    bool is_open() const override { return inner_->is_open(); }
    int sensor_count() const override { return inner_->sensor_count(); }
    BackendSensor* sensor(size_t index) override { return inner_->sensor(index); }

    int capture(std::shared_ptr<CaptureData>& out, std::string& err) override {
        const int ret = inner_->capture(out, err);
        if (ret != AC_OK || !out)
            return ret;
        std::lock_guard<std::mutex> lk(mu_);
        if (writer_.is_open()) {
            std::string werr;
            const int wr = writer_.write_frameset(*out, werr);
            if (wr != AC_OK) {
                err = werr;
                return wr;
            }
        }
        return AC_OK;
    }

    int get_attribute_tree_json(std::string& out, std::string& err) override {
        return inner_->get_attribute_tree_json(out, err);
    }
    int get_intrinsics_json(std::string& out, std::string& err) override {
        return inner_->get_intrinsics_json(out, err);
    }
    int get_calibration_json(std::string& out, std::string& err) override {
        return inner_->get_calibration_json(out, err);
    }
    int set_value_int(const std::string& key, int32_t value, std::string& err) override {
        return inner_->set_value_int(key, value, err);
    }
    int set_value_float(const std::string& key, float value, std::string& err) override {
        return inner_->set_value_float(key, value, err);
    }
    int set_value_bool(const std::string& key, bool value, std::string& err) override {
        return inner_->set_value_bool(key, value, err);
    }
    int get_value_int(const std::string& key, int32_t& out, std::string& err) override {
        return inner_->get_value_int(key, out, err);
    }
    int get_value_float(const std::string& key, float& out, std::string& err) override {
        return inner_->get_value_float(key, out, err);
    }
    int get_value_bool(const std::string& key, bool& out, std::string& err) override {
        return inner_->get_value_bool(key, out, err);
    }

private:
    std::shared_ptr<BackendDevice> inner_;
    mutable std::mutex mu_;
    AcamWriter writer_;
};

class PlaybackDevice final : public BackendDevice {
public:
    explicit PlaybackDevice(std::string path) : path_(std::move(path)) {
        desc_.name = "AinstecPlayback";
        desc_.model = "ACAM-FILE";
        desc_.ip = "file";
        desc_.serial = path_;
        desc_.mac = "00:00:00:00:00:00";
    }

    const CameraDesc& info() const override { return desc_; }

    int open(std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (opened_)
            return AC_OK;
        AcamHeader hdr;
        const int ret = reader_.open(path_, hdr, err);
        if (ret != AC_OK)
            return ret;
        attributes_json_ = hdr.attributes_json;
        intrinsics_json_ = hdr.intrinsics_json;
        calibration_json_ = hdr.calibration_json; // empty in pre-calibration files
        values_.clear();
        parse_values_map(hdr.values_json, hdr.attributes_json, values_);
        CameraDesc parsed;
        if (parse_camera_desc(hdr.info_json, parsed)) {
            desc_ = parsed;
            if (desc_.model.empty())
                desc_.model = "ACAM-FILE";
        }
        opened_ = true;
        return AC_OK;
    }

    int close(std::string& /*err*/) override {
        std::lock_guard<std::mutex> lk(mu_);
        reader_.close();
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
        std::shared_ptr<BufferCapture> raw;
        const int ret = reader_.read_frameset(raw, err);
        if (ret != AC_OK)
            return ret;
        const uint32_t mask = masks_[0] | masks_[1];
        auto filtered = std::make_shared<BufferCapture>();
        filtered->index = raw->index;
        filtered->timestamp_s = raw->timestamp_s;
        for (const Plane& p : raw->planes()) {
            const uint32_t bit = plane_output_bit(p.type);
            if (bit == 0 || (mask & bit) == 0)
                continue;
            uint8_t* dst = filtered->add_plane(p.type, p.w, p.h, p.byte_size);
            if (p.byte_size > 0 && p.data != nullptr)
                std::memcpy(dst, p.data, p.byte_size);
        }
        out = std::move(filtered);
        return AC_OK;
    }

    int get_attribute_tree_json(std::string& out, std::string& /*err*/) override {
        std::lock_guard<std::mutex> lk(mu_);
        out = attributes_json_.empty() ? "{\"attributes\":[]}" : attributes_json_;
        return AC_OK;
    }

    int get_intrinsics_json(std::string& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_) {
            err = "device is not open";
            return kErrNotOpened;
        }
        out = intrinsics_json_;
        return AC_OK;
    }

    int get_calibration_json(std::string& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_) {
            err = "device is not open";
            return kErrNotOpened;
        }
        if (!calibration_json_.empty()) {
            out = calibration_json_;
            return AC_OK;
        }
        // Files written before the calibration field existed only carry the
        // depth intrinsics; expose them with null RGB/extrinsic fields.
        out = "{\"depth\":" + intrinsics_json_ + ",\"rgb\":null,\"points_to_rgb\":null}";
        return AC_OK;
    }

    int set_value_int(const std::string& key, int32_t /*value*/, std::string& err) override {
        err = "playback device is read-only: " + key;
        return kErrInvalidParam;
    }
    int set_value_float(const std::string& key, float /*value*/, std::string& err) override {
        err = "playback device is read-only: " + key;
        return kErrInvalidParam;
    }
    int set_value_bool(const std::string& key, bool /*value*/, std::string& err) override {
        err = "playback device is read-only: " + key;
        return kErrInvalidParam;
    }

    int get_value_int(const std::string& key, int32_t& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = values_.find(key);
        if (it == values_.end() || it->second.type != 'i') {
            err = "unknown int parameter: " + key;
            return kErrNotFound;
        }
        out = it->second.i;
        return AC_OK;
    }
    int get_value_float(const std::string& key, float& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = values_.find(key);
        if (it == values_.end() || it->second.type != 'f') {
            err = "unknown float parameter: " + key;
            return kErrNotFound;
        }
        out = it->second.f;
        return AC_OK;
    }
    int get_value_bool(const std::string& key, bool& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = values_.find(key);
        if (it == values_.end() || it->second.type != 'b') {
            err = "unknown bool parameter: " + key;
            return kErrNotFound;
        }
        out = it->second.b;
        return AC_OK;
    }

private:
    class Sensor final : public BackendSensor {
    public:
        Sensor(PlaybackDevice* dev, int slot, const char* name)
            : dev_(dev), slot_(slot), name_(name) {}
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
        PlaybackDevice* dev_;
        int slot_;
        const char* name_;
    };

    std::string path_;
    CameraDesc desc_;
    mutable std::mutex mu_;
    bool opened_ = false;
    AcamReader reader_;
    std::string attributes_json_;
    std::string intrinsics_json_;
    std::string calibration_json_;
    std::map<std::string, ParamValue> values_;
    uint32_t masks_[2] = {AC_OUTPUT_POINT3D | AC_OUTPUT_DEPTH | AC_OUTPUT_IR, AC_OUTPUT_RGB};
    Sensor sensor_3d_{this, 0, "3D Module"};
    Sensor sensor_rgb_{this, 1, "RGB Camera"};
};

} // namespace

int begin_recording(std::shared_ptr<BackendDevice>& device,
                    const std::string& path,
                    std::string& err) {
    if (!device) {
        err = "device is NULL";
        return kErrNullPtr;
    }
    if (std::dynamic_pointer_cast<RecordingDevice>(device)) {
        err = "already recording";
        return kErrInvalidParam;
    }
    auto rec = std::make_shared<RecordingDevice>(device);
    const int ret = rec->start(path, err);
    if (ret != AC_OK)
        return ret;
    device = std::move(rec);
    return AC_OK;
}

int end_recording(std::shared_ptr<BackendDevice>& device, std::string& err) {
    if (!device)
        return AC_OK;
    auto rec = std::dynamic_pointer_cast<RecordingDevice>(device);
    if (!rec)
        return AC_OK; // idempotent
    const int ret = rec->stop(err);
    device = rec->inner();
    return ret;
}

std::shared_ptr<BackendDevice> create_playback_device(const std::string& path,
                                                      std::string& err) {
    if (path.empty()) {
        err = "path is empty";
        return nullptr;
    }
    err.clear();
    return std::make_shared<PlaybackDevice>(path);
}

} // namespace acam
