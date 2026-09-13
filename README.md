# Pastiche

Offline neural style transfer for the desktop: a command-line tool
(`pastiche`) and a small GUI (`pastiche-gui`), written in C++17, running
inference on the GPU through ONNX Runtime (DirectML / CUDA / CPU) and
stable-diffusion.cpp (Vulkan). No Python needed at run time.

Status: early development. See `DECYZJE.md` (Polish) for the design decisions
and the staged plan.

## Algorithms

| id         | style input | speed (T2000, 1024 px) | notes                                    |
|------------|-------------|------------------------|------------------------------------------|
| `identity` | none        | -                      | copies the content image; pipeline test  |
| `johnson`  | preset      | ~0.8 s DirectML        | Johnson et al. 2016 feed-forward nets; presets candy, mosaic, pointilism, rain_princess, udnie (BSD-3) |
| `adain`    | image       | ~1.1 s DirectML        | Huang & Belongie 2017, arbitrary style; `alpha`, `style_size`; tiling with global statistics |
| `sd`       | image       | slow                   | SD 1.5 + IP-Adapter img2img via stable-diffusion.cpp (planned, stage 5) |

Both ONNX algorithms refuse to start when the estimated GPU memory exceeds
what is free, and print the `--size` that would fit (plus `--tile` where
supported). Nothing is rescaled silently.

## Command line

```
pastiche <content> <style> <out> <algo> [-p key=value ...] [--size N] [--tile]
         [--backend cpu|dml|cuda] [--models-dir D] [--threads N] [-v]
pastiche download [--list | --verify | <model>...] [--models-dir D] [--yes]
pastiche --list-algos
pastiche --help [algo]
pastiche --selftest
pastiche --benchmark
pastiche --version [-v]
```

`--version -v` doubles as the backend report: which runtimes were found, and
which GPU the diffusion backend will use by default.

Input formats: JPEG (EXIF orientation applied), PNG, WebP, and JPEG XL when
built with libjxl. Output: PNG (or lossless JPEG XL with `.jxl`). Next to the
output a `.json` sidecar records the algorithm, parameters, inputs and version.

Example:

```
pastiche photo.jpg - out/photo.png identity -p gamma=1.2
pastiche photo.jpg mosaic out/mosaic.png johnson --size 1600
pastiche photo.jpg vangogh.jpg out/adain.png adain -p alpha=0.8
```

Parameters are declared by each algorithm; `pastiche --help <algo>` prints
them with types, ranges and defaults. The GUI builds its controls from the
same declarations.

## GUI

`pastiche-gui` shows a 2x2 grid: content image, style image, result and a
parameter panel built from the algorithm's own parameter declarations. Open
images from the File menu or drop them on the window; the first file becomes
the content, the second the style. Optional arguments preload them:

```
pastiche-gui [content-image] [style-image]
```

The run happens on a worker thread, so the window stays responsive: the
progress bar shows the stage, Cancel stops the work (a cancelled run writes no
file) and the parameter panel is greyed out until it finishes. Every result is
saved automatically to the output folder as `YYYYMMDD_HHMMSS.png` with a
matching `.json`; "Save as..." writes a copy elsewhere and "Use as content"
feeds the result back in for chaining.

When the estimated GPU memory does not fit, the panel explains why and offers
a one-click smaller size instead of silently rescaling.

## Building (Windows, MSYS2 MinGW-w64)

```bash
pacman -S mingw-w64-x86_64-{gcc,cmake,ninja,pkgconf,libpng,libjpeg-turbo,libwebp,libjxl,SDL2}

# MSYS2 must precede any other MinGW (e.g. Git's) in PATH:
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=/c/msys64/mingw64
cmake --build build
./build/pastiche.exe --selftest
./build/pastiche-tests.exe

bash package.sh    # -> pastiche-<version>-win64.zip (portable)
```

ONNX Runtime with the DirectML execution provider is not in MSYS2; the build
expects the Microsoft binaries under `third_party/onnxruntime/` (`include/`,
`bin/onnxruntime.dll`, `bin/DirectML.dll`). See `THIRD-PARTY-LICENSES.md`
for where they come from.

Dear ImGui and nativefiledialog-extended are fetched at configure time with
pinned tags; `-DPASTICHE_BUILD_GUI=OFF` builds the command-line tool alone, and
`-DPASTICHE_BUILD_TESTS=OFF` skips doctest.

Linux: builds with the same CMake project (CPU or CUDA execution provider);
see `BUILD-linux.md` once available.

There are no automated GUI tests. `tools/gui_shot.ps1` (Windows, PowerShell)
starts the window, replays clicks and keystrokes and saves screenshots, which
is how GUI changes are checked:

```powershell
& .\tools\gui_shot.ps1 -Exe build\pastiche-gui.exe `
    -AppArgs D:\hg\style\testdata\content.png,D:\hg\style\testdata\style.png `
    -Steps "click:771,754","wait:4000","shot:build/gui_done.png"
```

## Models

Only permissively licensed weights ship with the release (`models/`). Everything
else is downloaded on demand, with the licence shown and confirmed first where
the terms are not plainly permissive:

```
pastiche download --list            what exists and what is already here
pastiche download sd15              asks you to accept CreativeML OpenRAIL-M
pastiche download --verify          re-check the checksums of what you have
```

The catalogue lives in `models.json`: URL, SHA-256 and licence per file.
Downloads go to `<file>.part` and are renamed only once complete, so an
interrupted transfer resumes instead of masquerading as a finished one, and
every file is checked against its SHA-256 before it is accepted.

The checksums are the Hugging Face LFS object ids, which are the SHA-256 of the
file contents, so bumping a model version does not require downloading it:

```
curl -s 'https://huggingface.co/api/models/<repo>/tree/main?recursive=1' | jq '.[] | {path, lfs}'
```

The bundled ONNX files are produced by the scripts in `tools/` (Python 3 with
`numpy` and `onnx`; no PyTorch needed):

```
# Johnson: ONNX Model Zoo exports (candy-9.onnx etc.) -> dynamic-size models
python tools/prepare_johnson_onnx.py <dir with *-9.onnx> models
# AdaIN: decoder.pth + vgg_normalised.pth from naoto0804/pytorch-AdaIN releases
python tools/prepare_adain_onnx.py <dir with the .pth files> models
```

## Licence

MIT, see `LICENSE`. Third-party components and model licences are listed in
`THIRD-PARTY-LICENSES.md`.
