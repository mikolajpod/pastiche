#include "models.hpp"

#include "fs.hpp"
#include "json.hpp"
#include "sha256.hpp"

#include <cctype>

namespace pastiche {

namespace {

bool is_hex64(const std::string& s)
{
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string to_lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Keeps a hand-edited catalogue from pointing anywhere outside the models
// directory. Downloads write wherever "path" says, so ".." there would be a way
// to overwrite arbitrary files.
bool is_safe_relative_path(const std::string& p)
{
    if (p.empty()) return false;
    if (p.front() == '/' || p.front() == '\\') return false;
    if (p.size() >= 2 && p[1] == ':') return false;   // drive letter
    std::size_t start = 0;
    while (start <= p.size()) {
        const std::size_t sep = p.find_first_of("/\\", start);
        const std::string part = p.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (part == "..") return false;
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return true;
}

} // namespace

uint64_t ModelEntry::total_bytes() const
{
    uint64_t sum = 0;
    for (const ModelFile& f : files) sum += f.bytes;
    return sum;
}

const ModelEntry* ModelCatalogue::find(const std::string& name) const
{
    const std::string wanted = to_lower(name);
    for (const ModelEntry& m : models) {
        if (to_lower(m.name) == wanted) return &m;
    }
    return nullptr;
}

std::string load_catalogue(const std::string& path, ModelCatalogue& out)
{
    out.models.clear();

    std::vector<uint8_t> bytes;
    const std::string read_error = read_file(path, bytes);
    if (!read_error.empty()) return read_error;

    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string json_error;
    const JsonValue root = json_parse(text, json_error);
    if (!json_error.empty()) return path + ": " + json_error;
    if (!root.is_object()) return path + ": the top level must be an object";

    const JsonValue* models = root.find("models");
    if (!models || !models->is_array()) return path + ": missing \"models\" array";

    for (const JsonValue& item : models->items()) {
        if (!item.is_object()) return path + ": every entry of \"models\" must be an object";

        ModelEntry entry;
        entry.name = item.string_or("name");
        entry.description = item.string_or("description");
        entry.licence = item.string_or("licence");
        entry.licence_url = item.string_or("licence_url");
        entry.licence_summary = item.string_or("licence_summary");
        entry.needs_acceptance = item.bool_or("needs_acceptance");

        if (entry.name.empty()) return path + ": a model entry has no \"name\"";
        if (out.find(entry.name)) return path + ": duplicate model name '" + entry.name + "'";

        const JsonValue* files = item.find("files");
        if (!files || !files->is_array() || files->items().empty()) {
            return path + ": model '" + entry.name + "' has no \"files\"";
        }

        for (const JsonValue& f : files->items()) {
            if (!f.is_object()) return path + ": model '" + entry.name + "' has a malformed file entry";
            ModelFile file;
            file.path = f.string_or("path");
            file.url = f.string_or("url");
            file.sha256 = to_lower(f.string_or("sha256"));
            file.bytes = static_cast<uint64_t>(f.int_or("bytes"));

            const std::string where = path + ": model '" + entry.name + "', file '" + file.path + "'";
            if (file.path.empty()) return path + ": model '" + entry.name + "' has a file with no \"path\"";
            if (!is_safe_relative_path(file.path)) return where + ": path must stay inside the models directory";
            if (file.url.empty()) return where + ": missing \"url\"";
            if (file.url.rfind("https://", 0) != 0) return where + ": url must be https";
            if (!is_hex64(file.sha256)) return where + ": \"sha256\" must be 64 hex characters";
            if (file.bytes == 0) return where + ": missing or zero \"bytes\"";

            entry.files.push_back(std::move(file));
        }

        if (entry.needs_acceptance && entry.licence_summary.empty()) {
            return path + ": model '" + entry.name +
                   "' needs acceptance but has no \"licence_summary\" to show";
        }

        out.models.push_back(std::move(entry));
    }

    if (out.models.empty()) return path + ": the catalogue lists no models";
    return {};
}

std::string find_catalogue_file()
{
    const std::string exe = exe_dir();
    const std::string beside = path_join(exe, "models.json");
    if (file_exists(beside)) return beside;
    // Development builds live in build/, one level below the repository root.
    const std::string above = path_join(path_dirname(exe), "models.json");
    if (file_exists(above)) return above;
    return {};
}

ModelStatus model_status(const ModelEntry& entry, const std::string& models_dir)
{
    std::size_t present = 0;
    bool partial = false;
    for (const ModelFile& f : entry.files) {
        const std::string full = path_join(models_dir, f.path);
        if (file_exists(full) && file_size(full) == f.bytes) {
            ++present;
        } else if (file_exists(full) || file_exists(full + ".part")) {
            partial = true;
        }
    }
    if (present == entry.files.size()) return ModelStatus::Installed;
    if (present > 0 || partial) return ModelStatus::Partial;
    return ModelStatus::Missing;
}

std::string verify_model(const ModelEntry& entry, const std::string& models_dir)
{
    for (const ModelFile& f : entry.files) {
        const std::string full = path_join(models_dir, f.path);
        if (!file_exists(full)) return "missing: " + f.path;
        std::string error;
        const std::string digest = sha256_file(full, error);
        if (!error.empty()) return error;
        if (digest != f.sha256) {
            return "checksum mismatch for " + f.path + "\n  expected " + f.sha256 + "\n  got      " + digest;
        }
    }
    return {};
}

} // namespace pastiche
