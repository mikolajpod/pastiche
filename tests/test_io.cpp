#include <doctest/doctest.h>

#include "core/fs.hpp"
#include "core/image_io.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <jpeglib.h>
#include <webp/encode.h>

using namespace pastiche;

namespace {

std::string temp_path(const char* name)
{
    return (std::filesystem::temp_directory_path() / (std::string("pastiche_test_") + name)).string();
}

std::vector<uint8_t> encode_jpeg(const Image& img, int quality)
{
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    unsigned char* buf = nullptr;
    unsigned long len = 0;
    jpeg_mem_dest(&cinfo, &buf, &len);
    cinfo.image_width = static_cast<JDIMENSION>(img.width);
    cinfo.image_height = static_cast<JDIMENSION>(img.height);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(img.row(static_cast<int>(cinfo.next_scanline)));
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    std::vector<uint8_t> out(buf, buf + len);
    jpeg_destroy_compress(&cinfo);
    free(buf);
    return out;
}

// Insert an APP1 Exif segment with the given orientation right after SOI.
std::vector<uint8_t> with_orientation(const std::vector<uint8_t>& jpeg, int orientation)
{
    std::vector<uint8_t> exif = {'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, 42, 0, 0, 0, 8,
                                 0, 1,                          // one entry
                                 0x01, 0x12, 0, 3, 0, 0, 0, 1,  // Orientation, SHORT, count 1
                                 0, static_cast<uint8_t>(orientation), 0, 0,
                                 0, 0, 0, 0};                   // next IFD
    std::vector<uint8_t> out = {0xFF, 0xD8, 0xFF, 0xE1};
    const uint16_t len = static_cast<uint16_t>(exif.size() + 2);
    out.push_back(len >> 8); out.push_back(len & 0xFF);
    out.insert(out.end(), exif.begin(), exif.end());
    out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
    return out;
}

} // namespace

TEST_CASE("format detection")
{
    const uint8_t png[16] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    const uint8_t jpg[16] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1};
    const uint8_t webp[16] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
    const uint8_t jxl[16] = {0xFF, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t junk[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    CHECK(detect_format(png, 16) == ImageFormat::PNG);
    CHECK(detect_format(jpg, 16) == ImageFormat::JPEG);
    CHECK(detect_format(webp, 16) == ImageFormat::WebP);
    CHECK(detect_format(jxl, 16) == ImageFormat::JXL);
    CHECK(detect_format(junk, 16) == ImageFormat::Unknown);
    CHECK(detect_format(png, 4) == ImageFormat::Unknown);
    Image img;
    CHECK(decode_image(junk, 16, img) != "");
}

TEST_CASE("png write/read roundtrip, RGB and RGBA")
{
    const std::string p = temp_path("rt.png");
    Image rgb = make_test_image(37, 23, 3);
    REQUIRE(save_png(p, rgb) == "");
    Image back;
    REQUIRE(load_image(p, back) == "");
    CHECK(back.channels == 3);
    CHECK(back.data == rgb.data);

    Image rgba = to_rgba(rgb);
    rgba.px(1, 1)[3] = 17;
    REQUIRE(save_image(p, rgba) == "");
    REQUIRE(load_image(p, back) == "");
    CHECK(back.channels == 4);
    CHECK(back.data == rgba.data);
    std::remove(p.c_str());
    CHECK(save_image(temp_path("x.bmp"), rgb) != "");
    CHECK(load_image(temp_path("does_not_exist.png"), back) != "");
}

TEST_CASE("jpeg decode applies exif orientation")
{
    Image src = make_test_image(40, 24, 4);
    const std::vector<uint8_t> plain = encode_jpeg(src, 95);
    Image a;
    REQUIRE(decode_image(plain.data(), plain.size(), a) == "");
    CHECK(a.width == 40); CHECK(a.height == 24); CHECK(a.channels == 3);
    // lossy (chroma subsampling smears the hard edges of the test image), so
    // compare the mean absolute error rather than the worst pixel
    double sum = 0;
    for (size_t i = 0; i < a.data.size(); ++i) sum += std::abs(int(a.data[i]) - int(src.data[i]));
    CHECK(sum / a.data.size() < 6.0);

    const std::vector<uint8_t> rotated = with_orientation(plain, 6);
    Image b;
    REQUIRE(decode_image(rotated.data(), rotated.size(), b) == "");
    CHECK(b.width == 24); CHECK(b.height == 40);
    // pixel (0,0) of the rotated image is the bottom-left of the stored one
    CHECK(std::abs(int(b.px(0, 0)[0]) - int(a.px(0, 23)[0])) < 8);

    std::vector<uint8_t> broken(plain.begin(), plain.begin() + 40);
    Image c;
    CHECK(decode_image(broken.data(), broken.size(), c) != "");
}

TEST_CASE("webp decode with and without alpha")
{
    Image src = make_test_image(33, 21, 5);
    uint8_t* out = nullptr;
    size_t n = WebPEncodeLosslessRGB(src.data.data(), src.width, src.height, static_cast<int>(src.row_bytes()), &out);
    REQUIRE(n > 0);
    Image a;
    REQUIRE(decode_image(out, n, a) == "");
    WebPFree(out);
    CHECK(a.channels == 3);
    CHECK(a.data == src.data);

    Image rgba = to_rgba(src);
    rgba.px(2, 2)[3] = 0;
    n = WebPEncodeLosslessRGBA(rgba.data.data(), rgba.width, rgba.height, static_cast<int>(rgba.row_bytes()), &out);
    REQUIRE(n > 0);
    Image b;
    REQUIRE(decode_image(out, n, b) == "");
    WebPFree(out);
    CHECK(b.channels == 4);
    CHECK(b.px(2, 2)[3] == 0);
    CHECK(b.px(5, 5)[3] == 255);
}

TEST_CASE("jxl roundtrip when available")
{
    if (!jxl_available()) return;
    const std::string p = temp_path("rt.jxl");
    Image rgb = make_test_image(29, 31, 6);
    REQUIRE(save_image(p, rgb) == "");
    Image back;
    REQUIRE(load_image(p, back) == "");
    CHECK(back.channels == 3);
    CHECK(back.data == rgb.data);
    std::remove(p.c_str());
}

TEST_CASE("fs helpers")
{
    CHECK(path_extension("a/b/c.PNG") == ".png");
    CHECK(path_extension("noext") == "");
    CHECK(path_extension(".hidden") == "");
    CHECK(path_basename("a/b\\c.png") == "c.png");
    CHECK(path_dirname("a/b/c.png") == "a/b");
    CHECK(path_dirname("c.png") == "");
    CHECK(path_stem("x/y.tar.gz") == "y.tar");
    CHECK(replace_extension("out/img.png", ".json") == "out/img.json");
    CHECK(replace_extension("out/img", ".json") == "out/img.json");
    CHECK(path_join("a/", "b") == "a/b");
    CHECK(path_join("a", "b") == "a/b");
    CHECK(timestamp_now().size() == 15);
    const std::string d = temp_path("dir/sub");
    CHECK(make_dirs(d));
    CHECK(dir_exists(d));
    CHECK(write_text_file(path_join(d, "f.txt"), "hi") == "");
    CHECK(file_exists(path_join(d, "f.txt")));
    CHECK(file_size(path_join(d, "f.txt")) == 2);
    CHECK(list_dir(d) == std::vector<std::string>{"f.txt"});
    std::filesystem::remove_all(temp_path("dir"));
}
