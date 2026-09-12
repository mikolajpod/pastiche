#pragma once

#include <cstddef>
#include <cstdint>

namespace pastiche {

// Parses an EXIF block (the payload of a JPEG APP1 segment, starting with
// "Exif\0\0" or directly with the TIFF header "II*\0" / "MM\0*") and returns
// the Orientation tag (0x0112) value 1..8, or 0 when absent / malformed.
int exif_orientation(const uint8_t* data, size_t size);

// Scans a whole JPEG file buffer for the APP1 Exif segment and returns the
// orientation (1..8) or 0 when not found.
int jpeg_exif_orientation(const uint8_t* jpeg, size_t size);

} // namespace pastiche
