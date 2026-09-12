#include "tiling.hpp"

#include <algorithm>
#include <cstring>

namespace pastiche {

std::vector<TileRect> make_tiles(int W, int H, int tile, int overlap)
{
    std::vector<TileRect> out;
    if (W <= tile && H <= tile) { out.push_back({0, 0, W, H}); return out; }
    const int step = std::max(1, tile - overlap);
    std::vector<int> xs, ys;
    for (int x = 0;; x += step) { xs.push_back(std::min(x, std::max(0, W - tile))); if (x + tile >= W) break; }
    for (int y = 0;; y += step) { ys.push_back(std::min(y, std::max(0, H - tile))); if (y + tile >= H) break; }
    for (int y0 : ys)
        for (int x0 : xs)
            out.push_back({x0, y0, std::min(tile, W - x0), std::min(tile, H - y0)});
    return out;
}

float tile_ramp(int i, int n, bool has_before, bool has_after, int overlap)
{
    float w = 1.f;
    const int ov = std::min(overlap, n);
    if (has_before && i < ov) w = std::min(w, (i + 1.f) / (ov + 1.f));
    if (has_after && i >= n - ov) w = std::min(w, (n - i) / (ov + 1.f));
    return w;
}

TileBlender::TileBlender(int W, int H, int overlap)
    : W_(W), H_(H), overlap_(overlap),
      acc_(static_cast<size_t>(W) * H * 3, 0.f), wsum_(static_cast<size_t>(W) * H, 0.f)
{}

void TileBlender::add(const Image& piece, const TileRect& r)
{
    for (int y = 0; y < r.h; ++y) {
        const float wy = tile_ramp(y, r.h, r.y > 0, r.y + r.h < H_, overlap_);
        for (int x = 0; x < r.w; ++x) {
            const float w = wy * tile_ramp(x, r.w, r.x > 0, r.x + r.w < W_, overlap_);
            const size_t idx = static_cast<size_t>(r.y + y) * W_ + (r.x + x);
            const uint8_t* s = piece.px(x, y);
            acc_[idx * 3 + 0] += w * s[0];
            acc_[idx * 3 + 1] += w * s[1];
            acc_[idx * 3 + 2] += w * s[2];
            wsum_[idx] += w;
        }
    }
}

Image TileBlender::finish() const
{
    Image out(W_, H_, 3);
    for (size_t i = 0; i < wsum_.size(); ++i) {
        const float inv = wsum_[i] > 0 ? 1.f / wsum_[i] : 0.f;
        for (int k = 0; k < 3; ++k)
            out.data[i * 3 + k] = static_cast<uint8_t>(std::clamp(acc_[i * 3 + k] * inv + 0.5f, 0.f, 255.f));
    }
    return out;
}

Image pad_to_multiple(const Image& img, int m)
{
    const int pw = (img.width + m - 1) / m * m;
    const int ph = (img.height + m - 1) / m * m;
    if (pw == img.width && ph == img.height) return img;
    Image out(pw, ph, img.channels);
    for (int y = 0; y < ph; ++y) {
        const int sy = std::min(y, img.height - 1);
        std::memcpy(out.row(y), img.row(sy), img.row_bytes());
        const uint8_t* last = img.px(img.width - 1, sy);
        for (int x = img.width; x < pw; ++x) std::memcpy(out.px(x, y), last, static_cast<size_t>(img.channels));
    }
    return out;
}

} // namespace pastiche
