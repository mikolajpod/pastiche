#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace pastiche {

// 8-bit sRGB image, interleaved, channels == 3 (RGB) or 4 (RGBA).
// Row stride is always width * channels (no padding).
struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> data;

    Image() = default;
    Image(int w, int h, int c) : width(w), height(h), channels(c),
        data(static_cast<size_t>(w) * h * c, 0) {}

    bool empty() const { return width <= 0 || height <= 0 || data.empty(); }
    size_t row_bytes() const { return static_cast<size_t>(width) * channels; }
    uint8_t* row(int y) { return data.data() + static_cast<size_t>(y) * row_bytes(); }
    const uint8_t* row(int y) const { return data.data() + static_cast<size_t>(y) * row_bytes(); }
    uint8_t* px(int x, int y) { return row(y) + static_cast<size_t>(x) * channels; }
    const uint8_t* px(int x, int y) const { return row(y) + static_cast<size_t>(x) * channels; }
};

// Channel conversions (return a copy; plain copy when already in target layout).
Image to_rgb(const Image& img);
Image to_rgba(const Image& img);

// Resize with area averaging when shrinking and bilinear when enlarging.
Image resize(const Image& img, int new_w, int new_h);

// Scale so that the longer side equals `longest`. Returns a copy if the image
// is already smaller or equal and `upscale` is false. Aspect ratio preserved.
Image fit_longest_side(const Image& img, int longest, bool upscale = false);

// EXIF orientation 1..8 (TIFF/EXIF convention). 1 = identity.
Image apply_orientation(const Image& img, int orientation);

// Crop a rectangle (clamped to the image bounds).
Image crop(const Image& img, int x, int y, int w, int h);

// Planar float conversion for ML: CHW layout, RGB only (alpha dropped),
// values multiplied by `scale` (e.g. 1/255 or 1.0). Output size = 3 * w * h.
void image_to_chw(const Image& img, std::vector<float>& out, float scale);
// Inverse: CHW float -> RGB image, values multiplied by `scale`, clamped to 0..255.
// NaN values become 0.
Image chw_to_image(const float* chw, int w, int h, float scale);

// Synthetic test image (smooth gradients, a disc and a checker patch), RGB.
Image make_test_image(int w, int h, uint32_t seed = 1);

// FNV-1a over dimensions and pixel data (used by --selftest for reference checks).
uint64_t image_hash(const Image& img);

// True when every pixel has the same value (e.g. all black).
bool is_uniform(const Image& img);

} // namespace pastiche
