#!/usr/bin/env bash
# Build Pastiche and produce a self-contained portable ZIP for Windows.
# Run from the repo root inside an MSYS2 MinGW-w64 shell (or Git Bash with
# MSYS2 on PATH). Requires a configured build directory (see README.md).
set -euo pipefail

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

NAME="pastiche"
# Version is single-sourced from CMakeLists.txt project()
VERSION="$(sed -n 's/^project(pastiche VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[ -n "${VERSION}" ] || { echo "ERROR: could not read version from CMakeLists.txt"; exit 1; }
DIST="${NAME}-${VERSION}-win64"
BUILD_DIR="${BUILD_DIR:-build}"
ZIP_NAME="${DIST}.zip"
ORT_DIR="${ORT_DIR:-third_party/onnxruntime}"

echo "=== Building ${NAME} v${VERSION} ==="
cmake --build "${BUILD_DIR}" -- -j"$(nproc)"

echo "=== Collecting files into ${DIST}/ ==="
rm -rf "${DIST}"
mkdir -p "${DIST}/models" "${DIST}/out"

EXES=("${BUILD_DIR}/${NAME}.exe")
[ -f "${BUILD_DIR}/${NAME}-gui.exe" ] && EXES+=("${BUILD_DIR}/${NAME}-gui.exe")
for exe in "${EXES[@]}"; do cp "${exe}" "${DIST}/"; done
cp LICENSE THIRD-PARTY-LICENSES.md README.md "${DIST}/"
cp models/README.md "${DIST}/models/"
# Bundled models with permissive licences only (see DECYZJE.md D8).
for f in models/*.onnx; do [ -f "$f" ] && cp "$f" "${DIST}/models/"; done
[ -f models.json ] && cp models.json "${DIST}/"

# Copy every MinGW DLL the executables depend on (skip Windows system DLLs).
for exe in "${EXES[@]}"; do
    ldd "${exe}" \
        | grep -i '/mingw64/' \
        | awk '{print $3}' \
        | sort -u \
        | while read -r dll; do
            if [ ! -f "${DIST}/$(basename "${dll}")" ]; then
                echo "  + $(basename "${dll}")"
                cp "${dll}" "${DIST}/"
            fi
        done
done

# ONNX Runtime + DirectML (loaded at run time, not linked, so ldd does not see them).
if [ -d "${ORT_DIR}/bin" ]; then
    for dll in onnxruntime.dll onnxruntime_providers_shared.dll DirectML.dll; do
        if [ -f "${ORT_DIR}/bin/${dll}" ]; then
            echo "  + ${dll}"
            cp "${ORT_DIR}/bin/${dll}" "${DIST}/"
        fi
    done
else
    echo "WARNING: ${ORT_DIR}/bin not found - ONNX Runtime DLLs not bundled"
fi

echo "=== Creating ${ZIP_NAME} ==="
python -c "
import zipfile, os, sys

dist  = sys.argv[1]
zname = sys.argv[2]

with zipfile.ZipFile(zname, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
    for root, dirs, files in os.walk(dist):
        for f in sorted(files):
            path = os.path.join(root, f)
            zf.write(path, path.replace(os.sep, '/'))
        if not files and not dirs:
            zf.writestr(root.replace(os.sep, '/') + '/', '')
" "${DIST}" "${ZIP_NAME}"

SIZE=$(python -c "import os; s=os.path.getsize('${ZIP_NAME}'); print(f'{s/1024/1024:.1f} MB')")
echo ""
echo "=== Done ==="
echo "  ${ZIP_NAME}  (${SIZE})"
echo ""
echo "Contents of ${DIST}/:"
ls -1 "${DIST}/"
