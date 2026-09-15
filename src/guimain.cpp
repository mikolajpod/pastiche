// Pastiche GUI: Dear ImGui + SDL2 + OpenGL 3.3 (D3, D9).
//
// Layout: a 2x2 grid - content image, style image, result, parameter panel.
// One worker thread runs the algorithm; the UI polls a mutex-protected
// WorkerState every frame (progress, stage text, log, preview, completion).
// The parameter panel is generated from ParamSpec, so new algorithms need no
// changes here.
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include <nfd.h>

#include "backends/ort_session.hpp"
#include "core/algorithm.hpp"
#include "core/fs.hpp"
#include "core/gpu_info.hpp"
#include "core/image_io.hpp"
#include "core/sidecar.hpp"
#include "version.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pastiche;

namespace {

// ---------------------------------------------------------------------------
// GL texture holding an RGBA copy of an Image
// ---------------------------------------------------------------------------
struct GlTex {
    GLuint id = 0;
    int w = 0, h = 0;

    ~GlTex() { release(); }
    void release()
    {
        if (id) glDeleteTextures(1, &id);
        id = 0; w = h = 0;
    }
    void upload(const Image& img)
    {
        if (img.empty()) { release(); return; }
        const Image rgba = to_rgba(img);
        if (!id) {
            glGenTextures(1, &id);
            glBindTexture(GL_TEXTURE_2D, id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_2D, id);
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba.width, rgba.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data.data());
        w = rgba.width;
        h = rgba.height;
    }
    ImTextureID imgui_id() const { return static_cast<ImTextureID>(static_cast<uintptr_t>(id)); }
};

struct ImageSlot {
    Image img;
    GlTex tex;
    std::string path;    // file it came from ("" for results)
    std::string error;   // last load error

    bool loaded() const { return !img.empty(); }
    void set(Image&& im, const std::string& p)
    {
        img = std::move(im);
        path = p;
        error.clear();
        tex.upload(img);
    }
    bool load(const std::string& p)
    {
        Image im;
        const std::string err = load_image(p, im);
        if (!err.empty()) { error = err; return false; }
        set(std::move(im), p);
        return true;
    }
    void clear()
    {
        img = Image();
        path.clear();
        error.clear();
        tex.release();
    }
};

// ---------------------------------------------------------------------------
// Worker thread state (D9: polled every frame under a mutex, no event queue)
// ---------------------------------------------------------------------------
struct WorkerState {
    std::mutex m;
    float progress = 0.f;
    std::string stage;
    std::vector<std::string> log;
    Image preview;
    bool preview_dirty = false;
    bool running = false;
    bool done = false;
    RunResult result;
    double seconds = 0;
    std::atomic<bool> cancel{false};
    std::thread thread;
};

class GuiProgress final : public Progress {
public:
    explicit GuiProgress(WorkerState& ws) : ws_(ws) {}
    void report(float fraction, const std::string& stage) override
    {
        std::lock_guard<std::mutex> lock(ws_.m);
        ws_.progress = fraction;
        ws_.stage = stage;
    }
    bool cancelled() const override { return ws_.cancel.load(); }
    void preview(const Image& img) override
    {
        std::lock_guard<std::mutex> lock(ws_.m);
        ws_.preview = img;
        ws_.preview_dirty = true;
    }
    void log(const std::string& line) override
    {
        std::lock_guard<std::mutex> lock(ws_.m);
        ws_.log.push_back(line);
    }
private:
    WorkerState& ws_;
};

const char* const kBackends[] = {"auto", "dml", "cpu"};

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
struct App {
    ImageSlot content, style, result;

    std::vector<std::string> algo_ids;
    int algo_index = 0;
    std::unique_ptr<IStyleAlgorithm> algo;
    std::vector<ParamSpec> specs;
    std::map<std::string, Params> params_by_algo;
    std::vector<std::string> presets;
    int preset_index = 0;

    int size = 0;
    bool tile = false;
    int backend_index = 0;
    std::string models_dir;
    char out_dir[1024] = {};

    std::string preflight_msg;   // refusal text when the run would not fit
    std::string unavailable_msg; // why the algorithm cannot run at all (missing weights)
    bool log_open = false;       // last frame's state of the Log header, for footer sizing
    int suggested_size = 0;
    uint64_t vram_estimate = 0;
    std::string status;          // one-line status shown under the buttons
    std::vector<std::string> log;
    std::string last_saved;

    WorkerState worker;
    RunRecord pending;           // record of the run in flight
    std::chrono::steady_clock::time_point run_started;
    bool show_about = false;

    Params& params() { return params_by_algo[algo->id()]; }
    RunOptions options() const
    {
        RunOptions o;
        o.tile = tile && algo && algo->supports_tiling();
        o.backend = kBackends[backend_index];
        if (o.backend == "auto") o.backend.clear();
        o.models_dir = models_dir;
        o.verbose = true;
        return o;
    }
};

std::string default_models_dir()
{
    const std::string env = getenv_utf8("PASTICHE_MODELS_DIR");
    if (!env.empty()) return env;
    const std::string beside = path_join(exe_dir(), "models");
    if (dir_exists(beside)) return beside;
    const std::string parent = path_join(path_dirname(exe_dir()), "models");
    if (dir_exists(parent)) return parent;
    return beside;
}

void select_algo(App& app, int index)
{
    if (app.algo_ids.empty()) return;
    app.algo_index = std::clamp(index, 0, static_cast<int>(app.algo_ids.size()) - 1);
    app.algo = Registry::instance().create(app.algo_ids[app.algo_index]);
    app.specs = app.algo->params();
    if (!app.params_by_algo.count(app.algo->id()))
        app.params_by_algo[app.algo->id()] = Params::defaults(app.specs);
    app.presets = app.algo->style_input() == StyleInput::Preset ? app.algo->presets(app.options()) : std::vector<std::string>{};
    app.preset_index = std::clamp(app.preset_index, 0, std::max(0, static_cast<int>(app.presets.size()) - 1));
}

// Recomputes the VRAM estimate and the refusal message for the current inputs.
void refresh_preflight(App& app)
{
    app.preflight_msg.clear();
    app.unavailable_msg.clear();
    app.suggested_size = 0;
    app.vram_estimate = 0;
    if (!app.algo) return;

    // Asked before anything else and independently of the inputs: an algorithm
    // whose weights are missing cannot run whatever is loaded, and the answer
    // is a download, not a smaller image.
    app.unavailable_msg = app.algo->unavailable_reason(app.options());
    if (!app.unavailable_msg.empty()) return;

    if (!app.content.loaded()) return;
    int w = app.content.img.width, h = app.content.img.height;
    if (app.size > 0) {
        const double s = static_cast<double>(app.size) / std::max(w, h);
        w = std::max(1, static_cast<int>(std::lround(w * s)));
        h = std::max(1, static_cast<int>(std::lround(h * s)));
    }
    const RunOptions opts = app.options();
    app.vram_estimate = app.algo->uses_gpu() ? app.algo->estimate_vram(w, h, app.params(), opts) : 0;
    app.preflight_msg = app.algo->preflight(w, h, app.params(), opts);
    if (!app.preflight_msg.empty() && app.algo->uses_gpu()) {
        const GpuMemoryInfo gpu = query_gpu_memory();
        if (gpu.ok) app.suggested_size = vram_suggest_size(*app.algo, w, h, app.params(), opts, gpu.available());
    }
}

std::string unique_output_path(const std::string& dir, const std::string& ext)
{
    const std::string stamp = timestamp_now();
    std::string p = path_join(dir, stamp + ext);
    for (int i = 2; file_exists(p) && i < 100; ++i) p = path_join(dir, stamp + "_" + std::to_string(i) + ext);
    return p;
}

void start_run(App& app)
{
    WorkerState& ws = app.worker;
    if (ws.running || !app.algo || !app.content.loaded()) return;
    const StyleInput si = app.algo->style_input();
    if (si == StyleInput::Image && !app.style.loaded()) { app.status = "Load a style image first."; return; }
    if (si == StyleInput::Preset && app.presets.empty()) { app.status = "No presets found in " + app.models_dir; return; }

    // Snapshot everything the worker needs.
    auto content = std::make_shared<Image>(app.size > 0 ? fit_longest_side(app.content.img, app.size, true) : app.content.img);
    auto style = std::make_shared<Image>(si == StyleInput::Image ? app.style.img : Image());
    const std::string style_name = si == StyleInput::Preset ? app.presets[app.preset_index] : std::string();
    const Params params = app.params();
    const RunOptions opts = app.options();
    std::unique_ptr<IStyleAlgorithm> algo = Registry::instance().create(app.algo->id());

    app.pending = RunRecord();
    app.pending.algorithm = app.algo->id();
    app.pending.content_path = app.content.path;
    if (si == StyleInput::Image) app.pending.style_path = app.style.path;
    app.pending.style_name = style_name;
    app.pending.content_w = app.content.img.width;
    app.pending.content_h = app.content.img.height;
    app.pending.processed_w = content->width;
    app.pending.processed_h = content->height;
    app.pending.size = app.size;
    app.pending.tile = opts.tile;
    app.pending.specs = app.specs;
    app.pending.params = params;

    {
        std::lock_guard<std::mutex> lock(ws.m);
        ws.progress = 0.f;
        ws.stage = "starting";
        ws.log.clear();
        ws.preview = Image();
        ws.preview_dirty = false;
        ws.done = false;
        ws.running = true;
        ws.result = RunResult();
    }
    ws.cancel = false;
    app.status = "Running " + app.algo->id() + "...";
    app.run_started = std::chrono::steady_clock::now();

    IStyleAlgorithm* algo_raw = algo.release();
    ws.thread = std::thread([&ws, algo_raw, content, style, style_name, params, opts]() {
        std::unique_ptr<IStyleAlgorithm> owned(algo_raw);
        GuiProgress progress(ws);
        const auto t0 = std::chrono::steady_clock::now();
        RunResult r = owned->run(*content, style->empty() ? nullptr : style.get(), style_name, params, opts, progress);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lock(ws.m);
        ws.result = std::move(r);
        ws.seconds = secs;
        ws.running = false;
        ws.done = true;
    });
}

// Called every frame: picks up previews and the finished result.
void poll_worker(App& app)
{
    WorkerState& ws = app.worker;
    Image preview;
    bool have_preview = false, finished = false;
    RunResult result;
    double secs = 0;
    std::vector<std::string> new_log;
    {
        std::lock_guard<std::mutex> lock(ws.m);
        if (ws.preview_dirty) { preview = std::move(ws.preview); ws.preview = Image(); ws.preview_dirty = false; have_preview = true; }
        if (!ws.log.empty()) { new_log = std::move(ws.log); ws.log.clear(); }
        if (ws.done) { finished = true; ws.done = false; result = std::move(ws.result); secs = ws.seconds; }
    }
    for (std::string& l : new_log) app.log.push_back(std::move(l));
    if (have_preview && !preview.empty()) app.result.tex.upload(preview);
    if (!finished) return;
    if (ws.thread.joinable()) ws.thread.join();

    if (result.cancelled) {
        app.status = "Cancelled.";
        if (app.result.loaded()) app.result.tex.upload(app.result.img);  // drop the preview
        return;
    }
    if (!result.error.empty()) {
        app.status = "Error: " + result.error;
        app.log.push_back(app.status);
        return;
    }
    // Autosave (D9): <out-dir>/YYYYMMDD_HHMMSS.png + .json
    const std::string out_dir = app.out_dir;
    std::string saved;
    if (make_dirs(out_dir)) {
        const std::string png = unique_output_path(out_dir, ".png");
        const std::string err = save_png(png, result.image);
        if (err.empty()) {
            saved = png;
            app.pending.output_path = png;
            app.pending.output_w = result.image.width;
            app.pending.output_h = result.image.height;
            app.pending.backend = result.backend_used;
            app.pending.time_seconds = secs;
            write_sidecar(replace_extension(png, ".json"), app.pending);
        } else {
            app.log.push_back(err);
        }
    } else {
        app.log.push_back("cannot create output directory " + out_dir);
    }
    app.result.set(std::move(result.image), saved);
    app.last_saved = saved;
    char buf[160];
    std::snprintf(buf, sizeof buf, "Done in %.1f s%s%s", secs,
                  result.backend_used.empty() ? "" : " on ", result.backend_used.c_str());
    app.status = buf;
    if (!saved.empty()) app.status += "  ->  " + path_basename(saved);
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------
bool pick_image(std::string& out)
{
    nfdu8char_t* path = nullptr;
    const std::string ext = supported_input_extensions();
    nfdu8filteritem_t filters[] = {{"Images", ext.c_str()}};
    const nfdresult_t r = NFD_OpenDialogU8(&path, filters, 1, nullptr);
    if (r != NFD_OKAY) return false;
    out = path;
    NFD_FreePathU8(path);
    return true;
}

bool pick_save(std::string& out, const std::string& default_name)
{
    nfdu8char_t* path = nullptr;
    std::vector<nfdu8filteritem_t> filters = {{"PNG", "png"}};
    if (jxl_available()) filters.push_back({"JPEG XL (lossless)", "jxl"});
    const nfdresult_t r = NFD_SaveDialogU8(&path, filters.data(), static_cast<nfdfiltersize_t>(filters.size()), nullptr,
                                           default_name.c_str());
    if (r != NFD_OKAY) return false;
    out = path;
    NFD_FreePathU8(path);
    return true;
}

bool pick_folder(std::string& out, const std::string& start)
{
    nfdu8char_t* path = nullptr;
    const nfdresult_t r = NFD_PickFolderU8(&path, start.empty() ? nullptr : start.c_str());
    if (r != NFD_OKAY) return false;
    out = path;
    NFD_FreePathU8(path);
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
void draw_fitted_image(const GlTex& tex, ImVec2 avail)
{
    if (!tex.id || avail.x < 4 || avail.y < 4) {
        ImGui::Dummy(avail);
        return;
    }
    const float s = std::min(avail.x / tex.w, avail.y / tex.h);
    const ImVec2 size(tex.w * s, tex.h * s);
    const ImVec2 pos = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(pos.x + (avail.x - size.x) * 0.5f, pos.y + (avail.y - size.y) * 0.5f));
    ImGui::Image(tex.imgui_id(), size);
    ImGui::SetCursorPos(ImVec2(pos.x, pos.y + avail.y));
}

void draw_image_cell(App& app, const char* title, ImageSlot& slot, ImVec2 size, bool is_content, bool is_style)
{
    ImGui::BeginChild(title, size, ImGuiChildFlags_Borders);
    const bool running = app.worker.running;
    ImGui::TextUnformatted(title);
    if (slot.loaded()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d  %s", slot.img.width, slot.img.height, path_basename(slot.path).c_str());
    }
    const float button_h = ImGui::GetFrameHeightWithSpacing();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= button_h;
    if (slot.loaded()) {
        draw_fitted_image(slot.tex, avail);
    } else {
        const ImVec2 pos = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(pos.x + 8, pos.y + avail.y * 0.5f - 8));
        if (!slot.error.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", slot.error.c_str());
        else if (is_content) ImGui::TextDisabled("Open a content image or drop a file here.");
        else if (is_style) ImGui::TextDisabled("Open a style image (used by algorithms that take one).");
        else ImGui::TextDisabled("The result appears here and is saved to the output folder.");
        ImGui::SetCursorPos(ImVec2(pos.x, pos.y + avail.y));
    }
    if (is_content || is_style) {
        if (ImGui::Button("Open...")) {
            std::string p;
            if (pick_image(p)) {
                if (slot.load(p) && is_content) refresh_preflight(app);
            }
        }
        if (slot.loaded()) {
            ImGui::SameLine();
            if (ImGui::Button("Clear")) { slot.clear(); if (is_content) refresh_preflight(app); }
        }
    } else {
        ImGui::BeginDisabled(!slot.loaded() || running);
        if (ImGui::Button("Save as...")) {
            std::string p;
            if (pick_save(p, path_basename(slot.path.empty() ? timestamp_now() + ".png" : slot.path))) {
                if (path_extension(p).empty()) p += ".png";
                const std::string err = save_image(p, slot.img);
                app.status = err.empty() ? "Saved " + p : "Error: " + err;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Use as content")) {
            Image copy = slot.img;
            app.content.set(std::move(copy), slot.path);
            refresh_preflight(app);
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
}

void draw_param_widget(App& app, const ParamSpec& spec)
{
    Params& p = app.params();
    ParamValue& v = p.values()[spec.key];
    bool changed = false;
    switch (spec.type) {
        case ParamType::Int: {
            int i = static_cast<int>(std::lround(v.num));
            if (spec.min != spec.max) changed = ImGui::SliderInt(spec.key.c_str(), &i, static_cast<int>(spec.min), static_cast<int>(spec.max));
            else changed = ImGui::InputInt(spec.key.c_str(), &i);
            if (changed) v.num = i;
            break;
        }
        case ParamType::Float: {
            float f = static_cast<float>(v.num);
            if (spec.min != spec.max) changed = ImGui::SliderFloat(spec.key.c_str(), &f, static_cast<float>(spec.min), static_cast<float>(spec.max), "%.2f");
            else changed = ImGui::InputFloat(spec.key.c_str(), &f);
            if (changed) v.num = f;
            break;
        }
        case ParamType::Bool: {
            bool b = v.num != 0.0;
            if (ImGui::Checkbox(spec.key.c_str(), &b)) { v.num = b ? 1.0 : 0.0; changed = true; }
            break;
        }
        case ParamType::Enum: {
            int cur = 0;
            for (size_t i = 0; i < spec.choices.size(); ++i) if (spec.choices[i] == v.str) cur = static_cast<int>(i);
            if (ImGui::BeginCombo(spec.key.c_str(), v.str.c_str())) {
                for (size_t i = 0; i < spec.choices.size(); ++i) {
                    if (ImGui::Selectable(spec.choices[i].c_str(), static_cast<int>(i) == cur)) { v.str = spec.choices[i]; changed = true; }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case ParamType::String: {
            char buf[512];
            std::snprintf(buf, sizeof buf, "%s", v.str.c_str());
            if (ImGui::InputText(spec.key.c_str(), buf, sizeof buf)) { v.str = buf; changed = true; }
            break;
        }
    }
    if (!spec.description.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s\n(%s, default %s)", spec.description.c_str(), spec.type_name(), spec.default_text().c_str());
    if (changed) refresh_preflight(app);
}

// Height the footer below the settings needs: the refusal text, the buttons,
// the progress bar and the log header. Computed rather than guessed because it
// is reserved before any of it is drawn.
float params_footer_height(const App& app, bool running, float wrap_w)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line = ImGui::GetTextLineHeightWithSpacing();
    const float frame = ImGui::GetFrameHeightWithSpacing();

    float h = style.ItemSpacing.y * 2.f;   // the two Spacing() calls below
    if (app.content.loaded() && app.algo && app.algo->uses_gpu() && app.vram_estimate > 0) h += line;
    if (!app.unavailable_msg.empty()) {
        h += ImGui::CalcTextSize(app.unavailable_msg.c_str(), nullptr, false, wrap_w).y +
             style.ItemSpacing.y + line;
    }
    if (!app.preflight_msg.empty()) {
        h += ImGui::CalcTextSize(app.preflight_msg.c_str(), nullptr, false, wrap_w).y + style.ItemSpacing.y;
        if (app.suggested_size > 0) h += frame;
    }
    h += frame;                                            // Run / Cancel
    if (running) h += frame;                               // progress bar
    if (!app.status.empty()) {
        h += ImGui::CalcTextSize(app.status.c_str(), nullptr, false, wrap_w).y + style.ItemSpacing.y;
    }
    h += frame;                                            // "Log" header
    if (app.log_open) h += 120.f + style.ItemSpacing.y;    // the log child itself
    return h;
}

void draw_params_panel(App& app, ImVec2 size)
{
    ImGui::BeginChild("Parameters", size, ImGuiChildFlags_Borders);
    const bool running = app.worker.running;
    ImGui::TextUnformatted("Parameters");
    ImGui::Separator();

    // Everything above the buttons scrolls, the buttons do not. Nesting a
    // second scroll area inside this one was the first attempt and gave two
    // scrollbars side by side in a default-sized window, which looked broken.
    const float footer = params_footer_height(app, running, ImGui::GetContentRegionAvail().x);
    ImGui::BeginChild("settings", ImVec2(0.f, -footer), ImGuiChildFlags_None);

    ImGui::BeginDisabled(running);
    // Algorithm
    if (ImGui::BeginCombo("algorithm", app.algo ? app.algo->id().c_str() : "-")) {
        for (size_t i = 0; i < app.algo_ids.size(); ++i) {
            if (ImGui::Selectable(app.algo_ids[i].c_str(), static_cast<int>(i) == app.algo_index)) {
                select_algo(app, static_cast<int>(i));
                refresh_preflight(app);
            }
        }
        ImGui::EndCombo();
    }
    if (app.algo) ImGui::TextWrapped("%s", app.algo->description().c_str());

    // Style source
    if (app.algo) {
        switch (app.algo->style_input()) {
            case StyleInput::Image:
                ImGui::TextDisabled("style: the image in the Style cell%s", app.style.loaded() ? "" : " (none loaded)");
                break;
            case StyleInput::None:
                ImGui::TextDisabled("style: not used by this algorithm");
                break;
            case StyleInput::Preset: {
                const char* cur = app.presets.empty() ? "(no presets found)" : app.presets[app.preset_index].c_str();
                if (ImGui::BeginCombo("style preset", cur)) {
                    for (size_t i = 0; i < app.presets.size(); ++i)
                        if (ImGui::Selectable(app.presets[i].c_str(), static_cast<int>(i) == app.preset_index)) app.preset_index = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                if (app.presets.empty()) ImGui::TextWrapped("Put johnson-*.onnx files into %s", app.models_dir.c_str());
                break;
            }
        }
    }
    ImGui::Spacing();
    for (const ParamSpec& spec : app.specs) draw_param_widget(app, spec);

    ImGui::Spacing();
    ImGui::SeparatorText("Common");
    if (ImGui::InputInt("size (longer side, 0 = original)", &app.size, 64, 256)) {
        app.size = std::max(0, app.size);
        refresh_preflight(app);
    }
    if (app.algo && app.algo->supports_tiling()) {
        if (ImGui::Checkbox("tile (feed-forward only, may show seams)", &app.tile)) refresh_preflight(app);
    }
    if (ImGui::Combo("backend", &app.backend_index, kBackends, 3)) { select_algo(app, app.algo_index); refresh_preflight(app); }
    ImGui::InputText("output folder", app.out_dir, sizeof app.out_dir);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string d;
        if (pick_folder(d, app.out_dir)) std::snprintf(app.out_dir, sizeof app.out_dir, "%s", d.c_str());
    }
    ImGui::EndDisabled();
    ImGui::EndChild();   // settings

    // VRAM / preflight
    ImGui::Spacing();
    if (app.content.loaded() && app.algo && app.algo->uses_gpu() && app.vram_estimate > 0)
        ImGui::TextDisabled("estimated GPU memory: %s", human_size(app.vram_estimate).c_str());
    if (!app.unavailable_msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.6f, 0.3f, 1.f));
        ImGui::TextWrapped("%s", app.unavailable_msg.c_str());
        ImGui::PopStyleColor();
        // No button for it yet: downloading needs the licence screen the CLI
        // has, so point at the command rather than starting a multi-gigabyte
        // transfer from a control that cannot show the terms.
        ImGui::TextDisabled("Run that command in a terminal, then reopen this window.");
    }
    if (!app.preflight_msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.6f, 0.3f, 1.f));
        ImGui::TextWrapped("%s", app.preflight_msg.c_str());
        ImGui::PopStyleColor();
        if (app.suggested_size > 0) {
            ImGui::BeginDisabled(running);
            if (ImGui::Button(("Apply suggestion: size " + std::to_string(app.suggested_size)).c_str())) {
                app.size = app.suggested_size;
                refresh_preflight(app);
            }
            ImGui::EndDisabled();
        }
    }

    // Run / cancel / progress
    ImGui::Spacing();
    const bool can_run = !running && app.algo && app.content.loaded() &&
                         app.preflight_msg.empty() && app.unavailable_msg.empty() &&
                         (app.algo->style_input() != StyleInput::Image || app.style.loaded()) &&
                         (app.algo->style_input() != StyleInput::Preset || !app.presets.empty());
    ImGui::BeginDisabled(!can_run);
    if (ImGui::Button("Run", ImVec2(120, 0))) start_run(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Cancel", ImVec2(120, 0))) { app.worker.cancel = true; app.status = "Cancelling..."; }
    ImGui::EndDisabled();
    if (running) {
        float progress;
        std::string stage;
        {
            std::lock_guard<std::mutex> lock(app.worker.m);
            progress = app.worker.progress;
            stage = app.worker.stage;
        }
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - app.run_started).count();
        char overlay[128];
        std::snprintf(overlay, sizeof overlay, "%d%%  %s  (%.0f s)", static_cast<int>(progress * 100), stage.c_str(), elapsed);
        ImGui::ProgressBar(progress, ImVec2(-1, 0), overlay);
    }
    if (!app.status.empty()) ImGui::TextWrapped("%s", app.status.c_str());

    // Remembered so the reserved footer height can account for the log next
    // frame; one frame of lag is invisible.
    app.log_open = ImGui::CollapsingHeader("Log");
    if (app.log_open) {
        ImGui::BeginChild("log", ImVec2(0, 120), ImGuiChildFlags_Borders);
        for (const std::string& l : app.log) ImGui::TextWrapped("%s", l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.f);
        ImGui::EndChild();
    }
    ImGui::EndChild();
}

void draw_about(App& app)
{
    if (!app.show_about) return;
    ImGui::OpenPopup("About Pastiche");
    if (ImGui::BeginPopupModal("About Pastiche", &app.show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Pastiche %s", PASTICHE_VERSION);
        ImGui::Text("Neural style transfer, MIT licence.");
        const OrtRuntime& rt = OrtRuntime::instance();
        if (rt.available()) {
            ImGui::Text("ONNX Runtime %s, backends:%s cpu", rt.version().c_str(), rt.dml_available() ? " dml" : "");
            ImGui::TextDisabled("%s", rt.library_path().c_str());
        } else {
            ImGui::TextWrapped("ONNX Runtime not loaded: %s", rt.error().c_str());
        }
        const GpuMemoryInfo gpu = query_gpu_memory();
        if (gpu.ok) ImGui::Text("GPU: %s, %s dedicated, %s free", gpu.adapter.c_str(), human_size(gpu.dedicated_total).c_str(), human_size(gpu.available()).c_str());
        ImGui::Text("Models: %s", app.models_dir.c_str());
        ImGui::Text("Dear ImGui %s, SDL2, %s", IMGUI_VERSION, jxl_available() ? "libjxl on" : "libjxl off");
        if (ImGui::Button("Close", ImVec2(120, 0))) { app.show_about = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

void draw_frame(App& app, SDL_Window* window, bool& quit)
{
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open content...")) { std::string p; if (pick_image(p) && app.content.load(p)) refresh_preflight(app); }
            if (ImGui::MenuItem("Open style...")) { std::string p; if (pick_image(p)) app.style.load(p); }
            ImGui::Separator();
            if (ImGui::MenuItem("Open output folder in Explorer", nullptr, false, dir_exists(app.out_dir))) {
#ifdef _WIN32
                const std::wstring cmd = L"explorer.exe \"" + utf8_to_wide(absolute_path(app.out_dir)) + L"\"";
                _wsystem(cmd.c_str());
#endif
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4")) quit = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About")) app.show_about = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    int win_w, win_h;
    SDL_GetWindowSize(window, &win_w, &win_h);
    const float menu_h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(0, menu_h));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(win_w), static_cast<float>(win_h) - menu_h));
    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const ImVec2 cell((avail.x - spacing) * 0.5f, (avail.y - spacing) * 0.5f);

    draw_image_cell(app, "Content", app.content, cell, true, false);
    ImGui::SameLine();
    draw_image_cell(app, "Style", app.style, cell, false, true);
    draw_image_cell(app, "Result", app.result, cell, false, false);
    ImGui::SameLine();
    draw_params_panel(app, cell);
    ImGui::End();

    draw_about(app);
}

} // namespace

int main(int argc, char* argv[])
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* window = SDL_CreateWindow("Pastiche " PASTICHE_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          1400, 900, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");
    NFD_Init();

    App app;
    app.algo_ids = Registry::instance().ids();
    app.models_dir = default_models_dir();
    std::snprintf(app.out_dir, sizeof app.out_dir, "%s", path_join(exe_dir(), "out").c_str());
    // Prefer a real algorithm over identity as the initial selection.
    int initial = 0;
    for (size_t i = 0; i < app.algo_ids.size(); ++i) if (app.algo_ids[i] == "adain") initial = static_cast<int>(i);
    select_algo(app, initial);
    if (argc > 1 && app.content.load(argv[1])) refresh_preflight(app);
    if (argc > 2) app.style.load(argv[2]);
    if (!OrtRuntime::instance().available()) app.log.push_back(OrtRuntime::instance().error());

    bool quit = false;
    while (!quit) {
        SDL_Event event;
        const int timeout_ms = app.worker.running ? 16 : 50;
        if (SDL_WaitEventTimeout(&event, timeout_ms)) {
            do {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT) quit = true;
                if (event.type == SDL_DROPFILE && event.drop.file) {
                    const std::string p = event.drop.file;
                    SDL_free(event.drop.file);
                    if (!app.worker.running) {
                        // First drop fills the content, the next one the style, then alternate by emptiness.
                        if (!app.content.loaded() || (app.style.loaded() && app.algo && app.algo->style_input() != StyleInput::Image)) {
                            if (app.content.load(p)) refresh_preflight(app);
                        } else if (!app.style.loaded()) {
                            app.style.load(p);
                        } else if (app.content.load(p)) {
                            refresh_preflight(app);
                        }
                    }
                }
            } while (SDL_PollEvent(&event));
        }
        poll_worker(app);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        draw_frame(app, window, quit);
        ImGui::Render();
        int dw, dh;
        SDL_GL_GetDrawableSize(window, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.08f, 0.08f, 0.09f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // Let a running job finish (cancel it) before tearing down GL.
    if (app.worker.running) app.worker.cancel = true;
    if (app.worker.thread.joinable()) app.worker.thread.join();
    app.content.tex.release();
    app.style.tex.release();
    app.result.tex.release();

    NFD_Quit();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
