#pragma once

#include "image.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace pastiche {

enum class ImageFormat { Unknown, JPEG, PNG, WebP, JXL };

ImageFormat detect_format(const uint8_t* data, size_t size);
const char* format_name(ImageFormat f);

// Decode from memory. Returns "" on success or an error message. JPEG EXIF
// orientation is applied so the result is always upright.
std::string decode_image(const uint8_t* data, size_t size, Image& out);

// Read + decode a file (any supported format, detected by content).
std::string load_image(const std::string& path, Image& out);

// Encoders. Returns "" on success or an error message; a failed write does not
// leave a truncated file behind.
std::string save_png(const std::string& path, const Image& img);
std::string save_jxl(const std::string& path, const Image& img);   // error when built without libjxl

// Chooses the encoder by extension (.png, .jxl).
std::string save_image(const std::string& path, const Image& img);

bool jxl_available();

// "png, jpeg, webp, jxl" etc. for --help / GUI dialogs.
std::string supported_input_formats();
std::string supported_output_formats();

} // namespace pastiche
