// ainstec_backend.cpp - real backend on top of the vendor gen3d::Camera SDK.
#include <Camera.h> // vendor header first: defines AC_OK (guarded in the public header)

#include "camsdk_backend.h"

#include <cstdio>
#include <exception>
#include <mutex>
#include <utility>

namespace acam {
namespace {

std::string sdk_msg(const std::string& what, int ret) {
    char buf[320];
    std::snprintf(buf, sizeof(buf), "%s failed, ret=%d", what.c_str(), ret);
    return buf;
}

// One capture: owns the gen3d::FrameData and splits it into typed planes.
class AinstecCapture final : public CaptureData {
public:
    explicit AinstecCapture(const gen3d::CameraParam& param) : param_(param) {}

    gen3d::FrameData data; // filled in place by Camera::Capture()

    // Splits the captured FrameData into typed planes for the requested mask;
    // planes the camera did not deliver are skipped.
    void build(uint32_t mask) {
        if ((mask & AC_OUTPUT_POINT3D) != 0 && data.point3D != nullptr && data.point3DSize > 0) {
            const size_t n = data.pointCount != 0 ? data.pointCount : data.point3DSize / 3;
            add(AC_FRAME_POINTS, static_cast<uint32_t>(n), 1, data.point3D, n * 12);
        }
        if ((mask & AC_OUTPUT_DEPTH) != 0 && data.depthmap != nullptr && data.depthmapSize > 0) {
            // Depth map dimensions depend on depthType: aligned to the IR
            // camera when DEPTH_MAP_ALIGN_CAMERA, else to the RGB texture.
            const uint32_t w = param_.depthType == DEPTH_MAP_ALIGN_CAMERA ? ir_w() : tex_w();
            const uint32_t h = param_.depthType == DEPTH_MAP_ALIGN_CAMERA ? ir_h() : tex_h();
            add(AC_FRAME_DEPTH, w, h, data.depthmap, static_cast<size_t>(w) * h * 4);
        }
        if ((mask & AC_OUTPUT_IR) != 0 && data.remapTexture != nullptr && data.remapTextureSize > 0) {
            // remapTexture holds 2 planes per side (bright then normal; left
            // side first, right side too for binocular), each irWidth*irHeight
            // bytes (8-bit mono by default). Plane 0 is the left bright image.
            add(AC_FRAME_IR, ir_w(), ir_h(), data.remapTexture, static_cast<size_t>(ir_w()) * ir_h());
        }
        if ((mask & AC_OUTPUT_RGB) != 0 && data.texture != nullptr && data.textureSize > 0) {
            add(AC_FRAME_RGB, tex_w(), tex_h(), data.texture, static_cast<size_t>(tex_w()) * tex_h() * 3);
        }
        if ((mask & AC_OUTPUT_NORMALS) != 0 && data.normals != nullptr && data.normalsSize > 0) {
            add(AC_FRAME_NORMALS, ir_w(), ir_h(), data.normals, static_cast<size_t>(ir_w()) * ir_h() * 12);
        }
    }

    const std::vector<Plane>& planes() const override { return plane_list_; }

private:
    uint32_t ir_w() const { return static_cast<uint32_t>(param_.irWidth); }
    uint32_t ir_h() const { return static_cast<uint32_t>(param_.irHeight); }
    uint32_t tex_w() const { return static_cast<uint32_t>(param_.textureWidth); }
    uint32_t tex_h() const { return static_cast<uint32_t>(param_.textureHeight); }
    void add(ac_frame_type type, uint32_t w, uint32_t h, const void* ptr, size_t bytes) {
        Plane p;
        p.type = type;
        p.w = w;
        p.h = h;
        p.data = ptr;
        p.byte_size = bytes;
        plane_list_.push_back(p);
    }

    gen3d::CameraParam param_;
    std::vector<Plane> plane_list_;
};

// One camera. mu_ serializes every vendor SDK call made through this device.
class AinstecDevice final : public BackendDevice {
public:
    AinstecDevice(gen3d::Camera* sdk, gen3d::CameraInfo info)
        : sdk_(sdk), info_(std::move(info)) {
        desc_.name   = info_.cameraName;
        desc_.model  = !info_.cameraModel.empty() ? info_.cameraModel : info_.model;
        desc_.ip     = info_.cameraIP;
        desc_.serial = info_.serialNum;
        desc_.mac    = info_.macAddr;
    }

    ~AinstecDevice() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (opened_) {
            try { sdk_->Close(info_); } catch (...) {}
        }
    }

    const CameraDesc& info() const override { return desc_; }

    int open(std::string& err) override {
        // Configure software-trigger work mode exactly like the vendor Capture
        // sample: SetValue "TriggerMode" = Camera_SoftTrigger right after Open.
        return sdk_call("Open", [this] {
            if (opened_)
                return AC_OK; // idempotent
            int ret = sdk_->Open(info_);
            if (ret == AC_OK) {
                ret = sdk_->SetValue(info_, "TriggerMode", static_cast<int>(gen3d::Camera_SoftTrigger));
                if (ret != AC_OK)
                    sdk_->Close(info_);
            }
            return ret;
        }, err, [this] { opened_ = true; });
    }

    int close(std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_)
            return AC_OK; // idempotent
        try {
            sdk_->Close(info_);
            opened_ = false;
            return AC_OK;
        } catch (const std::exception& e) {
            err = std::string("Close threw: ") + e.what();
        } catch (...) {
            err = "Close threw an unknown exception";
        }
        return kErrUnknown;
    }

    bool is_open() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return opened_;
    }

    // Sensor 0 is always the 3D module; the RGB camera exists only when the
    // device reports a working RGB module (rgbStatus == 0, known after Open).
    int sensor_count() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return (opened_ && info_.rgbStatus == 0) ? 2 : 1;
    }
    BackendSensor* sensor(size_t index) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (index == 0)
            return &sensor_3d_;
        if (index == 1 && opened_ && info_.rgbStatus == 0)
            return &sensor_rgb_;
        return nullptr;
    }

    int capture(std::shared_ptr<CaptureData>& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_) {
            err = "device is not open";
            return kErrNotOpened;
        }
        try {
            const uint32_t mask = masks_[0] | masks_[1]; // union of all sensors
            gen3d::OutputSettings os = {};               // everything false by default
            os.sendPoint3D      = (mask & AC_OUTPUT_POINT3D) != 0;
            os.sendDepthmap     = (mask & AC_OUTPUT_DEPTH) != 0;
            os.sendRemapTexture = (mask & AC_OUTPUT_IR) != 0;
            os.sendTexture      = (mask & AC_OUTPUT_RGB) != 0 && info_.rgbStatus == 0; // no-RGB guard
            os.sendNormals      = (mask & AC_OUTPUT_NORMALS) != 0;
            info_.outputSettings = os;
            std::shared_ptr<AinstecCapture> cap = std::make_shared<AinstecCapture>(info_.camParam);
            const int ret = sdk_->Capture(info_, cap->data);
            if (ret != AC_OK) {
                err = sdk_msg("Capture", ret);
                return ret;
            }
            cap->index = cap->data.frameInfo.frameIndex;
            cap->timestamp_s = cap->data.frameInfo.frameTimestamp;
            cap->build(mask);
            out = std::move(cap);
            return AC_OK;
        } catch (const std::exception& e) {
            err = std::string("Capture threw: ") + e.what();
        } catch (...) {
            err = "Capture threw an unknown exception";
        }
        return kErrUnknown;
    }

    int get_attribute_tree_json(std::string& out, std::string& err) override {
        return sdk_call("GetAttributeTreeJson",
                        [this, &out] { return sdk_->GetAttributeTreeJson(info_, out); }, err);
    }
    int get_intrinsics_json(std::string& out, std::string& err) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_) {
            err = "device is not open";
            return kErrNotOpened;
        }
        try {
            // Like the CalibrationParams sample, pick the camera position by
            // depthType: the depth map lives in the RGB frame when aligned to
            // RGB, otherwise in the left IR camera frame. W/H follow the same
            // rule so the intrinsics match the depth frames we hand out.
            const gen3d::CAMERA_POSITION pos =
                info_.camParam.depthType == DEPTH_MAP_ALIGN_RGB ? gen3d::CAM_RGB : gen3d::CAM_LEFT;
            const int w = info_.camParam.depthType == DEPTH_MAP_ALIGN_RGB
                ? info_.camParam.textureWidth : info_.camParam.irWidth;
            const int h = info_.camParam.depthType == DEPTH_MAP_ALIGN_RGB
                ? info_.camParam.textureHeight : info_.camParam.irHeight;
            std::vector<double> k; // row-major 3x3: k[0]=fx, k[2]=cx, k[4]=fy, k[5]=cy
            const int ret = sdk_->Get3DCameraIntrinsic(info_, pos, k);
            if (ret != AC_OK) {
                err = sdk_msg("Get3DCameraIntrinsic", ret);
                return ret;
            }
            if (k.size() < 9) {
                err = "Get3DCameraIntrinsic returned unexpected data";
                return kErrUnknown;
            }
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "{\"width\":%d,\"height\":%d,\"fx\":%.10g,\"fy\":%.10g,\"cx\":%.10g,\"cy\":%.10g}",
                          w, h, k[0], k[4], k[2], k[5]);
            out = buf;
            return AC_OK;
        } catch (const std::exception& e) {
            err = std::string("Get3DCameraIntrinsic threw: ") + e.what();
        } catch (...) {
            err = "Get3DCameraIntrinsic threw an unknown exception";
        }
        return kErrUnknown;
    }
    int set_value_int(const std::string& key, int32_t value, std::string& err) override {
        return sdk_call("SetValue('" + key + "')",
                        [this, &key, value] { return sdk_->SetValue(info_, key, static_cast<int>(value)); }, err);
    }
    int set_value_float(const std::string& key, float value, std::string& err) override {
        return sdk_call("SetValue('" + key + "')",
                        [this, &key, value] { return sdk_->SetValue(info_, key, value); }, err);
    }
    int set_value_bool(const std::string& key, bool value, std::string& err) override {
        return sdk_call("SetValue('" + key + "')",
                        [this, &key, value] { return sdk_->SetValue(info_, key, value); }, err);
    }
    int get_value_int(const std::string& key, int32_t& out, std::string& err) override {
        int v = 0;
        const int ret = sdk_call("GetValue('" + key + "')",
                                 [this, &key, &v] { return sdk_->GetValue(info_, key, v); }, err);
        if (ret == AC_OK)
            out = static_cast<int32_t>(v);
        return ret;
    }
    int get_value_float(const std::string& key, float& out, std::string& err) override {
        return sdk_call("GetValue('" + key + "')",
                        [this, &key, &out] { return sdk_->GetValue(info_, key, out); }, err);
    }
    int get_value_bool(const std::string& key, bool& out, std::string& err) override {
        return sdk_call("GetValue('" + key + "')",
                        [this, &key, &out] { return sdk_->GetValue(info_, key, out); }, err);
    }

private:
    // Sensor exposed to the C layer; masks live on the device (slot 0 = 3D
    // module, slot 1 = RGB camera) and are guarded by the device mutex.
    class Sensor final : public BackendSensor {
    public:
        Sensor(AinstecDevice* dev, int slot, const char* name) : dev_(dev), slot_(slot), name_(name) {}
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
        AinstecDevice* dev_;
        int slot_;
        const char* name_;
    };

    // Runs fn under mu_ inside try/catch; maps the return code and any
    // exception to an error code plus message. on_ok runs on success.
    template <typename Fn, typename OnOk>
    int sdk_call(const std::string& what, Fn&& fn, std::string& err, OnOk&& on_ok) {
        std::lock_guard<std::mutex> lk(mu_);
        try {
            const int ret = fn();
            if (ret != AC_OK) {
                err = sdk_msg(what, ret);
                return ret;
            }
            on_ok();
            return AC_OK;
        } catch (const std::exception& e) {
            err = what + " threw: " + e.what();
        } catch (...) {
            err = what + " threw an unknown exception";
        }
        return kErrUnknown;
    }
    template <typename Fn>
    int sdk_call(const std::string& what, Fn&& fn, std::string& err) {
        return sdk_call(what, std::forward<Fn>(fn), err, [] {});
    }

    gen3d::Camera* sdk_;     // non-owning; owned by AinstecBackend
    gen3d::CameraInfo info_; // de-facto handle, carried into every SDK call
    CameraDesc desc_;
    mutable std::mutex mu_;
    bool opened_ = false;
    uint32_t masks_[2] = {AC_OUTPUT_POINT3D, 0}; // vendor default: points only
    Sensor sensor_3d_{this, 0, "3D Module"};
    Sensor sensor_rgb_{this, 1, "RGB Camera"};
};

class AinstecBackend final : public CamBackend {
public:
    AinstecBackend() {
        try {
            sdk_ = gen3d::CreateCamera(gen3d::CamType::CamPro);
        } catch (...) {
            sdk_ = nullptr;
        }
    }
    ~AinstecBackend() override {
        if (sdk_ != nullptr) {
            try { gen3d::DestoryCamera(sdk_); } catch (...) {}
        }
    }

    int discover(uint32_t timeout_ms,
                 std::vector<std::shared_ptr<BackendDevice>>& out,
                 std::string& err) override {
        std::lock_guard<std::mutex> lk(discover_mu_);
        if (sdk_ == nullptr) {
            err = "gen3d::CreateCamera failed";
            return kErrUnknown;
        }
        try {
            std::vector<gen3d::CameraInfo> found;
            const int ret = sdk_->DiscoverCameras(found, timeout_ms);
            if (ret != AC_OK) {
                err = sdk_msg("DiscoverCameras", ret);
                return ret;
            }
            // Every discovered camera becomes a device, even if a later
            // open() would fail; the info query works regardless.
            out.clear();
            for (const auto& ci : found)
                out.push_back(std::make_shared<AinstecDevice>(sdk_, ci));
            return AC_OK;
        } catch (const std::exception& e) {
            err = std::string("DiscoverCameras threw: ") + e.what();
        } catch (...) {
            err = "DiscoverCameras threw an unknown exception";
        }
        return kErrUnknown;
    }

private:
    gen3d::Camera* sdk_ = nullptr; // one Camera* per backend
    std::mutex discover_mu_;
};

} // namespace

std::unique_ptr<CamBackend> create_ainstec_backend() {
    return std::unique_ptr<CamBackend>(new AinstecBackend());
}

} // namespace acam
