#include "image.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pastiche {

Image to_rgb(const Image& img)
{
    if (img.channels == 3) return img;
    Image out(img.width, img.height, 3);
    const size_t n = static_cast<size_t>(img.width) * img.height;
    const uint8_t* s = img.data.data();
    uint8_t* d = out.data.data();
    for (size_t i = 0; i < n; ++i) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
        s += 4; d += 3;
    }
    return out;
}

Image to_rgba(const Image& img)
{
    if (img.channels == 4) return img;
    Image out(img.width, img.height, 4);
    const size_t n = static_cast<size_t>(img.width) * img.height;
    const uint8_t* s = img.data.data();
    uint8_t* d = out.data.data();
    for (size_t i = 0; i < n; ++i) {
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        s += 3; d += 4;
    }
    return out;
}

namespace {

// Exact area averaging, separable: horizontal pass into a float buffer,
// then vertical pass into the 8-bit output.
Image area_downscale(const Image& img, int new_w, int new_h)
{
    const int c = img.channels;
    std::vector<float> tmp(static_cast<size_t>(new_w) * img.height * c, 0.f);
    const double sx = static_cast<double>(img.width) / new_w;
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* srow = img.row(y);
        float* drow = tmp.data() + static_cast<size_t>(y) * new_w * c;
        for (int dx = 0; dx < new_w; ++dx) {
            const double x0 = dx * sx, x1 = (dx + 1) * sx;
            const int ix0 = static_cast<int>(std::floor(x0));
            const int ix1 = std::min(static_cast<int>(std::ceil(x1)), img.width);
            float acc[4] = {0, 0, 0, 0};
            double wsum = 0;
            for (int ix = ix0; ix < ix1; ++ix) {
                const double w = std::min<double>(x1, ix + 1) - std::max<double>(x0, ix);
                if (w <= 0) continue;
                for (int k = 0; k < c; ++k) acc[k] += static_cast<float>(w) * srow[ix * c + k];
                wsum += w;
            }
            const float inv = wsum > 0 ? static_cast<float>(1.0 / wsum) : 0.f;
            for (int k = 0; k < c; ++k) drow[dx * c + k] = acc[k] * inv;
        }
    }
    Image out(new_w, new_h, c);
    const double sy = static_cast<double>(img.height) / new_h;
    for (int dy = 0; dy < new_h; ++dy) {
        const double y0 = dy * sy, y1 = (dy + 1) * sy;
        const int iy0 = static_cast<int>(std::floor(y0));
        const int iy1 = std::min(static_cast<int>(std::ceil(y1)), img.height);
        uint8_t* drow = out.row(dy);
        for (int x = 0; x < new_w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            double wsum = 0;
            for (int iy = iy0; iy < iy1; ++iy) {
                const double w = std::min<double>(y1, iy + 1) - std::max<double>(y0, iy);
                if (w <= 0) continue;
                const float* srow = tmp.data() + static_cast<size_t>(iy) * new_w * c;
                for (int k = 0; k < c; ++k) acc[k] += static_cast<float>(w) * srow[x * c + k];
                wsum += w;
            }
            const float inv = wsum > 0 ? static_cast<float>(1.0 / wsum) : 0.f;
            for (int k = 0; k < c; ++k) {
                const float v = acc[k] * inv + 0.5f;
                drow[x * c + k] = static_cast<uint8_t>(std::clamp(v, 0.f, 255.f));
            }
        }
    }
    return out;
}

Image bilinear_scale(const Image& img, int new_w, int new_h)
{
    const int c = img.channels;
    Image out(new_w, new_h, c);
    const double sx = static_cast<double>(img.width) / new_w;
    const double sy = static_cast<double>(img.height) / new_h;
    for (int dy = 0; dy < new_h; ++dy) {
        double fy = (dy + 0.5) * sy - 0.5;
        fy = std::clamp(fy, 0.0, static_cast<double>(img.height - 1));
        const int y0 = static_cast<int>(fy);
        const int y1 = std::min(y0 + 1, img.height - 1);
        const float wy = static_cast<float>(fy - y0);
        uint8_t* drow = out.row(dy);
        for (int dx = 0; dx < new_w; ++dx) {
            double fx = (dx + 0.5) * sx - 0.5;
            fx = std::clamp(fx, 0.0, static_cast<double>(img.width - 1));
            const int x0 = static_cast<int>(fx);
            const int x1 = std::min(x0 + 1, img.width - 1);
            const float wx = static_cast<float>(fx - x0);
            const uint8_t* p00 = img.px(x0, y0);
            const uint8_t* p10 = img.px(x1, y0);
            const uint8_t* p01 = img.px(x0, y1);
            const uint8_t* p11 = img.px(x1, y1);
            for (int k = 0; k < c; ++k) {
                const float top = p00[k] + (p10[k] - p00[k]) * wx;
                const float bot = p01[k] + (p11[k] - p01[k]) * wx;
                const float v = top + (bot - top) * wy + 0.5f;
                drow[dx * c + k] = static_cast<uint8_t>(std::clamp(v, 0.f, 255.f));
            }
        }
    }
    return out;
}

} // namespace

Image resize(const Image& img, int new_w, int new_h)
{
    new_w = std::max(1, new_w);
    new_h = std::max(1, new_h);
    if (img.empty() || (new_w == img.width && new_h == img.height)) return img;
    if (new_w <= img.width && new_h <= img.height) return area_downscale(img, new_w, new_h);
    if (new_w >= img.width && new_h >= img.height) return bilinear_scale(img, new_w, new_h);
    // Mixed: shrink the smaller axis first, then enlarge the other.
    Image mid = area_downscale(img, std::min(new_w, img.width), std::min(new_h, img.height));
    return bilinear_scale(mid, new_w, new_h);
}

Image fit_longest_side(const Image& img, int longest, bool upscale)
{
    if (longest <= 0 || img.empty()) return img;
    const int cur = std::max(img.width, img.height);
    if (cur == longest || (cur < longest && !upscale)) return img;
    const double s = static_cast<double>(longest) / cur;
    const int nw = std::max(1, static_cast<int>(std::lround(img.width * s)));
    const int nh = std::max(1, static_cast<int>(std::lround(img.height * s)));
    return resize(img, nw, nh);
}

Image apply_orientation(const Image& img, int orientation)
{
    if (orientation <= 1 || orientation > 8 || img.empty()) return img;
    const int c = img.channels;
    const bool transpose = orientation >= 5;
    const int ow = transpose ? img.height : img.width;
    const int oh = transpose ? img.width : img.height;
    Image out(ow, oh, c);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            int dx = x, dy = y;
            switch (orientation) {
                case 2: dx = img.width - 1 - x; dy = y; break;                   // mirror horizontal
                case 3: dx = img.width - 1 - x; dy = img.height - 1 - y; break;  // rotate 180
                case 4: dx = x; dy = img.height - 1 - y; break;                  // mirror vertical
                case 5: dx = y; dy = x; break;                                   // transpose
                case 6: dx = img.height - 1 - y; dy = x; break;                  // rotate 90 CW
                case 7: dx = img.height - 1 - y; dy = img.width - 1 - x; break;  // transverse
                case 8: dx = y; dy = img.width - 1 - x; break;                   // rotate 270 CW
                default: break;
            }
            std::memcpy(out.px(dx, dy), img.px(x, y), static_cast<size_t>(c));
        }
    }
    return out;
}

Image crop(const Image& img, int x, int y, int w, int h)
{
    x = std::clamp(x, 0, std::max(0, img.width - 1));
    y = std::clamp(y, 0, std::max(0, img.height - 1));
    w = std::clamp(w, 1, img.width - x);
    h = std::clamp(h, 1, img.height - y);
    Image out(w, h, img.channels);
    for (int r = 0; r < h; ++r)
        std::memcpy(out.row(r), img.px(x, y + r), out.row_bytes());
    return out;
}

void image_to_chw(const Image& img, std::vector<float>& out, float scale)
{
    const size_t plane = static_cast<size_t>(img.width) * img.height;
    out.resize(plane * 3);
    const int c = img.channels;
    const uint8_t* s = img.data.data();
    float* r = out.data();
    float* g = r + plane;
    float* b = g + plane;
    for (size_t i = 0; i < plane; ++i, s += c) {
        r[i] = s[0] * scale;
        g[i] = s[1] * scale;
        b[i] = s[2] * scale;
    }
}

Image chw_to_image(const float* chw, int w, int h, float scale)
{
    Image out(w, h, 3);
    const size_t plane = static_cast<size_t>(w) * h;
    uint8_t* d = out.data.data();
    for (size_t i = 0; i < plane; ++i) {
        for (int k = 0; k < 3; ++k) {
            float v = chw[k * plane + i] * scale;
            if (std::isnan(v)) v = 0.f;
            v = std::clamp(v + 0.5f, 0.f, 255.f);
            d[i * 3 + k] = static_cast<uint8_t>(v);
        }
    }
    return out;
}

Image make_test_image(int w, int h, uint32_t seed)
{
    Image img(w, h, 3);
    uint32_t s = seed * 2654435761u + 12345u;
    auto rnd = [&s]() { s = s * 1664525u + 1013904223u; return (s >> 8) & 0xFF; };
    const int cx = w / 3 + static_cast<int>(rnd()) % std::max(1, w / 3);
    const int cy = h / 3 + static_cast<int>(rnd()) % std::max(1, h / 3);
    const int rad = std::max(4, std::min(w, h) / 4);
    const float phase = static_cast<float>(rnd()) / 255.f * 6.28f;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = img.row(y);
        for (int x = 0; x < w; ++x) {
            const float fx = static_cast<float>(x) / std::max(1, w - 1);
            const float fy = static_cast<float>(y) / std::max(1, h - 1);
            float r = 255.f * fx;
            float g = 255.f * fy;
            float b = 255.f * (0.5f + 0.5f * std::sin(fx * 12.f + fy * 7.f + phase));
            const int ddx = x - cx, ddy = y - cy;
            if (ddx * ddx + ddy * ddy < rad * rad) { r = 255.f - r; g = 40.f; b = 255.f - b; }
            if (((x / 16) + (y / 16)) % 7 == 0 && x > w / 2 && y > h / 2) { r *= 0.3f; g *= 0.3f; b *= 0.3f; }
            row[x * 3 + 0] = static_cast<uint8_t>(std::clamp(r, 0.f, 255.f));
            row[x * 3 + 1] = static_cast<uint8_t>(std::clamp(g, 0.f, 255.f));
            row[x * 3 + 2] = static_cast<uint8_t>(std::clamp(b, 0.f, 255.f));
        }
    }
    return img;
}

uint64_t image_hash(const Image& img)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(static_cast<uint64_t>(img.width));
    mix(static_cast<uint64_t>(img.height));
    mix(static_cast<uint64_t>(img.channels));
    for (uint8_t b : img.data) mix(b);
    return h;
}

bool is_uniform(const Image& img)
{
    if (img.empty()) return true;
    const int c = img.channels;
    const uint8_t* first = img.data.data();
    const size_t n = static_cast<size_t>(img.width) * img.height;
    for (size_t i = 1; i < n; ++i)
        if (std::memcmp(first, first + i * c, static_cast<size_t>(c)) != 0) return false;
    return true;
}

} // namespace pastiche
