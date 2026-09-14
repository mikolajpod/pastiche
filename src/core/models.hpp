#pragma once

// The catalogue of downloadable models (D8, D19): what exists, where it comes
// from, what it hashes to and under what licence. Everything permissively
// licensed and small enough is already in models/ and is not listed here.
#include <cstdint>
#include <string>
#include <vector>

namespace pastiche {

// Work a file needs after downloading before anything can use it. Only one
// operation exists, and it is a workaround for an upstream bug rather than a
// general facility; see D24 and safetensors.hpp.
struct PostProcess {
    std::string op;                   // "" = nothing to do; "prefix_tensors"
    std::string prefix;
    std::string output;               // relative to the models directory
    std::vector<std::string> drop;    // tensor names to leave out

    bool empty() const { return op.empty(); }
};

struct ModelFile {
    std::string path;    // relative to the models directory
    std::string url;
    std::string sha256;  // lowercase hex
    uint64_t bytes = 0;
    PostProcess post;

    // What the algorithms actually open: the post-processed file when there is
    // one, otherwise the download itself.
    const std::string& usable_path() const { return post.empty() ? path : post.output; }
};

struct ModelEntry {
    std::string name;    // what `pastiche download <name>` takes
    std::string description;
    std::string licence;
    std::string licence_url;
    std::string licence_summary;
    bool needs_acceptance = false;   // non-permissive: show the terms and ask first (D8)
    std::vector<ModelFile> files;

    uint64_t total_bytes() const;
};

enum class ModelStatus {
    Missing,     // nothing downloaded
    Partial,     // some files present, or a .part left by an interrupted run
    Installed,   // every file present with the expected size
};

struct ModelCatalogue {
    std::vector<ModelEntry> models;

    const ModelEntry* find(const std::string& name) const;
};

// Loads and validates the catalogue. Returns "" on success, otherwise a message
// naming what is wrong; a catalogue with a missing URL or a malformed checksum
// is rejected outright rather than failing later mid-download.
std::string load_catalogue(const std::string& path, ModelCatalogue& out);

// Where models.json lives: next to the executable, then one level up so a
// development build finds the one in the repository. "" when not found.
std::string find_catalogue_file();

// Size check only; verifying the checksum of several gigabytes is a separate,
// explicit step (verify_model).
ModelStatus model_status(const ModelEntry& entry, const std::string& models_dir);

// Hashes every file of `entry` and compares against the catalogue. Returns ""
// when all match, otherwise the first mismatch. Slow by design.
std::string verify_model(const ModelEntry& entry, const std::string& models_dir);

} // namespace pastiche
