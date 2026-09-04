@echo off
setlocal
set "REPO_DIR=%CD%"
call D:/tools/BuildTools/VC/Auxiliary/Build/vcvars64.bat >nul 2>&1
cd /d "%REPO_DIR%"
set "CM=D:/tools/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
set "CT=D:/tools/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe"
"%CM%" -G Ninja -B build-Release2 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
"%CM%" --build build-Release2
if errorlevel 1 exit /b 1
"%CT%" --test-dir build-Release2 --output-on-failure -E "map_gen_self_test|map_gen_generate_smoke"
