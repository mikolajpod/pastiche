#include <doctest/doctest.h>

#include "core/fs.hpp"
#include "core/json.hpp"
#include "core/safetensors.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace pastiche;

namespace {

// Builds a minimal but valid safetensors file: two tiny tensors whose data is
// recognisable, so the test can prove the payload survived the rewrite.
std::string write_sample(const std::string& path)
{
    const std::string header =
        R"({"vision_model.weight":{"dtype":"F32","shape":[2],"data_offsets":[0,8]},)"
        R"("vision_model.bias":{"dtype":"F32","shape":[1],"data_offsets":[8,12]},)"
        R"("__metadata__":{"format":"pt"}})";

    std::string padded = header;
    while ((padded.size() + 8) % 8 != 0) padded += ' ';

    std::string out;
    for (int i = 0; i < 8; ++i) {
        out += static_cast<char>((padded.size() >> (i * 8)) & 0xff);
    }
    out += padded;
    // 12 bytes of payload with a pattern we can check byte for byte.
    for (int i = 0; i < 12; ++i) out += static_cast<char>(0xA0 + i);

    const std::string error = write_file(path, out.data(), out.size());
    REQUIRE(error.empty());
    return out.substr(out.size() - 12);
}

// Reads back the header and the payload of a safetensors file.
void read_back(const std::string& path, JsonValue& header, std::string& payload)
{
    std::vector<uint8_t> bytes;
    REQUIRE(read_file(path, bytes).empty());
    REQUIRE(bytes.size() > 8);

    uint64_t len = 0;
    for (int i = 7; i >= 0; --i) len = (len << 8) | bytes[static_cast<size_t>(i)];
    REQUIRE(8 + len <= bytes.size());

    const std::string text(reinterpret_cast<const char*>(bytes.data() + 8), static_cast<size_t>(len));
    std::string error;
    header = json_parse(text, error);
    REQUIRE(error.empty());

    payload.assign(reinterpret_cast<const char*>(bytes.data() + 8 + len),
                   bytes.size() - 8 - static_cast<size_t>(len));
}

} // namespace

TEST_CASE("safetensors prefixing renames tensors and keeps the payload intact")
{
    const std::string src = "test_st_src.safetensors";
    const std::string dst = "test_st_dst.safetensors";
    const std::string payload = write_sample(src);

    REQUIRE(safetensors_prefix_tensors(src, dst, "clip_vision.").empty());

    JsonValue header;
    std::string got_payload;
    read_back(dst, header, got_payload);

    CHECK(header.find("clip_vision.vision_model.weight") != nullptr);
    CHECK(header.find("clip_vision.vision_model.bias") != nullptr);
    // The unprefixed names must be gone, or the loader would see both.
    CHECK(header.find("vision_model.weight") == nullptr);
    // Metadata is not a tensor and must not be renamed.
    CHECK(header.find("__metadata__") != nullptr);

    // Offsets are relative to the payload, so they must be untouched even
    // though the header in front of them changed size.
    const JsonValue* weight = header.find("clip_vision.vision_model.weight");
    REQUIRE(weight != nullptr);
    const JsonValue* offsets = weight->find("data_offsets");
    REQUIRE(offsets != nullptr);
    REQUIRE(offsets->items().size() == 2);
    CHECK(offsets->items()[0].as_int() == 0);
    CHECK(offsets->items()[1].as_int() == 8);
    CHECK(weight->string_or("dtype") == "F32");

    CHECK(got_payload == payload);

    std::remove(src.c_str());
    std::remove(dst.c_str());
}

TEST_CASE("safetensors prefixing can drop tensors")
{
    const std::string src = "test_st_drop_src.safetensors";
    const std::string dst = "test_st_drop_dst.safetensors";
    write_sample(src);

    REQUIRE(safetensors_prefix_tensors(src, dst, "clip_vision.", {"vision_model.bias"}).empty());

    JsonValue header;
    std::string payload;
    read_back(dst, header, payload);
    CHECK(header.find("clip_vision.vision_model.weight") != nullptr);
    CHECK(header.find("clip_vision.vision_model.bias") == nullptr);

    std::remove(src.c_str());
    std::remove(dst.c_str());
}

TEST_CASE("safetensors prefixing refuses files it does not understand")
{
    const std::string path = "test_st_bad.safetensors";
    const std::string out = "test_st_bad_out.safetensors";

    // Nonsense header length: must be rejected rather than allocated.
    const std::string garbage(64, '\xff');
    REQUIRE(write_file(path, garbage.data(), garbage.size()).empty());
    CHECK_FALSE(safetensors_prefix_tensors(path, out, "x.").empty());
    CHECK_FALSE(file_exists(out));

    // Too short to even hold a length.
    const std::string tiny = "abc";
    REQUIRE(write_file(path, tiny.data(), tiny.size()).empty());
    CHECK_FALSE(safetensors_prefix_tensors(path, out, "x.").empty());

    CHECK_FALSE(safetensors_prefix_tensors("no_such_file.safetensors", out, "x.").empty());

    std::remove(path.c_str());
}

TEST_CASE("safetensors prefixing is idempotent")
{
    const std::string src = "test_st_idem_src.safetensors";
    const std::string dst = "test_st_idem_dst.safetensors";
    write_sample(src);

    REQUIRE(safetensors_prefix_tensors(src, dst, "clip_vision.").empty());
    // Running it again over the result finds nothing left to rename and says so
    // rather than producing a doubly prefixed file.
    const std::string second = safetensors_prefix_tensors(dst, dst, "clip_vision.");
    CHECK_FALSE(second.empty());

    std::remove(src.c_str());
    std::remove(dst.c_str());
}
