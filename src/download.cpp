#include "download.hpp"

#include "core/fs.hpp"
#include "core/http.hpp"
#include "core/models.hpp"
#include "core/safetensors.hpp"
#include "core/sha256.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace pastiche {

namespace {

std::string human_size(uint64_t bytes)
{
    char buf[64];
    if (bytes >= (1ull << 30)) {
        std::snprintf(buf, sizeof buf, "%.1f GiB", static_cast<double>(bytes) / (1ull << 30));
    } else if (bytes >= (1ull << 20)) {
        std::snprintf(buf, sizeof buf, "%.1f MiB", static_cast<double>(bytes) / (1ull << 20));
    } else {
        std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buf;
}

const char* status_word(ModelStatus s)
{
    switch (s) {
    case ModelStatus::Installed: return "installed";
    case ModelStatus::Partial:   return "partial";
    default:                     return "not downloaded";
    }
}

// Prints a single progress line, rewritten in place. Rate-limited so a fast
// link does not spend its time on console writes.
class ConsoleDownloadProgress final : public HttpProgress {
public:
    explicit ConsoleDownloadProgress(std::string label) : label_(std::move(label)) {}

    bool on_progress(uint64_t received, uint64_t total) override
    {
        const auto now = std::chrono::steady_clock::now();
        if (last_.time_since_epoch().count() != 0 &&
            now - last_ < std::chrono::milliseconds(200) && received != total) {
            return true;
        }
        if (start_.time_since_epoch().count() == 0) {
            start_ = now;
            start_bytes_ = received;
        }
        last_ = now;

        const double seconds =
            std::chrono::duration_cast<std::chrono::duration<double>>(now - start_).count();
        const double rate = seconds > 0.5 ? static_cast<double>(received - start_bytes_) / seconds : 0.0;

        char rate_text[32] = "";
        if (rate > 0) {
            std::snprintf(rate_text, sizeof rate_text, "  %.1f MiB/s", rate / (1024 * 1024));
        }

        if (total > 0) {
            const int pct = static_cast<int>(100.0 * static_cast<double>(received) /
                                             static_cast<double>(total));
            std::fprintf(stderr, "\r  %-28s %3d%%  %s of %s%s   ", label_.c_str(), pct,
                         human_size(received).c_str(), human_size(total).c_str(), rate_text);
        } else {
            std::fprintf(stderr, "\r  %-28s %s%s   ", label_.c_str(),
                         human_size(received).c_str(), rate_text);
        }
        std::fflush(stderr);
        return true;
    }

    void finish() { std::fprintf(stderr, "\n"); }

private:
    std::string label_;
    std::chrono::steady_clock::time_point start_{};
    std::chrono::steady_clock::time_point last_{};
    uint64_t start_bytes_ = 0;
};

// Wraps the summary at a readable width; the OpenRAIL-M summary is a paragraph
// and an unwrapped one is exactly the sort of thing nobody reads.
void print_wrapped(const std::string& text, std::size_t width, const char* indent)
{
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = std::min(pos + width, text.size());
        if (end < text.size()) {
            const std::size_t space = text.rfind(' ', end);
            if (space != std::string::npos && space > pos) end = space;
        }
        std::printf("%s%s\n", indent, text.substr(pos, end - pos).c_str());
        pos = end;
        while (pos < text.size() && text[pos] == ' ') ++pos;
    }
}

// Shows the licence and asks. D8 requires this for anything whose terms are not
// plainly permissive; --yes records that the caller accepted them anyway, which
// is what a scripted install needs.
bool confirm_licence(const ModelEntry& entry, bool assume_yes)
{
    if (!entry.needs_acceptance) return true;

    std::printf("\n%s is covered by the %s licence.\n\n", entry.name.c_str(), entry.licence.c_str());
    print_wrapped(entry.licence_summary, 74, "  ");
    if (!entry.licence_url.empty()) {
        std::printf("\n  Full text: %s\n", entry.licence_url.c_str());
    }
    std::printf("\nThis is a summary, not the licence. Read the full text before you rely on it.\n");

    if (assume_yes) {
        std::printf("Accepted via --yes.\n\n");
        return true;
    }

    std::printf("Do you accept these terms? [y/N] ");
    std::fflush(stdout);

    char answer[16] = {};
    if (!std::fgets(answer, sizeof answer, stdin)) {
        std::printf("\nNo answer, nothing downloaded.\n");
        return false;
    }
    const bool yes = answer[0] == 'y' || answer[0] == 'Y';
    if (!yes) std::printf("Declined, nothing downloaded.\n");
    std::printf("\n");
    return yes;
}

int list_models(const ModelCatalogue& catalogue, const std::string& models_dir)
{
    std::printf("Models directory: %s\n\n", models_dir.c_str());
    for (const ModelEntry& m : catalogue.models) {
        const ModelStatus status = model_status(m, models_dir);
        std::printf("%-16s %-16s %9s  %s\n", m.name.c_str(), status_word(status),
                    human_size(m.total_bytes()).c_str(), m.licence.c_str());
        std::printf("%-16s %s\n", "", m.description.c_str());
        if (m.needs_acceptance) {
            std::printf("%-16s requires accepting the licence before download\n", "");
        }
        std::printf("\n");
    }
    std::printf("Download with: pastiche download <name>\n");
    return 0;
}

int verify_models(const ModelCatalogue& catalogue, const std::string& models_dir,
                  const std::vector<std::string>& names)
{
    int failures = 0;
    for (const ModelEntry& m : catalogue.models) {
        if (!names.empty()) {
            bool wanted = false;
            for (const std::string& n : names) {
                if (catalogue.find(n) == &m) { wanted = true; break; }
            }
            if (!wanted) continue;
        }
        if (model_status(m, models_dir) == ModelStatus::Missing) {
            std::printf("%-16s not downloaded, skipped\n", m.name.c_str());
            continue;
        }
        std::printf("%-16s hashing %s...\n", m.name.c_str(), human_size(m.total_bytes()).c_str());
        const std::string error = verify_model(m, models_dir);
        if (error.empty()) {
            std::printf("%-16s OK\n", m.name.c_str());
        } else {
            std::fprintf(stderr, "%-16s FAILED: %s\n", m.name.c_str(), error.c_str());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

// Runs a file's postprocess step if it has one and the result is not already
// there. Prints its own errors; returns false when the caller should give up.
bool run_postprocess(const ModelFile& file, const std::string& models_dir)
{
    if (file.post.empty()) return true;

    const std::string output = path_join(models_dir, file.post.output);
    if (file_exists(output)) return true;

    std::printf("  %-28s preparing %s...\n", file.path.c_str(), file.post.output.c_str());
    const std::string error = safetensors_prefix_tensors(path_join(models_dir, file.path), output,
                                                         file.post.prefix, file.post.drop);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }
    std::printf("  %-28s OK\n", file.post.output.c_str());
    return true;
}

int download_one(const ModelEntry& entry, const std::string& models_dir, bool assume_yes)
{
    if (model_status(entry, models_dir) == ModelStatus::Installed) {
        std::printf("%s is already downloaded. Use 'pastiche download --verify %s' to check it.\n",
                    entry.name.c_str(), entry.name.c_str());
        return 0;
    }

    if (!confirm_licence(entry, assume_yes)) return 1;

    std::printf("Downloading %s (%s) to %s\n", entry.name.c_str(),
                human_size(entry.total_bytes()).c_str(), models_dir.c_str());

    for (const ModelFile& file : entry.files) {
        const std::string dest = path_join(models_dir, file.path);

        if (file_exists(dest) && file_size(dest) == file.bytes) {
            std::printf("  %-28s already present\n", file.path.c_str());
            if (!run_postprocess(file, models_dir)) return 1;
            continue;
        }

        ConsoleDownloadProgress progress(path_basename(file.path));
        const HttpResult result = http_download(file.url, dest, &progress);
        progress.finish();

        if (result.cancelled) {
            std::fprintf(stderr, "Cancelled. The partial file is kept, run the same command to resume.\n");
            return 1;
        }
        if (!result.ok) {
            std::fprintf(stderr, "error: %s\n", result.error.c_str());
            return 1;
        }

        std::printf("  %-28s verifying checksum...\n", file.path.c_str());
        std::string hash_error;
        const std::string digest = sha256_file(dest, hash_error);
        if (!hash_error.empty()) {
            std::fprintf(stderr, "error: %s\n", hash_error.c_str());
            return 1;
        }
        if (digest != file.sha256) {
            std::fprintf(stderr,
                         "error: checksum mismatch for %s\n  expected %s\n  got      %s\n"
                         "The file was NOT what the catalogue describes; it has been left in place "
                         "for inspection but must not be trusted.\n",
                         file.path.c_str(), file.sha256.c_str(), digest.c_str());
            return 1;
        }
        std::printf("  %-28s OK\n", file.path.c_str());
        if (!run_postprocess(file, models_dir)) return 1;
    }

    std::printf("%s downloaded.\n", entry.name.c_str());
    return 0;
}

} // namespace

void print_download_usage()
{
    std::printf(
        "Usage:\n"
        "  pastiche download --list             what is available and what is already here\n"
        "  pastiche download <name>...          download one or more models\n"
        "  pastiche download --verify [<name>]  re-check checksums of what is downloaded\n"
        "\n"
        "Options:\n"
        "  --models-dir D   where to put them (default: models/ next to the executable)\n"
        "  --yes            accept model licences without the interactive prompt\n");
}

int run_download(const std::vector<std::string>& args, const std::string& models_dir,
                 bool assume_yes)
{
    const std::string catalogue_path = find_catalogue_file();
    if (catalogue_path.empty()) {
        std::fprintf(stderr, "error: models.json not found next to the executable\n");
        return 1;
    }

    ModelCatalogue catalogue;
    const std::string error = load_catalogue(catalogue_path, catalogue);
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    bool verify = false;
    std::vector<std::string> names;
    for (const std::string& a : args) {
        if (a == "--list") return list_models(catalogue, models_dir);
        if (a == "--verify") { verify = true; continue; }
        if (a == "--help" || a == "-h") { print_download_usage(); return 0; }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
            print_download_usage();
            return 2;
        }
        names.push_back(a);
    }

    for (const std::string& n : names) {
        if (!catalogue.find(n)) {
            std::fprintf(stderr, "error: unknown model '%s'\n\n", n.c_str());
            list_models(catalogue, models_dir);
            return 2;
        }
    }

    if (verify) return verify_models(catalogue, models_dir, names);

    if (names.empty()) {
        print_download_usage();
        std::printf("\n");
        return list_models(catalogue, models_dir);
    }

    if (!http_available()) {
        std::fprintf(stderr, "error: this build cannot download (no HTTP backend)\n");
        return 1;
    }

    if (!dir_exists(models_dir) && !make_dirs(models_dir)) {
        std::fprintf(stderr, "error: cannot create %s\n", models_dir.c_str());
        return 1;
    }

    for (const std::string& n : names) {
        const int rc = download_one(*catalogue.find(n), models_dir, assume_yes);
        if (rc != 0) return rc;
    }
    return 0;
}

} // namespace pastiche
