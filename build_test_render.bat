@echo off
rem One-shot local build + test (Release, RENDER=ON - includes the render_*
rem ctest entries). Output dir: build-Render2.
rem
rem Usage:  cmd //c /absolute/path/build_test_render.bat
rem
rem Same absolute-path caveat as build_test.bat (relative filenames are
rem rejected by cmd.exe on this machine under a Chinese path). Comments are
rem ASCII for the same code-page reason (no chcp 65001 needed).
rem
rem raylib is fetched from GitHub on first configure (~42 MB). If that request
rem 502s / times out, download + verify + unpack it manually, then point
rem -DFETCHCONTENT_SOURCE_DIR_RAYLIB at it:
rem
rem   curl -sL -o /tmp/raylib.tar.gz https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
rem   sha256sum /tmp/raylib.tar.gz
rem   rem  expected aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e
rem   mkdir -p /tmp/raylib_src
rem   tar -xzf /tmp/raylib.tar.gz -C /tmp/raylib_src --strip-components=1
rem
rem The script below already points -DFETCHCONTENT_SOURCE_DIR_RAYLIB at that
rem path; if the network is fine, drop the -D argument and let it download.
rem
setlocal
set "REPO_DIR=%CD%"
call D:/tools/BuildTools/VC/Auxiliary/Build/vcvars64.bat >nul 2>&1
cd /d "%REPO_DIR%"
set "CM=D:/tools/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
set "CT=D:/tools/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/ctest.exe"
"%CM%" -G Ninja -B build-Render2 -DCMAKE_BUILD_TYPE=Release -DRTS_BUILD_RENDER=ON -DFETCHCONTENT_SOURCE_DIR_RAYLIB=C:/Users/42221/AppData/Local/Temp/raylib_src -DFETCHCONTENT_SOURCE_DIR_CATCH2=C:/Users/42221/AppData/Local/Temp/catch2_src
if errorlevel 1 exit /b 1
"%CM%" --build build-Render2
if errorlevel 1 exit /b 1
"%CT%" --test-dir build-Render2 --output-on-failure
