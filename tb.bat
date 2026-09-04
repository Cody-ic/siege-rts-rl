@echo off
setlocal
set "REPO_DIR=%CD%"
call D:/tools/BuildTools/VC/Auxiliary/Build/vcvars64.bat >nul 2>&1
cd /d "%REPO_DIR%"
set "CM=D:/tools/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"
"%CM%" --build build-Release2 --target rts_tests
