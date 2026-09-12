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

# --- pastiche-core: image I/O, EXIF, parameters, algorithm registry
add_library(pastiche-core STATIC
    src/core/image.cpp
    src/core/image_io.cpp
    src/core/exif.cpp
    src/core/fs.cpp
    src/core/params.cpp
    src/core/algorithm.cpp
)
target_include_directories(pastiche-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}   # generated version.hpp
)
target_link_libraries(pastiche-core PUBLIC PNG::PNG JPEG::JPEG PkgConfig::WEBP)
target_compile_options(pastiche-core PUBLIC -Wall -Wextra)
if(WIN32)
    target_link_libraries(pastiche-core PUBLIC shell32)
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
)
target_link_libraries(pastiche-algos PUBLIC pastiche-core)

# --- pastiche: command-line tool
add_executable(pastiche
    src/main.cpp
    src/selftest.cpp
    $<TARGET_OBJECTS:pastiche-algos>
)
target_link_libraries(pastiche PRIVATE pastiche-core)

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
        $<TARGET_OBJECTS:pastiche-algos>
    )
    target_link_libraries(pastiche-tests PRIVATE pastiche-core doctest::doctest)
    enable_testing()
    add_test(NAME unit COMMAND pastiche-tests)
    add_test(NAME selftest COMMAND pastiche --selftest)
endif()
