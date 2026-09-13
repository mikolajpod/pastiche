#pragma once

#include "params.hpp"

#include <string>
#include <vector>

namespace pastiche {

// Everything the .json written next to a result needs to know (D4): what ran,
// on which inputs, with which parameters, producing what.
struct RunRecord {
    std::string algorithm;
    std::string content_path;
    std::string style_path;      // style image path (StyleInput::Image)
    std::string style_name;      // preset name (StyleInput::Preset)
    std::string output_path;
    std::string backend;         // "" when not applicable
    int content_w = 0, content_h = 0;        // as loaded
    int processed_w = 0, processed_h = 0;    // after --size
    int output_w = 0, output_h = 0;
    int size = 0;
    bool tile = false;
    double time_seconds = 0;
    std::vector<ParamSpec> specs;
    Params params;
};

std::string sidecar_json(const RunRecord& r);
// Writes sidecar_json(r) to json_path. Returns "" or an error message.
std::string write_sidecar(const std::string& json_path, const RunRecord& r);

} // namespace pastiche
