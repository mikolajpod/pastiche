# Pastiche

Offline neural style transfer for the desktop: a command-line tool
(`pastiche`) and a small GUI (`pastiche-gui`), written in C++17, running
inference on the GPU through ONNX Runtime (DirectML / CUDA / CPU) and
stable-diffusion.cpp (Vulkan). No Python needed at run time.

Status: early development. See `DECYZJE.md` (Polish) for the design decisions
and the staged plan.

## Algorithms

| id         | style input | speed  | notes                                              |
|------------|-------------|--------|----------------------------------------------------|
| `identity` | none        | -      | copies the content image; pipeline test            |
| `johnson`  | preset      | fast   | Johnson et al. 2016 feed-forward nets, bundled styles (stage 2) |
| `adain`    | image       | fast   | Huang & Belongie 2017, arbitrary style (stage 3)   |
| `sd`       | image       | slow   | SD 1.5 + IP-Adapter img2img via stable-diffusion.cpp (stage 5) |

## Command line

```
pastiche <content> <style> <out> <algo> [-p key=value ...] [--size N] [--tile]
         [--backend cpu|dml|cuda] [--models-dir D] [--threads N] [-v]
pastiche --list-algos
pastiche --help [algo]
pastiche --selftest
pastiche --benchmark
```

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

Linux: builds with the same CMake project (CPU or CUDA execution provider);
see `BUILD-linux.md` once available.

## Models

Only permissively licensed weights ship with the release (`models/`). Others
are downloaded on demand (`pastiche download <name>`, planned) with their
licence shown first. Model conversion scripts live in `tools/`.

## Licence

MIT, see `LICENSE`. Third-party components and model licences are listed in
`THIRD-PARTY-LICENSES.md`.
