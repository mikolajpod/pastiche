// Pastiche command-line interface.
//
//   pastiche <content> <style> <out> <algo> [-p k=v ...] [--size N] [--tile]
//            [--backend X] [--models-dir D] [--threads N] [-v]
//   pastiche --list-algos | --help [algo] | --selftest | --benchmark | --version
//
// Everything algorithm-specific comes from the registry and ParamSpec, so
// adding an algorithm does not touch this file.
#include "core/algorithm.hpp"
#include "core/fs.hpp"
#include "core/image_io.hpp"
#include "selftest.hpp"
#include "version.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace pastiche;

namespace {

enum ExitCode { EX_OK = 0, EX_USAGE = 1, EX_INPUT = 2, EX_RUN = 3, EX_CANCELLED = 4, EX_VRAM = 5 };

void print_usage(FILE* f)
{
    std::fprintf(f,
        "Pastiche %s - neural style transfer\n"
        "\n"
        "Usage:\n"
        "  pastiche <content> <style> <out> <algo> [options]\n"
        "  pastiche --list-algos\n"
        "  pastiche --help [algo]\n"
        "  pastiche --selftest [--backend X] [--models-dir D]\n"
        "  pastiche --benchmark [--backend X] [--models-dir D]\n"
        "  pastiche --version\n"
        "\n"
        "Arguments:\n"
        "  content          Input image (%s); JPEG EXIF orientation is applied\n"
        "  style            Style image, preset name or \"-\", depending on the algorithm\n"
        "                   (see --help <algo>)\n"
        "  out              Output file (%s); a .json sidecar with the settings used\n"
        "                   is written next to it\n"
        "  algo             Algorithm id (see --list-algos)\n"
        "\n"
        "Options:\n"
        "  -p key=value     Algorithm parameter, repeatable (see --help <algo>)\n"
        "  --size N         Scale the content so its longer side is N pixels first\n"
        "  --tile           Process in tiles (feed-forward algorithms only, may show seams)\n"
        "  --backend X      cpu | dml | cuda (default: first available)\n"
        "  --models-dir D   Model directory (default: PASTICHE_MODELS_DIR or models/ next to\n"
        "                   the executable)\n"
        "  --threads N      CPU threads for inference (default: library default)\n"
        "  -v, --verbose    Print timings and details\n",
        PASTICHE_VERSION, supported_input_formats().c_str(), supported_output_formats().c_str());
}

void print_algo_help(const IStyleAlgorithm& algo, const RunOptions& opts)
{
    std::printf("%s - %s\n", algo.id().c_str(), algo.description().c_str());
    switch (algo.style_input()) {
        case StyleInput::Image: std::printf("style:   path to a style image\n"); break;
        case StyleInput::None: std::printf("style:   ignored (pass \"-\")\n"); break;
        case StyleInput::Preset: {
            std::printf("style:   preset name, one of:");
            const std::vector<std::string> presets = algo.presets(opts);
            if (presets.empty()) std::printf(" (none found in %s)", opts.models_dir.c_str());
            for (const std::string& p : presets) std::printf(" %s", p.c_str());
            std::printf("\n");
            break;
        }
    }
    std::printf("tiling:  %s\n", algo.supports_tiling() ? "supported (--tile)" : "not supported");
    std::printf("gpu:     %s\n", algo.uses_gpu() ? "yes" : "no");
    const std::vector<ParamSpec> specs = algo.params();
    if (specs.empty()) std::printf("params:  none\n");
    else std::printf("params (-p key=value):\n%s", params_help(specs, 2).c_str());
}

void list_algos()
{
    for (const std::string& id : Registry::instance().ids()) {
        std::unique_ptr<IStyleAlgorithm> a = Registry::instance().create(id);
        std::printf("%-14s %s\n", id.c_str(), a->description().c_str());
    }
}

class ConsoleProgress final : public Progress {
public:
    void report(float fraction, const std::string& stage) override
    {
        const int pct = static_cast<int>(fraction * 100.f + 0.5f);
        if (pct == last_pct_ && stage == last_stage_) return;
        last_pct_ = pct;
        last_stage_ = stage;
        std::fprintf(stderr, "\r  %3d%%  %-40s", pct, stage.c_str());
        std::fflush(stderr);
        printed_ = true;
    }
    void log(const std::string& line) override
    {
        finish();
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    void finish()
    {
        if (printed_) std::fprintf(stderr, "\n");
        printed_ = false;
    }
private:
    int last_pct_ = -1;
    std::string last_stage_;
    bool printed_ = false;
};

std::string resolve_models_dir(const std::string& from_flag)
{
    if (!from_flag.empty()) return from_flag;
    const std::string env = getenv_utf8("PASTICHE_MODELS_DIR");
    if (!env.empty()) return env;
    const std::string beside = path_join(exe_dir(), "models");
    if (dir_exists(beside)) return beside;
    // Development layout: build/pastiche.exe with models/ in the repo root.
    const std::string parent = path_join(path_dirname(exe_dir()), "models");
    if (dir_exists(parent)) return parent;
    return beside;
}

std::string human_bytes(uint64_t b)
{
    char buf[64];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof buf, "%.2f GiB", b / double(1ull << 30));
    else std::snprintf(buf, sizeof buf, "%.0f MiB", b / double(1ull << 20));
    return buf;
}

struct Cli {
    std::vector<std::string> positional;
    std::vector<std::string> params;
    int size = 0;
    bool tile = false;
    std::string backend, models_dir;
    int threads = 0;
    bool verbose = false, list = false, help = false, selftest = false, benchmark = false, version = false;
    std::string error;
};

Cli parse_cli(const std::vector<std::string>& args)
{
    Cli c;
    auto need = [&](size_t& i, const char* flag) -> const std::string* {
        if (i + 1 >= args.size()) { c.error = std::string(flag) + " requires a value"; return nullptr; }
        return &args[++i];
    };
    for (size_t i = 1; i < args.size() && c.error.empty(); ++i) {
        const std::string& a = args[i];
        if (a == "-p") { if (const std::string* v = need(i, "-p")) c.params.push_back(*v); }
        else if (a.rfind("-p", 0) == 0 && a.size() > 2 && a.find('=') != std::string::npos) c.params.push_back(a.substr(2));
        else if (a == "--size") { if (const std::string* v = need(i, "--size")) c.size = std::atoi(v->c_str()); }
        else if (a == "--tile") c.tile = true;
        else if (a == "--backend") { if (const std::string* v = need(i, "--backend")) c.backend = *v; }
        else if (a == "--models-dir") { if (const std::string* v = need(i, "--models-dir")) c.models_dir = *v; }
        else if (a == "--threads") { if (const std::string* v = need(i, "--threads")) c.threads = std::atoi(v->c_str()); }
        else if (a == "-v" || a == "--verbose") c.verbose = true;
        else if (a == "--list-algos") c.list = true;
        else if (a == "-h" || a == "--help") c.help = true;
        else if (a == "--selftest") c.selftest = true;
        else if (a == "--benchmark") c.benchmark = true;
        else if (a == "--version") c.version = true;
        else if (!a.empty() && a[0] == '-' && a != "-") c.error = "unknown option '" + a + "'";
        else c.positional.push_back(a);
    }
    return c;
}

int run_cli(const std::vector<std::string>& args)
{
    Cli cli = parse_cli(args);
    if (!cli.error.empty()) {
        std::fprintf(stderr, "error: %s\n\n", cli.error.c_str());
        print_usage(stderr);
        return EX_USAGE;
    }
    RunOptions opts;
    opts.tile = cli.tile;
    opts.backend = cli.backend;
    opts.models_dir = resolve_models_dir(cli.models_dir);
    opts.threads = cli.threads;
    opts.verbose = cli.verbose;

    if (cli.version) { std::printf("pastiche %s\n", PASTICHE_VERSION); return EX_OK; }
    if (cli.list) { list_algos(); return EX_OK; }
    if (cli.help) {
        if (cli.positional.empty()) { print_usage(stdout); return EX_OK; }
        std::unique_ptr<IStyleAlgorithm> a = Registry::instance().create(cli.positional[0]);
        if (!a) { std::fprintf(stderr, "error: unknown algorithm '%s'\n", cli.positional[0].c_str()); list_algos(); return EX_USAGE; }
        print_algo_help(*a, opts);
        return EX_OK;
    }
    if (cli.selftest) return run_selftest(opts);
    if (cli.benchmark) return run_benchmark(opts);

    if (cli.positional.size() != 4) {
        if (!cli.positional.empty())
            std::fprintf(stderr, "error: expected 4 arguments <content> <style> <out> <algo>, got %zu\n\n", cli.positional.size());
        print_usage(cli.positional.empty() ? stdout : stderr);
        return cli.positional.empty() ? EX_OK : EX_USAGE;
    }
    const std::string& content_path = cli.positional[0];
    const std::string& style_arg = cli.positional[1];
    const std::string& out_path = cli.positional[2];
    const std::string& algo_id = cli.positional[3];

    std::unique_ptr<IStyleAlgorithm> algo = Registry::instance().create(algo_id);
    if (!algo) {
        std::fprintf(stderr, "error: unknown algorithm '%s'. Available:\n", algo_id.c_str());
        list_algos();
        return EX_USAGE;
    }
    const std::vector<ParamSpec> specs = algo->params();
    Params params = Params::defaults(specs);
    for (const std::string& kv : cli.params) {
        const std::string err = params.set_kv(specs, kv);
        if (!err.empty()) {
            std::fprintf(stderr, "error: %s\n\n", err.c_str());
            print_algo_help(*algo, opts);
            return EX_USAGE;
        }
    }
    if (cli.tile && !algo->supports_tiling()) {
        std::fprintf(stderr, "error: algorithm '%s' does not support --tile\n", algo_id.c_str());
        return EX_USAGE;
    }
    const std::string out_ext = path_extension(out_path);
    if (out_ext != ".png" && !(out_ext == ".jxl" && jxl_available())) {
        std::fprintf(stderr, "error: output must be %s, got '%s'\n", supported_output_formats().c_str(), out_path.c_str());
        return EX_USAGE;
    }

    // Inputs.
    const auto t_load = std::chrono::steady_clock::now();
    Image content;
    std::string err = load_image(content_path, content);
    if (!err.empty()) { std::fprintf(stderr, "error: %s\n", err.c_str()); return EX_INPUT; }
    const int orig_w = content.width, orig_h = content.height;
    if (cli.size > 0) content = fit_longest_side(content, cli.size, true);

    Image style_img;
    const Image* style = nullptr;
    std::string style_name = style_arg;
    switch (algo->style_input()) {
        case StyleInput::Image:
            if (style_arg == "-") { std::fprintf(stderr, "error: algorithm '%s' needs a style image\n", algo_id.c_str()); return EX_USAGE; }
            err = load_image(style_arg, style_img);
            if (!err.empty()) { std::fprintf(stderr, "error: %s\n", err.c_str()); return EX_INPUT; }
            style = &style_img;
            break;
        case StyleInput::Preset: {
            const std::vector<std::string> presets = algo->presets(opts);
            bool found = false;
            for (const std::string& p : presets) if (p == style_arg) found = true;
            if (!found && !file_exists(style_arg)) {
                std::fprintf(stderr, "error: unknown preset '%s' for '%s'. Available in %s:", style_arg.c_str(),
                             algo_id.c_str(), opts.models_dir.c_str());
                for (const std::string& p : presets) std::fprintf(stderr, " %s", p.c_str());
                std::fprintf(stderr, "%s\n", presets.empty() ? " (none)" : "");
                return EX_INPUT;
            }
            break;
        }
        case StyleInput::None:
            style_name.clear();
            break;
    }
    if (cli.verbose) {
        std::fprintf(stderr, "content: %dx%d%s, style: %s, algorithm: %s, models: %s\n", content.width, content.height,
                     (cli.size > 0 && (orig_w != content.width || orig_h != content.height))
                         ? (" (scaled from " + std::to_string(orig_w) + "x" + std::to_string(orig_h) + ")").c_str() : "",
                     style ? (std::to_string(style->width) + "x" + std::to_string(style->height)).c_str()
                           : (style_name.empty() ? "-" : style_name.c_str()),
                     algo_id.c_str(), opts.models_dir.c_str());
        std::fprintf(stderr, "params: %s\n", params_to_json(specs, params).c_str());
    }

    // VRAM estimate and refusal (D5): nothing is scaled automatically.
    const uint64_t vram = algo->estimate_vram(content.width, content.height, params, opts);
    if (cli.verbose && vram > 0) std::fprintf(stderr, "estimated GPU memory: %s\n", human_bytes(vram).c_str());
    err = algo->preflight(content.width, content.height, params, opts);
    if (!err.empty()) { std::fprintf(stderr, "error: %s\n", err.c_str()); return EX_VRAM; }

    // Run.
    ConsoleProgress progress;
    const auto t_run = std::chrono::steady_clock::now();
    RunResult result = algo->run(content, style, style_name, params, opts, progress);
    progress.finish();
    const auto t_end = std::chrono::steady_clock::now();
    if (result.cancelled) { std::fprintf(stderr, "cancelled\n"); return EX_CANCELLED; }
    if (!result.error.empty()) { std::fprintf(stderr, "error: %s\n", result.error.c_str()); return EX_RUN; }
    if (result.image.empty()) { std::fprintf(stderr, "error: algorithm returned an empty image\n"); return EX_RUN; }

    // Output + sidecar.
    const std::string out_dir = path_dirname(out_path);
    if (!out_dir.empty() && !make_dirs(out_dir)) { std::fprintf(stderr, "error: cannot create directory '%s'\n", out_dir.c_str()); return EX_RUN; }
    err = save_image(out_path, result.image);
    if (!err.empty()) { std::fprintf(stderr, "error: %s\n", err.c_str()); return EX_RUN; }

    const double secs = std::chrono::duration<double>(t_end - t_run).count();
    std::string json = "{\n";
    json += "  \"app\": \"pastiche\",\n";
    json += "  \"version\": " + json_quote(PASTICHE_VERSION) + ",\n";
    json += "  \"timestamp\": " + json_quote(iso_time_now()) + ",\n";
    json += "  \"algorithm\": " + json_quote(algo_id) + ",\n";
    json += "  \"content\": " + json_quote(absolute_path(content_path)) + ",\n";
    json += "  \"style\": " + (style_name.empty() && !style ? std::string("null") : json_quote(style ? absolute_path(style_arg) : style_name)) + ",\n";
    json += "  \"output\": " + json_quote(absolute_path(out_path)) + ",\n";
    json += "  \"content_size\": [" + std::to_string(orig_w) + ", " + std::to_string(orig_h) + "],\n";
    json += "  \"processed_size\": [" + std::to_string(content.width) + ", " + std::to_string(content.height) + "],\n";
    json += "  \"output_size\": [" + std::to_string(result.image.width) + ", " + std::to_string(result.image.height) + "],\n";
    json += "  \"size\": " + std::to_string(cli.size) + ",\n";
    json += "  \"tile\": " + std::string(cli.tile ? "true" : "false") + ",\n";
    json += "  \"backend\": " + (result.backend_used.empty() ? std::string("null") : json_quote(result.backend_used)) + ",\n";
    json += "  \"params\": " + params_to_json(specs, params) + ",\n";
    json += "  \"time_seconds\": " + json_number(secs) + "\n";
    json += "}\n";
    const std::string json_path = replace_extension(out_path, ".json");
    err = write_text_file(json_path, json);
    if (!err.empty()) { std::fprintf(stderr, "warning: %s\n", err.c_str()); }

    if (cli.verbose) {
        const double load_s = std::chrono::duration<double>(t_run - t_load).count();
        std::fprintf(stderr, "load %.2f s, run %.2f s%s\n", load_s, secs,
                     result.backend_used.empty() ? "" : (", backend " + result.backend_used).c_str());
    }
    std::printf("%s\n", out_path.c_str());
    return EX_OK;
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    setup_console_utf8();
    std::vector<std::string> args = utf8_argv();
    if (args.empty()) for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
#else
    std::vector<std::string> args(argv, argv + argc);
#endif
    return run_cli(args);
}
