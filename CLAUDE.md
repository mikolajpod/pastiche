# Pastiche — Project Instructions

## Project Overview

Offline neural style transfer for the desktop: `pastiche.exe` (CLI) and
`pastiche-gui.exe` (Dear ImGui), both linked against the static `pastiche-core`.
Inference only — training happens offline in Python and is exported to ONNX.

**`DECYZJE.md` is the source of truth** for design decisions (D1..D19, Polish).
Read it before proposing anything architectural; do not re-litigate a closed
decision, add new ones as D20, D21... with a date.

Stages (D13): 1 skeleton, 2 Johnson, 3 AdaIN, 4 GUI — **done**;
5 diffusion (stable-diffusion.cpp + IP-Adapter, includes the model downloader
deferred by D19) — **next**; 6 polish, Linux, 1.0 release — partly done
(JXL, licences, README).

## Language (D14)

- Code, comments, CLI/GUI strings, README, THIRD-PARTY-LICENSES: **English**.
- `DECYZJE.md`, research notes, commit messages, talking to the user: **Polish,
  with full diacritics**. Never strip ą/ę/ł to ASCII.

## Tech Stack

- C++17, GCC 15.2 (MSYS2 MinGW-w64). **No MSVC, no CUDA Toolkit, no Qt** — the
  whole architecture depends on this staying true.
- CMake + Ninja. FetchContent with pinned tags: Dear ImGui v1.91.6,
  nativefiledialog-extended v1.2.1, doctest v2.4.11.
- `find_package` / `pkg_check_modules` for MSYS2 libraries: libpng,
  libjpeg-turbo, libwebp, libjxl (optional, `HAVE_JXL`), SDL2.
- ONNX Runtime 1.22.1 with the DirectML EP: Microsoft binaries, **not** in
  MSYS2, expected under `third_party/onnxruntime/{include,bin}` (untracked; see
  D15 for where they came from). The DLL is loaded at run time, never linked.

## Build

```bash
# MSYS2 must precede Git's MinGW in PATH or cc1.exe loads the wrong DLLs and
# the build breaks silently. Every shell session:
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=/c/msys64/mingw64
cmake --build build

./build/pastiche-tests.exe      # doctest: params, EXIF, image ops, I/O
./build/pastiche.exe --selftest # every algorithm at 256 px, GPU vs CPU reference
./build/pastiche.exe --benchmark

bash package.sh                 # -> pastiche-<version>-win64.zip
```

`-DPASTICHE_BUILD_GUI=OFF` and `-DPASTICHE_BUILD_TESTS=OFF` cut the GUI and
doctest. The version lives only in `project(pastiche VERSION ...)`;
`version.hpp` is generated and `package.sh` greps the same line.

**The exe must not be running when you rebuild** — the linker fails with
permission denied.

## Hard Rules

- **Never scale or tile behind the user's back** (D5). When the VRAM estimate
  exceeds free GPU memory, `preflight()` refuses with the numbers, a suggested
  `--size` and `--tile` if supported. The GUI shows the same text plus an
  "apply suggestion" button.
- **Adding an algorithm must not touch `main.cpp` or `guimain.cpp`** (D4). One
  file in `src/algos/`, a class implementing `IStyleAlgorithm`,
  `REGISTER_ALGORITHM` at the end, and a line in the `pastiche-algos` list in
  `CMakeLists.txt`. Parameters are declared as `ParamSpec` data; the CLI parser,
  `--help` and the GUI widgets are all generated from them.
- **Only permissively licensed weights ship in the release** (D8). Anything
  else is a user download with the licence shown first.
- **Verification is CLI-first** (D12). There are no automated GUI tests; use
  `tools/gui_shot.ps1` to drive the window and take screenshots.
- No Unicode checkmarks in scripts. Keep the line endings of edited files
  (everything here is LF, enforced by `.gitattributes`).
- `git add` new source files; never commit build artefacts, `third_party/`,
  model weights, ZIPs or stray images dropped into the repo root. Prefer
  explicit paths over `git add -A`.

## Layout

| Path | Role |
|---|---|
| `src/core/image.*` | `Image` (8-bit sRGB, RGB/RGBA), resize (area/bilinear), EXIF rotation, crop, CHW conversion, hash |
| `src/core/image_io.*` | Format detection **by content**, JPEG/PNG/WebP/JXL decode, PNG/JXL encode, extension lists |
| `src/core/exif.*` | Small EXIF parser, orientation tag 0x0112 only |
| `src/core/fs.*` | UTF-8 paths (UTF-16 under the hood on Windows), file I/O, exe dir, timestamps |
| `src/core/params.*` | `ParamSpec`, `Params`, validation, help text, JSON rendering |
| `src/core/algorithm.*` | `IStyleAlgorithm`, `Progress`, `RunResult`, `Registry`, `REGISTER_ALGORITHM` |
| `src/core/tiling.*` | Tile rectangles, ramp weights, `TileBlender`, `pad_to_multiple` |
| `src/core/gpu_info.*` | DXGI `QueryVideoMemoryInfo` on the biggest hardware adapter |
| `src/core/sidecar.*` | The `.json` written next to every result; shared by CLI and GUI |
| `src/backends/ort_session.*` | ONNX Runtime loader, `OrtModel`, VRAM preflight, cancellation watchdog |
| `src/algos/*.cpp` | One file per algorithm: `identity`, `johnson`, `adain` |
| `src/main.cpp` | CLI: argument parsing, run, sidecar |
| `src/guimain.cpp` | GUI: 2x2 grid, ParamSpec panel, worker thread, autosave |
| `src/selftest.cpp` | `--selftest` and `--benchmark` |
| `tools/*.py` | Model conversion (no PyTorch needed); `torch_legacy.py` reads `.pth` with numpy alone |
| `tools/gui_shot.ps1` | Drives the GUI and saves screenshots |

## Key Invariants

**Algorithms are an OBJECT library.** `pastiche-algos` is `add_library(...
OBJECT ...)` and injected into both executables with `$<TARGET_OBJECTS:...>`.
In a static library the linker would drop the object files that contain nothing
but a static registrar, and the algorithm would silently vanish from
`--list-algos` (D17).

**ONNX Runtime is loaded at run time**, via `LoadLibraryEx` with
`LOAD_WITH_ALTERED_SEARCH_PATH` (D16). This keeps the exe startable without
ORT and, more importantly, makes it pick up `DirectML.dll` from the exe
directory instead of the ancient one in `System32`. Search order:
`PASTICHE_ORT_DIR`, exe dir, the compile-time `third_party` path, then `PATH`.

**DirectML needs `DisableMemPattern` + `ORT_SEQUENTIAL`** before appending the
EP, and goes through `SessionOptionsAppendExecutionProvider_DML2` with
`HighPerformance` + GPU filter so laptops pick the discrete GPU. `OrtDmlApi` is
mirrored as a local struct to avoid pulling in `d3d12.h` / `DirectML.h`.

**Cancellation.** A single `Run()` takes seconds and cannot be polled from
inside, so `OrtModel::run()` starts a watchdog thread that checks
`Progress::cancelled()` every 50 ms and sets the ORT terminate flag; `run()`
then returns the `kCancelled` sentinel, which algorithms turn into
`RunResult::aborted()`. A cancelled run writes no file. The flag is cleared
before every run.

**VRAM constants are measured, not guessed.** `kFixedBytes` and
`kBytesPerPixel` in `johnson.cpp` / `adain.cpp` come from `-v` runs that print
the DXGI usage delta; the comments list the measurements. The DirectML
allocator rounds buffers to powers of two, so the curve is a staircase — keep
the constants above the measured points. Re-measure after changing a model or
the ORT version.

**Models directory resolution** (CLI and GUI both): `--models-dir`, then
`PASTICHE_MODELS_DIR`, then `models/` next to the exe, then `models/` one level
up so `build/pastiche.exe` finds the repo's models during development.

**Image invariants.** Interleaved 8-bit sRGB, stride is always
`width * channels`, no padding, ICC ignored (D6). JPEG EXIF orientation is
applied at decode, so everything downstream sees an upright image. Format is
detected from the magic bytes, never the extension — file dialogs use
`supported_input_extensions()`, which must stay in step with `detect_format()`.

**AdaIN statistics are global.** Mean and standard deviation are accumulated
over all content tiles before any tile is decoded, so tiling cannot shift the
tone between tiles. Johnson has no such protection — its instance
normalisation is per tile, which is why the `tile_size` description warns about
seams on flat areas.

## Model Conversion

No PyTorch anywhere (pip is blocked on this network, D15). `tools/torch_legacy.py`
reads both `.pth` formats with numpy and pickle alone;
`tools/prepare_adain_onnx.py` builds the encoder/decoder graphs with
`onnx.helper`; `tools/prepare_johnson_onnx.py` only relaxes the fixed input
dimensions of the ONNX Model Zoo exports. Model I/O ranges differ: Johnson is
0..255, AdaIN is 0..1.

## Known Gotchas

- **`exit=127` when running the exe from Bash** means the MinGW DLLs are not on
  `PATH` — export the MSYS2 path first.
- **`git add -A` is dangerous here** — the repo root collects test photos and
  the odd artefact.
- **PowerShell `-File` does not pass arrays**, so `tools/gui_shot.ps1` must be
  invoked as `& .\tools\gui_shot.ps1 -Steps "a","b"`, not via `powershell -File`.
- **GUI click coordinates shift** by one line height when the GPU estimate or
  the refusal message appears; take a screenshot before trusting old
  coordinates (the script's header lists the current ones).
- **libjpeg uses `setjmp`** for errors; keep the error struct and the jump
  buffer alive across the decode and do not let destructors run between them.
- DirectML and CPU results agree to a mean difference of 0.00/255 (max 1) —
  `--selftest` fails above 2.0, so a real regression is obvious.
