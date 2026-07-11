@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "SDK_ROOT=C:\tmp\Glue360-Library-v1.2-standalone\sdk"
cd /d "C:\tmp\Workspace 1\xbox360-recompiler-v1.0\xbox360-recompiler"
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%SDK_ROOT%" -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
echo === CONFIGURE EXIT: %ERRORLEVEL% ===
if errorlevel 1 exit /b 1
cmake --build build --parallel
echo === BUILD EXIT: %ERRORLEVEL% ===
