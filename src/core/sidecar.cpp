#include "sidecar.hpp"

#include "fs.hpp"
#include "version.hpp"

namespace pastiche {

std::string sidecar_json(const RunRecord& r)
{
    auto pair = [](int a, int b) { return "[" + std::to_string(a) + ", " + std::to_string(b) + "]"; };
    std::string style = "null";
    if (!r.style_path.empty()) style = json_quote(absolute_path(r.style_path));
    else if (!r.style_name.empty()) style = json_quote(r.style_name);

    std::string j = "{\n";
    j += "  \"app\": \"pastiche\",\n";
    j += "  \"version\": " + json_quote(PASTICHE_VERSION) + ",\n";
    j += "  \"timestamp\": " + json_quote(iso_time_now()) + ",\n";
    j += "  \"algorithm\": " + json_quote(r.algorithm) + ",\n";
    j += "  \"content\": " + (r.content_path.empty() ? std::string("null") : json_quote(absolute_path(r.content_path))) + ",\n";
    j += "  \"style\": " + style + ",\n";
    j += "  \"output\": " + json_quote(absolute_path(r.output_path)) + ",\n";
    j += "  \"content_size\": " + pair(r.content_w, r.content_h) + ",\n";
    j += "  \"processed_size\": " + pair(r.processed_w, r.processed_h) + ",\n";
    j += "  \"output_size\": " + pair(r.output_w, r.output_h) + ",\n";
    j += "  \"size\": " + std::to_string(r.size) + ",\n";
    j += "  \"tile\": " + std::string(r.tile ? "true" : "false") + ",\n";
    j += "  \"backend\": " + (r.backend.empty() ? std::string("null") : json_quote(r.backend)) + ",\n";
    j += "  \"params\": " + params_to_json(r.specs, r.params) + ",\n";
    j += "  \"time_seconds\": " + json_number(r.time_seconds) + "\n";
    j += "}\n";
    return j;
}

std::string write_sidecar(const std::string& json_path, const RunRecord& r)
{
    return write_text_file(json_path, sidecar_json(r));
}

} // namespace pastiche
