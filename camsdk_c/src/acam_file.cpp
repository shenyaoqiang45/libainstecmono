#include "acam_file.hpp"

#include <cstring>

namespace acam {
namespace {

constexpr char kMagic[4] = {'A', 'C', 'A', 'M'};
constexpr char kFrm1[4]  = {'F', 'R', 'M', '1'};
constexpr char kEof1[4]  = {'E', 'O', 'F', '1'};

template <typename T>
bool write_pod(std::ostream& out, const T& v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(T));
    return static_cast<bool>(out);
}

template <typename T>
bool read_pod(std::istream& in, T& v) {
    in.read(reinterpret_cast<char*>(&v), sizeof(T));
    return static_cast<bool>(in) && in.gcount() == static_cast<std::streamsize>(sizeof(T));
}

bool write_bytes(std::ostream& out, const void* p, size_t n) {
    if (n == 0)
        return static_cast<bool>(out);
    out.write(static_cast<const char*>(p), static_cast<std::streamsize>(n));
    return static_cast<bool>(out);
}

bool read_bytes(std::istream& in, void* p, size_t n) {
    if (n == 0)
        return static_cast<bool>(in);
    in.read(static_cast<char*>(p), static_cast<std::streamsize>(n));
    return static_cast<bool>(in) && in.gcount() == static_cast<std::streamsize>(n);
}

// Extract a top-level JSON object/array value for `key` without a full parser.
// Returns the raw JSON text of the value (including braces/brackets), or "".
std::string extract_json_value(const std::string& doc, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = doc.find(needle);
    if (pos == std::string::npos)
        return {};
    pos = doc.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return {};
    ++pos;
    while (pos < doc.size() && (doc[pos] == ' ' || doc[pos] == '\t' || doc[pos] == '\n' ||
                                doc[pos] == '\r'))
        ++pos;
    if (pos >= doc.size())
        return {};
    const char open = doc[pos];
    char close = 0;
    if (open == '{')
        close = '}';
    else if (open == '[')
        close = ']';
    else if (open == '"') {
        size_t i = pos + 1;
        while (i < doc.size()) {
            if (doc[i] == '\\' && i + 1 < doc.size()) {
                i += 2;
                continue;
            }
            if (doc[i] == '"')
                return doc.substr(pos, i - pos + 1);
            ++i;
        }
        return {};
    } else {
        // number / bool / null
        size_t i = pos;
        while (i < doc.size() && doc[i] != ',' && doc[i] != '}' && doc[i] != ']')
            ++i;
        return doc.substr(pos, i - pos);
    }
    int depth = 0;
    bool in_str = false;
    for (size_t i = pos; i < doc.size(); ++i) {
        const char c = doc[i];
        if (in_str) {
            if (c == '\\' && i + 1 < doc.size()) {
                ++i;
                continue;
            }
            if (c == '"')
                in_str = false;
            continue;
        }
        if (c == '"') {
            in_str = true;
            continue;
        }
        if (c == open)
            ++depth;
        else if (c == close) {
            --depth;
            if (depth == 0)
                return doc.substr(pos, i - pos + 1);
        }
    }
    return {};
}

} // namespace

std::string build_acam_header_document(const AcamHeader& h) {
    std::string doc = "{";
    doc += "\"info\":";
    doc += h.info_json.empty() ? "{}" : h.info_json;
    doc += ",\"attributes\":";
    // Prefer embedding the flat tree object; if caller passed only the array,
    // wrap it.
    if (h.attributes_json.empty()) {
        doc += "{\"attributes\":[]}";
    } else if (!h.attributes_json.empty() && h.attributes_json[0] == '[') {
        doc += "{\"attributes\":";
        doc += h.attributes_json;
        doc += "}";
    } else {
        doc += h.attributes_json;
    }
    doc += ",\"values\":";
    doc += h.values_json.empty() ? "{}" : h.values_json;
    doc += ",\"intrinsics\":";
    doc += h.intrinsics_json.empty() ? "{}" : h.intrinsics_json;
    doc += "}";
    return doc;
}

bool parse_acam_header_document(const std::string& doc, AcamHeader& out) {
    out.info_json = extract_json_value(doc, "info");
    if (out.info_json.empty())
        out.info_json = "{}";
    std::string attrs = extract_json_value(doc, "attributes");
    if (attrs.empty()) {
        out.attributes_json = "{\"attributes\":[]}";
    } else if (!attrs.empty() && attrs[0] == '[') {
        out.attributes_json = std::string("{\"attributes\":") + attrs + "}";
    } else {
        out.attributes_json = attrs;
    }
    out.values_json = extract_json_value(doc, "values");
    if (out.values_json.empty())
        out.values_json = "{}";
    out.intrinsics_json = extract_json_value(doc, "intrinsics");
    if (out.intrinsics_json.empty())
        out.intrinsics_json = "{}";
    return true;
}

int AcamWriter::open(const std::string& path, const AcamHeader& header, std::string& err) {
    if (out_.is_open()) {
        err = "acam writer already open";
        return kErrInvalidParam;
    }
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_) {
        err = "failed to open acam file for write: " + path;
        return kErrInvalidParam;
    }
    const std::string doc = build_acam_header_document(header);
    if (doc.size() > 0xffffffffu) {
        err = "acam header too large";
        out_.close();
        return kErrInvalidParam;
    }
    if (!write_bytes(out_, kMagic, 4) || !write_pod(out_, kAcamVersion) ||
        !write_pod(out_, static_cast<uint32_t>(doc.size())) ||
        !write_bytes(out_, doc.data(), doc.size())) {
        err = "failed to write acam header";
        out_.close();
        return kErrUnknown;
    }
    return AC_OK;
}

int AcamWriter::write_frameset(const CaptureData& cap, std::string& err) {
    if (!out_.is_open()) {
        err = "acam writer is not open";
        return kErrNotOpened;
    }
    const auto& planes = cap.planes();
    if (!write_bytes(out_, kFrm1, 4) || !write_pod(out_, cap.index) ||
        !write_pod(out_, cap.timestamp_s) ||
        !write_pod(out_, static_cast<uint32_t>(planes.size()))) {
        err = "failed to write acam frameset header";
        return kErrUnknown;
    }
    for (const Plane& p : planes) {
        const uint32_t type = static_cast<uint32_t>(p.type);
        if (!write_pod(out_, type) || !write_pod(out_, p.w) || !write_pod(out_, p.h) ||
            !write_pod(out_, static_cast<uint64_t>(p.byte_size)) ||
            !write_bytes(out_, p.data, p.byte_size)) {
            err = "failed to write acam plane";
            return kErrUnknown;
        }
    }
    return AC_OK;
}

int AcamWriter::close(std::string& err) {
    if (!out_.is_open())
        return AC_OK;
    if (!write_bytes(out_, kEof1, 4)) {
        err = "failed to write acam EOF trailer";
        out_.close();
        return kErrUnknown;
    }
    out_.flush();
    out_.close();
    return AC_OK;
}

int AcamReader::open(const std::string& path, AcamHeader& header, std::string& err) {
    if (in_.is_open()) {
        err = "acam reader already open";
        return kErrInvalidParam;
    }
    in_.open(path, std::ios::binary);
    if (!in_) {
        err = "failed to open acam file for read: " + path;
        return kErrInvalidParam;
    }
    char magic[4] = {};
    uint32_t version = 0;
    uint32_t header_len = 0;
    if (!read_bytes(in_, magic, 4) || std::memcmp(magic, kMagic, 4) != 0) {
        err = "not an acam file (bad magic)";
        in_.close();
        return kErrInvalidParam;
    }
    if (!read_pod(in_, version) || version != kAcamVersion) {
        err = "unsupported acam version";
        in_.close();
        return kErrInvalidParam;
    }
    if (!read_pod(in_, header_len)) {
        err = "truncated acam header";
        in_.close();
        return kErrInvalidParam;
    }
    std::string doc(header_len, '\0');
    if (header_len > 0 && !read_bytes(in_, &doc[0], header_len)) {
        err = "truncated acam header body";
        in_.close();
        return kErrInvalidParam;
    }
    parse_acam_header_document(doc, header);
    return AC_OK;
}

int AcamReader::read_frameset(std::shared_ptr<BufferCapture>& out, std::string& err) {
    if (!in_.is_open()) {
        err = "acam reader is not open";
        return kErrNotOpened;
    }
    char tag[4] = {};
    if (!read_bytes(in_, tag, 4)) {
        err = "end of file";
        return kErrNoData;
    }
    if (std::memcmp(tag, kEof1, 4) == 0) {
        err = "end of file";
        return kErrNoData;
    }
    if (std::memcmp(tag, kFrm1, 4) != 0) {
        err = "corrupt acam frameset tag";
        return kErrInvalidParam;
    }
    uint64_t index = 0;
    double timestamp_s = 0.0;
    uint32_t plane_count = 0;
    if (!read_pod(in_, index) || !read_pod(in_, timestamp_s) || !read_pod(in_, plane_count)) {
        err = "truncated acam frameset header";
        return kErrInvalidParam;
    }
    auto cap = std::make_shared<BufferCapture>();
    cap->index = index;
    cap->timestamp_s = timestamp_s;
    for (uint32_t i = 0; i < plane_count; ++i) {
        uint32_t type = 0, w = 0, h = 0;
        uint64_t byte_size = 0;
        if (!read_pod(in_, type) || !read_pod(in_, w) || !read_pod(in_, h) ||
            !read_pod(in_, byte_size)) {
            err = "truncated acam plane header";
            return kErrInvalidParam;
        }
        uint8_t* dst = cap->add_plane(static_cast<ac_frame_type>(type), w, h,
                                      static_cast<size_t>(byte_size));
        if (byte_size > 0 && !read_bytes(in_, dst, static_cast<size_t>(byte_size))) {
            err = "truncated acam plane payload";
            return kErrInvalidParam;
        }
    }
    out = std::move(cap);
    return AC_OK;
}

void AcamReader::close() {
    if (in_.is_open())
        in_.close();
}

} // namespace acam
