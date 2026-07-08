@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set PATH=C:\Program Files\LLVM\bin;%PATH%
cd "C:\tmp\Workspace 1\xbox360-recompiler"
echo === CONFIGURING ===
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:\tmp\Workspace 1\RexGlue360Recomp" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl 2>&1
echo === CONFIGURE EXIT: %ERRORLEVEL% ===
if %ERRORLEVEL% neq 0 exit /b 1
echo === BUILDING ===
cmake --build build --parallel 2>&1
echo === BUILD EXIT: %ERRORLEVEL% ===
if exist "build\xbox360-recompiler.exe" (
    echo === BUILD SUCCESS ===
    "build\xbox360-recompiler.exe" --help 2>&1
) else (
    echo === BUILD FAILED ===
)
