cmake_minimum_required(VERSION 3.20)
project(pastiche VERSION 0.1.0 LANGUAGES CXX)

# Single source of truth for the app version: generates version.hpp,
# package.sh greps the version from this file.
configure_file(src/version.hpp.in version.hpp @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(PASTICHE_BUILD_TESTS "Build doctest unit tests" ON)
option(PASTICHE_BUILD_GUI "Build pastiche-gui (Dear ImGui + SDL2)" ON)

# --- Image libraries (MSYS2: pacman -S mingw-w64-x86_64-{libpng,libjpeg-turbo,libwebp,libjxl})
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp)
pkg_check_modules(LIBJXL IMPORTED_TARGET libjxl)

# --- ONNX Runtime: headers from the Microsoft binary package, the DLL is loaded
# at run time (no import library needed). Default layout: third_party/onnxruntime/
# {include,bin}. Override with -DPASTICHE_ORT_DIR=...
set(PASTICHE_ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH
    "Directory with ONNX Runtime include/ and bin/ (onnxruntime.dll, DirectML.dll)")
if(NOT EXISTS "${PASTICHE_ORT_DIR}/include/onnxruntime_c_api.h")
    message(FATAL_ERROR "onnxruntime_c_api.h not found under ${PASTICHE_ORT_DIR}/include - see README.md")
endif()

# --- stable-diffusion.cpp: headers only. The DLL is built out of tree by
# tools/build_sdcpp.sh and loaded at run time, never linked (D20, D21).
# Missing headers disable diffusion instead of failing the build, the same way
# a missing libjxl disables JPEG XL.
set(PASTICHE_SD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stable-diffusion" CACHE PATH
    "Directory with stable-diffusion.cpp include/ and bin/ (see tools/build_sdcpp.sh)")
if(EXISTS "${PASTICHE_SD_DIR}/include/stable-diffusion.h")
    set(PASTICHE_HAVE_SD ON)
    message(STATUS "Diffusion support enabled (headers from ${PASTICHE_SD_DIR}/include)")
else()
    set(PASTICHE_HAVE_SD OFF)
    message(STATUS "Diffusion support disabled: stable-diffusion.h not found under "
                   "${PASTICHE_SD_DIR}/include - run tools/build_sdcpp.sh to enable it")
endif()

# --- pastiche-core: image I/O, EXIF, parameters, algorithm registry, ORT backend
add_library(pastiche-core STATIC
    src/core/image.cpp
    src/core/image_io.cpp
    src/core/exif.cpp
    src/core/fs.cpp
    src/core/params.cpp
    src/core/json.cpp
    src/core/sha256.cpp
    src/core/http.cpp
    src/core/models.cpp
    src/core/algorithm.cpp
    src/core/gpu_info.cpp
    src/core/tiling.cpp
    src/core/sidecar.cpp
    src/backends/ort_session.cpp
)
target_include_directories(pastiche-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}   # generated version.hpp
    ${PASTICHE_ORT_DIR}/include
)
target_compile_definitions(pastiche-core PRIVATE PASTICHE_ORT_DIR="${PASTICHE_ORT_DIR}/bin")

if(PASTICHE_HAVE_SD)
    target_sources(pastiche-core PRIVATE src/backends/sd_session.cpp)
    target_include_directories(pastiche-core PUBLIC ${PASTICHE_SD_DIR}/include)
    target_compile_definitions(pastiche-core PUBLIC HAVE_SD)
    target_compile_definitions(pastiche-core PRIVATE PASTICHE_SD_DIR="${PASTICHE_SD_DIR}/bin")
endif()
target_link_libraries(pastiche-core PUBLIC PNG::PNG JPEG::JPEG PkgConfig::WEBP)
target_compile_options(pastiche-core PUBLIC -Wall -Wextra)
if(WIN32)
    # winhttp: the model downloader (D19). Part of Windows, nothing to ship.
    target_link_libraries(pastiche-core PUBLIC shell32 dxgi dxguid winhttp)
else()
    target_link_libraries(pastiche-core PUBLIC ${CMAKE_DL_LIBS})
    # libcurl is the Linux equivalent; without it downloads report that they
    # are unavailable rather than failing to build (D10 keeps Linux best-effort).
    find_package(CURL)
    if(CURL_FOUND)
        target_compile_definitions(pastiche-core PRIVATE HAVE_CURL)
        target_link_libraries(pastiche-core PUBLIC CURL::libcurl)
    else()
        message(STATUS "libcurl not found - model downloads will be unavailable")
    endif()
endif()

if(LIBJXL_FOUND)
    message(STATUS "JPEG XL support enabled (libjxl ${LIBJXL_VERSION})")
    target_compile_definitions(pastiche-core PUBLIC HAVE_JXL)
    target_link_libraries(pastiche-core PUBLIC PkgConfig::LIBJXL)
else()
    message(STATUS "JPEG XL support disabled (libjxl not found)")
endif()

# --- pastiche-algos: one .cpp per algorithm, OBJECT library so that the
# REGISTER_ALGORITHM static registrars are always linked into both executables.
add_library(pastiche-algos OBJECT
    src/algos/identity.cpp
    src/algos/johnson.cpp
    src/algos/adain.cpp
)
target_link_libraries(pastiche-algos PUBLIC pastiche-core)

# --- pastiche: command-line tool
add_executable(pastiche
    src/main.cpp
    src/download.cpp
    src/selftest.cpp
    $<TARGET_OBJECTS:pastiche-algos>
)
target_link_libraries(pastiche PRIVATE pastiche-core)

# Development convenience: put the runtime DLLs next to the executables so
# `build/pastiche.exe` finds them without package.sh.
if(WIN32 AND EXISTS "${PASTICHE_ORT_DIR}/bin/onnxruntime.dll")
    set(PASTICHE_RUNTIME_DLLS "${PASTICHE_ORT_DIR}/bin/onnxruntime.dll")
    foreach(dll onnxruntime_providers_shared.dll DirectML.dll)
        if(EXISTS "${PASTICHE_ORT_DIR}/bin/${dll}")
            list(APPEND PASTICHE_RUNTIME_DLLS "${PASTICHE_ORT_DIR}/bin/${dll}")
        endif()
    endforeach()
    add_custom_command(TARGET pastiche POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PASTICHE_RUNTIME_DLLS} $<TARGET_FILE_DIR:pastiche>
        COMMENT "Copying ONNX Runtime DLLs next to pastiche.exe")
endif()

# Same convenience for the diffusion DLL. In a release it is a separate
# download (D20), but during development it belongs next to the executables.
if(WIN32 AND PASTICHE_HAVE_SD)
    foreach(dll libstable-diffusion.dll stable-diffusion.dll)
        if(EXISTS "${PASTICHE_SD_DIR}/bin/${dll}")
            add_custom_command(TARGET pastiche POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${PASTICHE_SD_DIR}/bin/${dll}" $<TARGET_FILE_DIR:pastiche>
                COMMENT "Copying ${dll} next to pastiche.exe")
        endif()
    endforeach()
endif()

# --- pastiche-gui: Dear ImGui + SDL2 + OpenGL 3.3, native file dialogs
if(PASTICHE_BUILD_GUI)
    include(FetchContent)
    # SDL2 - install via: pacman -S mingw-w64-x86_64-SDL2
    find_package(SDL2 REQUIRED)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)
    FetchContent_Declare(
        nfd
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG        v1.2.1
        GIT_SHALLOW    TRUE
    )
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nfd)

    set(IMGUI_SOURCES
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    add_executable(pastiche-gui
        src/guimain.cpp
        ${IMGUI_SOURCES}
        $<TARGET_OBJECTS:pastiche-algos>
    )
    target_include_directories(pastiche-gui PRIVATE
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    # OpenGL: opengl32 is Windows-only; use the imported target elsewhere (Linux)
    if(WIN32)
        set(OPENGL_LIBS opengl32)
    else()
        find_package(OpenGL REQUIRED)
        set(OPENGL_LIBS OpenGL::GL)
    endif()
    target_link_libraries(pastiche-gui PRIVATE
        pastiche-core
        SDL2::SDL2main
        SDL2::SDL2
        ${OPENGL_LIBS}
        nfd
    )
    # Hide the console window on Windows (SDL2main bridges WinMain -> main)
    if(WIN32)
        target_link_options(pastiche-gui PRIVATE -mwindows)
    endif()
endif()

# --- tests (doctest, MIT)
if(PASTICHE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.4.11
        GIT_SHALLOW    TRUE
    )
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)   # doctest 2.4.11 declares cmake_minimum_required(3.0)
    FetchContent_MakeAvailable(doctest)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_executable(pastiche-tests
        tests/test_main.cpp
        tests/test_params.cpp
        tests/test_exif.cpp
        tests/test_image.cpp
        tests/test_io.cpp
        tests/test_json.cpp
        tests/test_sha256.cpp
        $<TARGET_OBJECTS:pastiche-algos>
    )
    target_link_libraries(pastiche-tests PRIVATE pastiche-core doctest::doctest)
    enable_testing()
    add_test(NAME unit COMMAND pastiche-tests)
    add_test(NAME selftest COMMAND pastiche --selftest)
endif()
