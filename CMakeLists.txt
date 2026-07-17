# =============================================================================
# xbox360-recompiler — root CMake build file
#
# A CLI-first (headless) application that automates the Spider-Man 3 (Xbox 360)
# recompilation pipeline using the RexGlue360 SDK.
#
# Toolchain: clang-cl + MSVC linker on Windows (matches the game build.bat):
#     cmake -G Ninja -S . -B out\build\release ^
#         -DCMAKE_BUILD_TYPE=Release ^
#         -DCMAKE_PREFIX_PATH="<RexGlue360Recomp root>" ^
#         -DCMAKE_C_COMPILER=clang-cl ^
#         -DCMAKE_CXX_COMPILER=clang-cl
#     cmake --build out\build\release --parallel
#
# The RexGlue360Recomp prebuilt tree provides:
#   include/spdlog/       - spdlog headers (consumed via find_package(spdlog))
#   include/toml++/       - toml++ single-header (toml.hpp)
#   lib/cmake/spdlog/     - spdlog CMake config package
#   lib/cmake/fmt/        - fmt CMake config package (spdlog dep)
# The inja single-header is vendored from the SDK source tree's
# thirdparty/inja/single_include/inja/inja.hpp into third_party/inja/ so the
# app can be configured without a network connection.
# =============================================================================
cmake_minimum_required(VERSION 3.25)

project(xbox360_recompiler
    VERSION 1.4.0
    LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output a single console executable; keep symbols for crash reports.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Treat warnings seriously on MSVC-like front-ends (clang-cl / cl).
if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC|Clang")
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    # /W4 + the project's own code is clean; spdlog/toml++ headers emit a few
    # benign warnings, so silence the noisiest ones at the target level below.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-Wno-unused-parameter -Wno-unused-variable)
    endif()
endif()

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------

# REXGLUE_SDK_ROOT may be supplied directly, or derived from CMAKE_PREFIX_PATH.
# This points at the *prebuilt* SDK (RexGlue360Recomp) — the app does not link
# the SDK's runtime; it shells out to rexglue.exe / cmake / ninja instead. We
# only need the vendored third-party headers (spdlog, toml++) that ship there.
if(NOT REXGLUE_SDK_ROOT)
    foreach(_p ${CMAKE_PREFIX_PATH})
        if(EXISTS "${_p}/include/spdlog/spdlog.h" AND EXISTS "${_p}/include/toml++/toml.hpp")
            set(REXGLUE_SDK_ROOT "${_p}")
            break()
        endif()
    endforeach()
endif()

if(NOT REXGLUE_SDK_ROOT OR NOT EXISTS "${REXGLUE_SDK_ROOT}/include/spdlog/spdlog.h")
    message(FATAL_ERROR
        "RexGlue360 SDK root not found. Point CMAKE_PREFIX_PATH or "
        "-DREXGLUE_SDK_ROOT at the prebuilt SDK (the 'RexGlue360Recomp' dir "
        "containing include/spdlog and include/toml++).")
endif()

message(STATUS "RexGlue360 SDK root: ${REXGLUE_SDK_ROOT}")

# spdlog is consumed via find_package(spdlog CONFIG) below — no header-only
# cache var needed (the config package sets the correct compile definitions).

# inja — single-header, vendored into third_party/inja/ (header-only).
# Vendoring is required: the prebuilt SDK does not ship inja headers, and the
# patch/profile stages need inja templating. The header is copied from the SDK
# source tree's thirdparty/inja/single_include/inja/inja.hpp at configure time
# if it is not already present.
set(INJA_HEADER_PATH "${CMAKE_SOURCE_DIR}/third_party/inja/inja.hpp")
if(NOT EXISTS "${INJA_HEADER_PATH}")
    set(_inja_src_candidates
        "${REXGLUE_SDK_ROOT}/../rexglue-sdk-0.8.0 Source code/thirdparty/inja/single_include/inja/inja.hpp"
        "${REXGLUE_SDK_ROOT}/source/thirdparty/inja/single_include/inja/inja.hpp"
        "$ENV{REXGLUE_SDK_SOURCE}/thirdparty/inja/single_include/inja/inja.hpp"
    )
    foreach(_c ${_inja_src_candidates})
        if(EXISTS "${_c}")
            file(COPY "${_c}" DESTINATION "${CMAKE_SOURCE_DIR}/third_party/inja/")
            message(STATUS "Vendored inja from: ${_c}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${INJA_HEADER_PATH}")
    message(WARNING
        "inja single-header not found. The patch/profile stages will fail to "
        "compile until third_party/inja/inja.hpp is vendored (see CMakeLists "
        "notes). The core framework still builds.")
endif()

# -----------------------------------------------------------------------------
# Application executable
# -----------------------------------------------------------------------------

# Collect sibling-module .cpp files (deps/, profile/, stages/) via GLOB so the
# app links as those modules are written by their owning agents. Re-configure
# picks up new files. Core sources are listed explicitly.
file(GLOB_RECURSE _sibling_srcs
    "${CMAKE_SOURCE_DIR}/src/deps/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/iso/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/gui/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/profile/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/stages/*.cpp"
)

add_executable(xbox360-recompiler
    src/main.cpp
    src/core/json_mini.cpp
    src/core/pipeline_context.cpp
    src/core/state_store.cpp
    src/core/orchestrator.cpp
    src/core/process_runner.cpp
    src/core/logger.cpp
    src/gui/app.rc
    ${_sibling_srcs}
)

target_include_directories(xbox360-recompiler PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${REXGLUE_SDK_ROOT}/include"          # spdlog/, toml++/
    "${CMAKE_SOURCE_DIR}/third_party"      # inja/inja.hpp
    "${CMAKE_SOURCE_DIR}/third_party/webview2/include"   # WebView2.h
)

# App version — surfaced to the GUI via the check_deps bridge response.
target_compile_definitions(xbox360-recompiler PRIVATE
    RECOMP_APP_VERSION="${PROJECT_VERSION}")

# Recompile the resource whenever the built UI changes (rc.exe does not
# emit depfiles for RCDATA payloads).
set_source_files_properties(src/gui/app.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/gui/dist/index.html")

# Make find_package(spdlog CONFIG) find the config package at
# <SDK>/lib/cmake/spdlog (and fmt at <SDK>/lib/cmake/fmt).
list(APPEND CMAKE_PREFIX_PATH "${REXGLUE_SDK_ROOT}")

# spdlog — use the CMake config package shipped in the SDK (lib/cmake/spdlog).
# find_package resolves the correct compile definitions (SPDLOG_FMT_EXTERNAL=ON)
# and pulls fmt via find_dependency, so we do NOT set SPDLOG_HEADER_ONLY or
# SPDLOG_FMT_EXTERNAL ourselves.
find_package(spdlog CONFIG REQUIRED)
target_link_libraries(xbox360-recompiler PRIVATE spdlog::spdlog)

# Console subsystem on Windows (clang-cl/MSVC).
if(WIN32)
    target_compile_definitions(xbox360-recompiler PRIVATE
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC|Clang")
        target_link_options(xbox360-recompiler PRIVATE /SUBSYSTEM:CONSOLE)
    endif()
endif()
# Link the Windows process/job APIs used by ProcessRunner.
if(WIN32)
    target_link_libraries(xbox360-recompiler PRIVATE
        advapi32      # CreateProcess, JobObjects
        user32        # window management, message loop
        comdlg32      # file picker dialogs (GetOpenFileNameW)
        comctl32      # progress bar controls
        ole32         # COM (WebView2, IFileOpenDialog)
        shell32       # SHGetKnownFolderPath, ShellExecute
        shlwapi       # path helpers
        dwmapi        # dark titlebar
        version       # WebView2LoaderStatic dependency
        "${CMAKE_SOURCE_DIR}/third_party/webview2/lib/WebView2LoaderStatic.lib"
    )
endif()

# Install target for a portable distribution layout.
include(GNUInstallDirs)
install(TARGETS xbox360-recompiler
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
