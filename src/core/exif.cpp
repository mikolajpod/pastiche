#include "exif.hpp"

#include <cstring>

namespace pastiche {

namespace {

uint16_t rd16(const uint8_t* p, bool le) { return le ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t rd32(const uint8_t* p, bool le)
{
    return le ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
              : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]));
}

} // namespace

int exif_orientation(const uint8_t* data, size_t size)
{
    if (!data) return 0;
    if (size >= 6 && std::memcmp(data, "Exif\0\0", 6) == 0) { data += 6; size -= 6; }
    if (size < 8) return 0;
    bool le;
    if (data[0] == 'I' && data[1] == 'I') le = true;
    else if (data[0] == 'M' && data[1] == 'M') le = false;
    else return 0;
    if (rd16(data + 2, le) != 42) return 0;
    const uint32_t ifd0 = rd32(data + 4, le);
    if (ifd0 < 8 || ifd0 + 2 > size) return 0;
    const uint16_t count = rd16(data + ifd0, le);
    size_t off = ifd0 + 2;
    for (uint16_t i = 0; i < count; ++i, off += 12) {
        if (off + 12 > size) return 0;
        const uint8_t* e = data + off;
        const uint16_t tag = rd16(e, le);
        if (tag != 0x0112) continue;
        const uint16_t type = rd16(e + 2, le);
        const uint32_t n = rd32(e + 4, le);
        if (n != 1) return 0;
        int v = 0;
        if (type == 3) v = rd16(e + 8, le);          // SHORT, stored inline
        else if (type == 4) v = static_cast<int>(rd32(e + 8, le));  // LONG (non-standard but seen)
        else return 0;
        return (v >= 1 && v <= 8) ? v : 0;
    }
    return 0;
}

int jpeg_exif_orientation(const uint8_t* jpeg, size_t size)
{
    if (!jpeg || size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return 0;
    size_t pos = 2;
    while (pos + 4 <= size) {
        if (jpeg[pos] != 0xFF) return 0;
        const uint8_t marker = jpeg[pos + 1];
        if (marker == 0xFF) { ++pos; continue; }          // fill byte
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) { pos += 2; continue; }  // standalone
        if (marker == 0xDA || marker == 0xD9) return 0;   // SOS / EOI: no more headers
        const size_t len = (static_cast<size_t>(jpeg[pos + 2]) << 8) | jpeg[pos + 3];
        if (len < 2 || pos + 2 + len > size) return 0;
        if (marker == 0xE1) {
            const uint8_t* payload = jpeg + pos + 4;
            const size_t plen = len - 2;
            if (plen >= 6 && std::memcmp(payload, "Exif\0\0", 6) == 0)
                return exif_orientation(payload, plen);
        }
        pos += 2 + len;
    }
    return 0;
}

} // namespace pastiche
