#include "safetensors.hpp"

#include "fs.hpp"
#include "json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace pastiche {

namespace {

// The header of a multi-gigabyte checkpoint is still only tens of kilobytes;
// this cap is here so a corrupt length field cannot make us allocate wildly.
constexpr uint64_t kMaxHeaderBytes = 64ull << 20;
constexpr std::size_t kCopyChunk = 8u << 20;

bool read_exactly(FILE* f, void* dst, std::size_t n)
{
    return std::fread(dst, 1, n, f) == n;
}

// Tensor names are ASCII in every checkpoint in the wild, so only the two
// characters JSON actually requires escaping are handled; anything else is
// passed through, which keeps UTF-8 intact.
void append_json_key(const std::string& key, std::string& out)
{
    out += '"';
    for (char c : key) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
}

} // namespace

std::string safetensors_prefix_tensors(const std::string& src_path, const std::string& dst_path,
                                       const std::string& prefix,
                                       const std::vector<std::string>& drop)
{
    FILE* src = fopen_utf8(src_path, "rb");
    if (!src) return "cannot open " + src_path;

    uint8_t length_bytes[8];
    if (!read_exactly(src, length_bytes, sizeof length_bytes)) {
        std::fclose(src);
        return src_path + " is too short to be a safetensors file";
    }
    uint64_t header_len = 0;
    for (int i = 7; i >= 0; --i) header_len = (header_len << 8) | length_bytes[i];

    if (header_len == 0 || header_len > kMaxHeaderBytes) {
        std::fclose(src);
        return src_path + ": implausible header length, this is probably not a safetensors file";
    }

    std::string header_text(static_cast<std::size_t>(header_len), '\0');
    if (!read_exactly(src, &header_text[0], header_text.size())) {
        std::fclose(src);
        return src_path + ": truncated header";
    }

    std::string json_error;
    const JsonValue header = json_parse(header_text, json_error);
    if (!json_error.empty()) {
        std::fclose(src);
        return src_path + ": malformed header (" + json_error + ")";
    }
    if (!header.is_object()) {
        std::fclose(src);
        return src_path + ": header is not a JSON object";
    }

    // Rebuild the header by hand rather than through JsonValue, which has no
    // mutation API; member order is preserved, which keeps tensors in the same
    // order as their data.
    std::string rebuilt = "{";
    bool first = true;
    std::size_t renamed = 0;
    for (const auto& member : header.members()) {
        const std::string& name = member.first;
        if (std::find(drop.begin(), drop.end(), name) != drop.end()) continue;

        std::string out_name = name;
        if (name != "__metadata__" && name.rfind(prefix, 0) != 0) {
            out_name = prefix + name;
            ++renamed;
        }

        if (!first) rebuilt += ',';
        first = false;
        append_json_key(out_name, rebuilt);
        rebuilt += ':';
        rebuilt += json_dump(member.second);
    }
    rebuilt += '}';

    if (renamed == 0) {
        std::fclose(src);
        return src_path + ": nothing to rename (already prefixed?)";
    }

    // safetensors readers are entitled to assume the data starts 8-byte
    // aligned; trailing spaces are valid JSON padding.
    while ((rebuilt.size() + 8) % 8 != 0) rebuilt += ' ';

    const std::string dir = path_dirname(dst_path);
    if (!dir.empty() && !dir_exists(dir) && !make_dirs(dir)) {
        std::fclose(src);
        return "cannot create directory " + dir;
    }

    // Write to a temporary and rename, so an interrupted conversion cannot
    // leave a half-written file that looks finished (same rule as downloads).
    const std::string tmp_path = dst_path + ".part";
    FILE* dst = fopen_utf8(tmp_path, "wb");
    if (!dst) {
        std::fclose(src);
        return "cannot open " + tmp_path + " for writing";
    }

    uint8_t out_len[8];
    const uint64_t new_len = rebuilt.size();
    for (int i = 0; i < 8; ++i) out_len[i] = static_cast<uint8_t>((new_len >> (i * 8)) & 0xff);

    std::string error;
    if (std::fwrite(out_len, 1, sizeof out_len, dst) != sizeof out_len ||
        std::fwrite(rebuilt.data(), 1, rebuilt.size(), dst) != rebuilt.size()) {
        error = "write error on " + tmp_path;
    }

    std::vector<uint8_t> buffer(kCopyChunk);
    while (error.empty()) {
        const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), src);
        if (got > 0 && std::fwrite(buffer.data(), 1, got, dst) != got) {
            error = "write error on " + tmp_path + " (disk full?)";
            break;
        }
        if (got < buffer.size()) {
            if (std::ferror(src)) error = "read error on " + src_path;
            break;
        }
    }

    std::fclose(src);
    std::fclose(dst);

    if (!error.empty()) {
        std::remove(tmp_path.c_str());
        return error;
    }

    std::remove(dst_path.c_str());
    if (std::rename(tmp_path.c_str(), dst_path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return "cannot rename " + tmp_path + " to " + dst_path;
    }
    return {};
}

} // namespace pastiche
