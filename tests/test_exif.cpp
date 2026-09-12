#include <doctest/doctest.h>

#include "core/exif.hpp"

#include <cstdint>
#include <vector>

using namespace pastiche;

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x, bool le)
{
    if (le) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
    else { v.push_back(x >> 8); v.push_back(x & 0xFF); }
}
void put32(std::vector<uint8_t>& v, uint32_t x, bool le)
{
    if (le) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); }
    else { for (int i = 3; i >= 0; --i) v.push_back((x >> (8 * i)) & 0xFF); }
}

// Builds "Exif\0\0" + TIFF header + IFD0 with a few tags including Orientation.
std::vector<uint8_t> make_exif(int orientation, bool le, bool with_prefix = true, uint16_t type = 3)
{
    std::vector<uint8_t> v;
    if (with_prefix) v.insert(v.end(), {'E', 'x', 'i', 'f', 0, 0});
    const size_t tiff = v.size();
    v.push_back(le ? 'I' : 'M'); v.push_back(le ? 'I' : 'M');
    put16(v, 42, le);
    put32(v, 8, le);              // IFD0 offset
    put16(v, 3, le);              // 3 entries
    // 0x010F Make (ASCII, offset elsewhere)
    put16(v, 0x010F, le); put16(v, 2, le); put32(v, 4, le); put32(v, 100, le);
    // 0x0112 Orientation
    put16(v, 0x0112, le); put16(v, type, le); put32(v, 1, le);
    if (type == 3) { put16(v, static_cast<uint16_t>(orientation), le); put16(v, 0, le); }
    else put32(v, static_cast<uint32_t>(orientation), le);
    // 0x011A XResolution (RATIONAL, offset)
    put16(v, 0x011A, le); put16(v, 5, le); put32(v, 1, le); put32(v, 200, le);
    put32(v, 0, le);              // next IFD
    (void)tiff;
    return v;
}

std::vector<uint8_t> make_jpeg(const std::vector<uint8_t>* app1)
{
    std::vector<uint8_t> j = {0xFF, 0xD8};
    // APP0 JFIF
    std::vector<uint8_t> app0 = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    j.push_back(0xFF); j.push_back(0xE0);
    put16(j, static_cast<uint16_t>(app0.size() + 2), false);
    j.insert(j.end(), app0.begin(), app0.end());
    if (app1) {
        j.push_back(0xFF); j.push_back(0xE1);
        put16(j, static_cast<uint16_t>(app1->size() + 2), false);
        j.insert(j.end(), app1->begin(), app1->end());
    }
    // SOS then fake entropy data
    j.push_back(0xFF); j.push_back(0xDA); put16(j, 2, false);
    j.insert(j.end(), {1, 2, 3, 0xFF, 0xD9});
    return j;
}

} // namespace

TEST_CASE("orientation tag little and big endian")
{
    for (int o = 1; o <= 8; ++o) {
        const auto le = make_exif(o, true);
        const auto be = make_exif(o, false);
        CHECK(exif_orientation(le.data(), le.size()) == o);
        CHECK(exif_orientation(be.data(), be.size()) == o);
    }
}

TEST_CASE("orientation without Exif prefix and as LONG")
{
    const auto a = make_exif(6, true, false);
    CHECK(exif_orientation(a.data(), a.size()) == 6);
    const auto b = make_exif(8, false, true, 4);
    CHECK(exif_orientation(b.data(), b.size()) == 8);
}

TEST_CASE("malformed or missing orientation yields 0")
{
    const auto bad_value = make_exif(9, true);
    CHECK(exif_orientation(bad_value.data(), bad_value.size()) == 0);
    const auto zero = make_exif(0, false);
    CHECK(exif_orientation(zero.data(), zero.size()) == 0);
    auto truncated = make_exif(6, true);
    truncated.resize(20);
    CHECK(exif_orientation(truncated.data(), truncated.size()) == 0);
    const std::vector<uint8_t> garbage = {'X', 'Y', 0, 42, 0, 0, 0, 8, 0, 0};
    CHECK(exif_orientation(garbage.data(), garbage.size()) == 0);
    CHECK(exif_orientation(nullptr, 0) == 0);
    auto huge_offset = make_exif(6, true);
    huge_offset[6 + 4] = 0xFF; huge_offset[6 + 5] = 0xFF;
    CHECK(exif_orientation(huge_offset.data(), huge_offset.size()) == 0);
}

TEST_CASE("jpeg segment scan finds APP1 after APP0")
{
    const auto exif = make_exif(3, false);
    const auto j = make_jpeg(&exif);
    CHECK(jpeg_exif_orientation(j.data(), j.size()) == 3);
    const auto plain = make_jpeg(nullptr);
    CHECK(jpeg_exif_orientation(plain.data(), plain.size()) == 0);
    const std::vector<uint8_t> not_jpeg = {0x89, 'P', 'N', 'G', 0, 0, 0, 0};
    CHECK(jpeg_exif_orientation(not_jpeg.data(), not_jpeg.size()) == 0);
    auto cut = j;
    cut.resize(8);
    CHECK(jpeg_exif_orientation(cut.data(), cut.size()) == 0);
}
