@echo off
REM ============================================================================
REM  build.bat — build the xbox360-recompiler app itself.
REM
REM  Sets up the MSVC + LLVM/clang-cl toolchain, configures with CMake + Ninja,
REM  and builds the console executable. Mirrors the toolchain setup pattern used
REM  by the Spider-Man 3 game build (spiderman3/build.bat) and the runtime LTO
REM  build (rebuild_runtime_lto.bat).
REM
REM  Usage:
REM    build.bat            Release build (default)
REM    build.bat debug      Debug build
REM    build.bat clean      Wipe build/ then configure + build
REM
REM  Override the toolchain locations via env vars if your install differs:
REM    set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
REM    set LLVM_DIR=C:\Program Files\LLVM
REM    set SDK_ROOT=C:\Tools\RexGlue360Recomp
REM ============================================================================
setlocal enabledelayedexpansion

REM --- Build configuration ---------------------------------------------------
set "BUILD_TYPE=Release"
set "DO_CLEAN=0"
if /I "%~1"=="debug"   set "BUILD_TYPE=Debug"
if /I "%~1"=="release" set "BUILD_TYPE=Release"
if /I "%~1"=="clean"   (
    set "DO_CLEAN=1"
    if /I not "%~2"=="" if /I "%~2"=="debug" set "BUILD_TYPE=Debug"
)

REM --- Resolve script + repo directories -------------------------------------
set "REPO_DIR=%~dp0"
if "%REPO_DIR:~-1%"=="\" set "REPO_DIR=%REPO_DIR:~0,-1%"
set "BUILD_DIR=%REPO_DIR%\build"

REM --- Toolchain locations (override via env) --------------------------------
if not defined VCVARS   set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
if not defined LLVM_DIR set "LLVM_DIR=C:\Program Files\LLVM"
if not defined SDK_ROOT set "SDK_ROOT="

REM --- 1. MSVC environment ---------------------------------------------------
if not exist "%VCVARS%" (
    echo FAILED: vcvarsall.bat not found at "%VCVARS%"
    echo        Set VCVARS to your Visual Studio 2022 vcvarsall.bat path.
    exit /b 1
)
call "%VCVARS%" x64
if errorlevel 1 (
    echo FAILED: vcvarsall x64
    exit /b 1
)

REM --- 2. LLVM / clang-cl on PATH (quoted set survives spaces) --------------
if not exist "%LLVM_DIR%\bin\clang-cl.exe" (
    echo FAILED: clang-cl not found in "%LLVM_DIR%\bin"
    echo        Set LLVM_DIR to your LLVM install root.
    exit /b 1
)
set "PATH=%LLVM_DIR%\bin;%PATH%"

REM --- 3. Sanity check the toolchain ----------------------------------------
where clang-cl >nul 2>nul
if errorlevel 1 (
    echo FAILED: clang-cl not on PATH after env setup
    exit /b 1
)
where cmake >nul 2>nul
if errorlevel 1 (
    echo FAILED: cmake not found on PATH
    exit /b 1
)
where ninja >nul 2>nul
if errorlevel 1 (
    echo FAILED: ninja not found on PATH
    exit /b 1
)

echo.
echo === TOOLCHAIN ===
clang-cl --version
cmake --version | findstr /R "cmake version"
echo.

REM --- 4. Optional clean -----------------------------------------------------
if "%DO_CLEAN%"=="1" (
    echo === CLEAN ===
    if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
)

REM --- 4b. Check SDK_ROOT ---------------------------------------------------
if "%SDK_ROOT%"=="" (
    echo FAILED: SDK_ROOT is not set.
    echo        Set it to your RexGlue360 prebuilt SDK root, e.g.:
    echo        set SDK_ROOT=C:\Tools\RexGlue360Recomp
    echo        Or run: build.bat SDK_ROOT=C:\Tools\RexGlue360Recomp
    exit /b 1
)

REM --- 5. CMake configure ----------------------------------------------------
REM  CMAKE_PREFIX_PATH points at the RexGlue SDK root so find_package can locate
REM  tomlplusplus, spdlog, inja, SDL3, and the rexglue headers/libs.
echo === CMAKE CONFIGURE (%BUILD_TYPE%) ===
cmake -G Ninja ^
    -S "%REPO_DIR%" ^
    -B "%BUILD_DIR%" ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_PREFIX_PATH="%SDK_ROOT%" ^
    -DCMAKE_C_COMPILER=clang-cl ^
    -DCMAKE_CXX_COMPILER=clang-cl
if errorlevel 1 (
    echo FAILED: cmake configure
    exit /b 1
)

REM --- 6. Build --------------------------------------------------------------
echo.
echo === CMAKE BUILD ===
cmake --build "%BUILD_DIR%" --parallel
if errorlevel 1 (
    echo FAILED: cmake build
    exit /b 1
)

REM --- 7. Report -------------------------------------------------------------
echo.
echo === BUILD COMPLETE ===
dir "%BUILD_DIR%\*.exe" 2>nul
echo.
echo Executable: %BUILD_DIR%\xbox360-recompiler.exe
echo Run "%BUILD_DIR%\xbox360-recompiler.exe --help" for usage.
exit /b 0
