@echo off
rem One-shot local build + test (Release, RENDER=OFF - same default as the
rem training server). Output dir: build-Release2 (does NOT touch the existing
rem build-Release).
rem
rem Usage:  cmd //c /absolute/path/build_test.bat
rem
rem MUST be called with an absolute path (forward slashes, no quotes). On this
rem machine a relative filename is reported as "not an internal/external
rem command" even though dir can see it (unclear why; likely a code-page /
rem argument-parsing quirk when Git Bash hands the name to cmd.exe under a
rem Chinese path). Absolute path + forward slashes is the one form that works.
rem
rem Comments are ASCII on purpose: a UTF-8 .bat with Chinese comments gets
rem mangled by cmd.exe under the GBK code page unless chcp 65001 runs first.
rem
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
"%CT%" --test-dir build-Release2 --output-on-failure
