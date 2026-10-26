// acam_file.hpp - self-contained .acam v1 container (record / playback).
#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "camsdk_backend.h"

namespace acam {

constexpr uint32_t kAcamVersion = 1;

// Header JSON fields (v1): info, attributes (flat array or tree object),
// optional values map, intrinsics object, calibration object (added later in
// v1: absent in older files; readers fall back to depth-only intrinsics).
struct AcamHeader {
    std::string info_json;         // {"name",...}
    std::string attributes_json;   // {"attributes":[...]} flat tree
    std::string values_json;       // {"Exposure":10,...} optional, may be "{}"
    std::string intrinsics_json;   // {"width",...}
    std::string calibration_json;  // {"depth":...,"rgb":...,"points_to_rgb":...}
};

class AcamWriter {
public:
    int open(const std::string& path, const AcamHeader& header, std::string& err);
    int write_frameset(const CaptureData& cap, std::string& err);
    int close(std::string& err); // writes EOF1 trailer
    bool is_open() const { return out_.is_open(); }

private:
    std::ofstream out_;
};

class AcamReader {
public:
    int open(const std::string& path, AcamHeader& header, std::string& err);
    // Reads the next frameset into a BufferCapture. Returns kErrNoData at EOF.
    int read_frameset(std::shared_ptr<BufferCapture>& out, std::string& err);
    void close();
    bool is_open() const { return in_.is_open(); }

private:
    std::ifstream in_;
};

// Build the file-level header JSON document from AcamHeader parts.
std::string build_acam_header_document(const AcamHeader& h);
// Parse header document into AcamHeader parts (best-effort field extract).
bool parse_acam_header_document(const std::string& doc, AcamHeader& out);

} // namespace acam
