#pragma once

// Just enough safetensors handling to rename the tensors of a downloaded file.
//
// A safetensors file is an 8-byte little-endian header length, that many bytes
// of JSON describing each tensor (dtype, shape, and its offsets *within the
// data buffer*), then one packed buffer. Because the offsets are relative to
// the buffer and not to the file, renaming tensors touches the header only and
// the payload can be streamed through untouched.
//
// This exists for one reason, documented in D24: stable-diffusion.cpp discards
// every tensor whose name begins with "vision_model." while loading any file,
// which silently empties the IP-Adapter CLIP-Vision encoder. Prefixing the
// names sidesteps the filter and lands them exactly where the loader expects.
#include <string>
#include <vector>

namespace pastiche {

// Copies `src_path` to `dst_path`, prefixing every tensor name with `prefix`
// and dropping any tensor named in `drop`. Returns "" on success, otherwise a
// message describing what was wrong with the file.
//
// Names that already start with `prefix` are left alone, so running this twice
// is harmless.
std::string safetensors_prefix_tensors(const std::string& src_path, const std::string& dst_path,
                                       const std::string& prefix,
                                       const std::vector<std::string>& drop = {});

} // namespace pastiche
