#pragma once

#include "image.hpp"

#include <vector>

namespace pastiche {

struct TileRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// Splits a W x H image into tiles of at most `tile` px on each side that
// overlap by `overlap` px. Returns a single full-size tile when the image fits.
std::vector<TileRect> make_tiles(int W, int H, int tile, int overlap);

// Blend weight (0..1) at position i of an n-long tile edge: linear ramp over
// the overlap at the leading edge when a neighbour exists there, and at the
// trailing edge likewise; 1 elsewhere.
float tile_ramp(int i, int n, bool has_before, bool has_after, int overlap);

// Accumulates processed RGB tiles with soft edges and produces the final image.
class TileBlender {
public:
    TileBlender(int W, int H, int overlap);
    // piece must be RGB (3 channels) of size r.w x r.h.
    void add(const Image& piece, const TileRect& r);
    Image finish() const;

private:
    int W_, H_, overlap_;
    std::vector<float> acc_;
    std::vector<float> wsum_;
};

// Extends the image at the right and bottom edges (replicating the last
// row/column) so both dimensions are multiples of m.
Image pad_to_multiple(const Image& img, int m);

} // namespace pastiche
