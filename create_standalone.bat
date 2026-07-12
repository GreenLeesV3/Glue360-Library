@echo off
REM ============================================================================
REM  create_standalone.bat — assemble a fully standalone xbox360-recompiler
REM  distribution that needs only VS 2022 ("Desktop development with C++")
REM  The SDK must be the D3D12 build (REX_HAS_D3D12=1, links dxc-headers).
REM
REM  Layout produced:
REM    standalone/
REM      xbox360-recompiler.exe       <- the app
REM      profiles/                    <- game profiles
REM      patches/                     <- shared + game-specific patches
REM      third_party/inja/inja.hpp    <- vendored inja header
REM      sdk/                         <- RexGlue prebuilt SDK
REM      tools/
REM        ninja.exe                  <- bundled Ninja
REM        cmake/bin/cmake.exe        <- bundled CMake portable
REM        extract-xiso.exe           <- bundled extract-xiso
REM
REM  Usage:
REM    create_standalone.bat          Release build (default)
REM    create_standalone.bat debug    Debug build
REM ============================================================================
setlocal enabledelayedexpansion

set "BUILD_TYPE=Release"
if /I "%~1"=="debug" set "BUILD_TYPE=Debug"

REM --- Resolve script + repo directories -------------------------------------
set "REPO_DIR=%~dp0"
if "%REPO_DIR:~-1%"=="\" set "REPO_DIR=%REPO_DIR:~0,-1%"
set "STANDALONE_DIR=%REPO_DIR%\standalone"

REM --- Toolchain locations (override via env) --------------------------------
if not defined SDK_ROOT set "SDK_ROOT="

REM --- 1. Build the recompiler app -------------------------------------------
echo === BUILDING XBOX360-RECOMPILER (%BUILD_TYPE%) ===
call "%REPO_DIR%\build.bat" %BUILD_TYPE%
if errorlevel 1 exit /b 1

set "APP_EXE=%REPO_DIR%\build\xbox360-recompiler.exe"
if not exist "%APP_EXE%" (
    echo FAILED: built exe not found at "%APP_EXE%"
    exit /b 1
)

REM --- 2. Wipe and recreate standalone dir -----------------------------------
echo.
echo === ASSEMBLING STANDALONE DISTRIBUTION ===
if exist "%STANDALONE_DIR%" rmdir /S /Q "%STANDALONE_DIR%"
mkdir "%STANDALONE_DIR%"

REM --- 3. Copy the app exe + profiles + patches + third_party ----------------
echo Copying xbox360-recompiler.exe...
copy /Y "%APP_EXE%" "%STANDALONE_DIR%\" >nul

echo Copying profiles/...
xcopy /E /I /Q /Y "%REPO_DIR%\profiles" "%STANDALONE_DIR%\profiles" >nul

if exist "%REPO_DIR%\patches" (
    echo Copying patches/...
    xcopy /E /I /Q /Y "%REPO_DIR%\patches" "%STANDALONE_DIR%\patches" >nul
)

if exist "%REPO_DIR%\third_party\inja\inja.hpp" (
    echo Copying third_party/inja/...
    mkdir "%STANDALONE_DIR%\third_party\inja" 2>nul
    copy /Y "%REPO_DIR%\third_party\inja\inja.hpp" "%STANDALONE_DIR%\third_party\inja\" >nul
)

REM --- 4. Copy RexGlue prebuilt SDK -> sdk/ ----------------------------------
echo Copying RexGlue SDK from "%SDK_ROOT%"...
if not exist "%SDK_ROOT%\bin\rexglue.exe" goto :sdk_missing
xcopy /E /I /Q /Y "%SDK_ROOT%\bin" "%STANDALONE_DIR%\sdk\bin" >nul
xcopy /E /I /Q /Y "%SDK_ROOT%\cmake" "%STANDALONE_DIR%\sdk\cmake" >nul
xcopy /E /I /Q /Y "%SDK_ROOT%\include" "%STANDALONE_DIR%\sdk\include" >nul
xcopy /E /I /Q /Y "%SDK_ROOT%\lib" "%STANDALONE_DIR%\sdk\lib" >nul
if exist "%SDK_ROOT%\share" xcopy /E /I /Q /Y "%SDK_ROOT%\share" "%STANDALONE_DIR%\sdk\share" >nul
goto :sdk_done

:sdk_missing
echo FAILED: RexGlue SDK not found at "%SDK_ROOT%"
echo        Set SDK_ROOT to the prebuilt SDK dir with bin\rexglue.exe, include\, lib\.
exit /b 1

:sdk_done

REM --- 4b. Do NOT trim debug/relwithdebinfo lib variants ---------------------
REM The SDK's CMake config files (fmt-targets.cmake, spdlog-targets.cmake,
REM rexglueConfig.cmake) reference *d.lib and *rd.lib for all configurations.
REM Trimming them breaks find_package(rexglue CONFIG) at game configure time.
REM Keep all .lib variants — the size cost (~200MB) is outweighed by correctness.

REM Do NOT trim debug DLLs from sdk/bin/ — rexglueTargets.cmake references
REM rexruntimed.dll, rexruntimerd.dll, TracyClientd.dll, TracyClientrd.dll
REM for all configurations. Trimming them breaks find_package(rexglue CONFIG).

REM --- 5. Copy bundled tools -------------------------------------------------
mkdir "%STANDALONE_DIR%\tools" 2>nul

REM extract-xiso
if exist "%REPO_DIR%\tools\extract-xiso.exe" (
    echo Copying tools/extract-xiso.exe...
    copy /Y "%REPO_DIR%\tools\extract-xiso.exe" "%STANDALONE_DIR%\tools\" >nul
) else (
    echo WARNING: tools\extract-xiso.exe not found in repo - add manually.
)

REM Ninja — resolve via where, copy first hit
echo Copying tools/ninja.exe...
where ninja >nul 2>nul
if errorlevel 1 goto :ninja_missing
for /f "delims=" %%i in ('where ninja 2^>nul') do (
    copy /Y "%%i" "%STANDALONE_DIR%\tools\ninja.exe" >nul
    echo   from %%i
    goto :ninja_done
)
:ninja_missing
echo WARNING: ninja not found on PATH - add manually.
:ninja_done

REM CMake — copy the whole portable tree (bin/ + share/)
echo Copying tools/cmake/...
where cmake >nul 2>nul
if errorlevel 1 goto :cmake_missing
for /f "delims=" %%i in ('where cmake 2^>nul') do (
    set "CMAKE_BIN_DIR=%%~dpi"
    goto :cmake_resolve
)
:cmake_resolve
for %%i in ("!CMAKE_BIN_DIR!\..") do set "CMAKE_ROOT=%%~fi"
echo   CMake root: !CMAKE_ROOT!
xcopy /E /I /Q /Y "!CMAKE_ROOT!\bin" "%STANDALONE_DIR%\tools\cmake\bin" >nul
if exist "!CMAKE_ROOT!\share" xcopy /E /I /Q /Y "!CMAKE_ROOT!\share" "%STANDALONE_DIR%\tools\cmake\share" >nul
goto :cmake_done
:cmake_missing
echo WARNING: cmake not found on PATH - add manually.
:cmake_done

REM --- 5b. Generate README.txt -----------------------------------------------
echo Writing README.txt...
(
    echo Glue360 Library - Xbox 360 Recompiler (Standalone Distribution)
    echo ===================================================================
    echo.
    echo This tool recompiles Xbox 360 games into native Windows
    echo executables using the RexGlue360 SDK.
    echo.
    echo Requirements
    echo ------------
    echo The only external requirements are:
    echo   1. Visual Studio 2022 with "Desktop development with C++"
    echo      (provides MSVC + Windows SDK)
    echo   2. LLVM/clang-cl (install separately)
    echo.
    echo Everything else (RexGlue SDK, CMake, Ninja, extract-xiso) is
    echo bundled in this folder.
    echo.
    echo Available game profiles
    echo -----------------------
    echo   spiderman3       Spider-Man 3 (D3D12, FSR, save system)
    echo   jurassic_hunted  Jurassic: The Hunted (D3D12, 60 FPS unlock)
    echo.
    echo Usage
    echo -----
    echo   xbox360-recompiler.exe --iso ^<game.iso^> --output ^<dir^> --profile ^<name^>
    echo.
    echo Verify your environment
    echo -----------------------
    echo   xbox360-recompiler.exe --check-deps
    echo.
    echo For full documentation see HOW_TO_USE.txt in the source repo
    echo or visit: https://github.com/GreenLeesV3/Glue360-Library
) > "%STANDALONE_DIR%\README.txt"

REM --- 6. Report -------------------------------------------------------------
echo.
echo === STANDALONE DISTRIBUTION READY ===
echo Location: %STANDALONE_DIR%
echo.
echo Contents:
dir /B "%STANDALONE_DIR%"
echo.
echo Verify with:
echo   "%STANDALONE_DIR%\xbox360-recompiler.exe" --check-deps
echo.
echo The only external requirement is Visual Studio 2022 with
echo "Desktop development with C++" which provides MSVC + Windows SDK.
echo LLVM/clang-cl also needs to be installed separately.
exit /b 0
