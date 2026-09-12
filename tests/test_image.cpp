#include <doctest/doctest.h>

#include "core/image.hpp"

#include <limits>
#include <vector>

using namespace pastiche;

namespace {

// 3x2 RGB image with distinct pixel values: value = y*3 + x in every channel.
Image small()
{
    Image img(3, 2, 3);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
            for (int k = 0; k < 3; ++k) img.px(x, y)[k] = static_cast<uint8_t>(y * 3 + x + k * 10);
    return img;
}

int v(const Image& img, int x, int y) { return img.px(x, y)[0]; }

} // namespace

TEST_CASE("apply_orientation moves pixels to the right place")
{
    const Image s = small();
    // original:  0 1 2
    //            3 4 5
    Image o1 = apply_orientation(s, 1);
    CHECK(o1.width == 3); CHECK(v(o1, 0, 0) == 0); CHECK(v(o1, 2, 1) == 5);

    Image o2 = apply_orientation(s, 2);   // mirror horizontal: 2 1 0 / 5 4 3
    CHECK(v(o2, 0, 0) == 2); CHECK(v(o2, 2, 1) == 3);

    Image o3 = apply_orientation(s, 3);   // rotate 180: 5 4 3 / 2 1 0
    CHECK(v(o3, 0, 0) == 5); CHECK(v(o3, 2, 1) == 0);

    Image o4 = apply_orientation(s, 4);   // mirror vertical: 3 4 5 / 0 1 2
    CHECK(v(o4, 0, 0) == 3); CHECK(v(o4, 2, 1) == 2);

    Image o5 = apply_orientation(s, 5);   // transpose: 2x3: 0 3 / 1 4 / 2 5
    CHECK(o5.width == 2); CHECK(o5.height == 3);
    CHECK(v(o5, 0, 0) == 0); CHECK(v(o5, 1, 0) == 3); CHECK(v(o5, 0, 2) == 2);

    Image o6 = apply_orientation(s, 6);   // rotate 90 CW: 3 0 / 4 1 / 5 2
    CHECK(o6.width == 2); CHECK(o6.height == 3);
    CHECK(v(o6, 0, 0) == 3); CHECK(v(o6, 1, 0) == 0); CHECK(v(o6, 1, 2) == 2);

    Image o7 = apply_orientation(s, 7);   // transverse: 5 2 / 4 1 / 3 0
    CHECK(v(o7, 0, 0) == 5); CHECK(v(o7, 1, 2) == 0);

    Image o8 = apply_orientation(s, 8);   // rotate 270 CW: 2 5 / 1 4 / 0 3
    CHECK(v(o8, 0, 0) == 2); CHECK(v(o8, 1, 0) == 5); CHECK(v(o8, 0, 2) == 0);

    // channels travel together
    CHECK(o6.px(0, 0)[2] == 3 + 20);
}

TEST_CASE("area downscale averages exactly")
{
    Image img(4, 4, 3);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const uint8_t val = (x < 2) ? (y < 2 ? 0 : 200) : (y < 2 ? 100 : 40);
            for (int k = 0; k < 3; ++k) img.px(x, y)[k] = val;
        }
    // put one different pixel into the top-left block: (0,0)=40 -> mean (40+0+0+0)/4 = 10
    img.px(0, 0)[0] = 40;
    Image half = resize(img, 2, 2);
    CHECK(half.width == 2); CHECK(half.height == 2);
    CHECK(half.px(0, 0)[0] == 10);
    CHECK(half.px(1, 0)[0] == 100);
    CHECK(half.px(0, 1)[0] == 200);
    CHECK(half.px(1, 1)[0] == 40);
    CHECK(half.px(0, 0)[1] == 0);
}

TEST_CASE("fit_longest_side keeps aspect and honours upscale flag")
{
    Image img = make_test_image(400, 200);
    Image a = fit_longest_side(img, 100);
    CHECK(a.width == 100); CHECK(a.height == 50);
    Image b = fit_longest_side(img, 800);           // no upscale by default
    CHECK(b.width == 400);
    Image c = fit_longest_side(img, 800, true);
    CHECK(c.width == 800); CHECK(c.height == 400);
    Image d = fit_longest_side(img, 0);
    CHECK(d.width == 400);
    Image tall = fit_longest_side(make_test_image(30, 90), 45);
    CHECK(tall.width == 15); CHECK(tall.height == 45);
}

TEST_CASE("bilinear upscale of a constant image stays constant")
{
    Image img(3, 3, 4);
    for (auto& b : img.data) b = 77;
    Image up = resize(img, 10, 7);
    CHECK(up.channels == 4);
    for (auto b : up.data) CHECK(b == 77);
}

TEST_CASE("chw roundtrip and NaN handling")
{
    Image img = make_test_image(17, 9);
    std::vector<float> chw;
    image_to_chw(img, chw, 1.f / 255.f);
    REQUIRE(chw.size() == 17u * 9u * 3u);
    CHECK(chw[0] == doctest::Approx(img.px(0, 0)[0] / 255.f));
    CHECK(chw[17 * 9] == doctest::Approx(img.px(0, 0)[1] / 255.f));
    Image back = chw_to_image(chw.data(), 17, 9, 255.f);
    CHECK(back.data == img.data);
    chw[5] = std::numeric_limits<float>::quiet_NaN();
    chw[6] = 1e9f;
    Image clamped = chw_to_image(chw.data(), 17, 9, 255.f);
    CHECK(clamped.px(5, 0)[0] == 0);
    CHECK(clamped.px(6, 0)[0] == 255);
}

TEST_CASE("rgb/rgba conversions and crop")
{
    Image rgb = make_test_image(8, 5);
    Image rgba = to_rgba(rgb);
    CHECK(rgba.channels == 4);
    CHECK(rgba.px(3, 2)[3] == 255);
    CHECK(rgba.px(3, 2)[1] == rgb.px(3, 2)[1]);
    Image again = to_rgb(rgba);
    CHECK(again.data == rgb.data);
    Image c = crop(rgb, 2, 1, 4, 3);
    CHECK(c.width == 4); CHECK(c.height == 3);
    CHECK(c.px(0, 0)[0] == rgb.px(2, 1)[0]);
    CHECK(c.px(3, 2)[2] == rgb.px(5, 3)[2]);
    Image clampedc = crop(rgb, 6, 3, 10, 10);
    CHECK(clampedc.width == 2); CHECK(clampedc.height == 2);
}

TEST_CASE("test image is not uniform and hash is stable")
{
    Image a = make_test_image(64, 48, 1);
    Image b = make_test_image(64, 48, 1);
    Image c = make_test_image(64, 48, 2);
    CHECK_FALSE(is_uniform(a));
    CHECK(image_hash(a) == image_hash(b));
    CHECK(image_hash(a) != image_hash(c));
    Image u(4, 4, 3);
    CHECK(is_uniform(u));
}
