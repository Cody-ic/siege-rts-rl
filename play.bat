@echo off
rem 双击这个文件就能玩。
rem
rem 它只做一件事：在几个常见的构建目录里找到 rts_render.exe 并启动它。
rem 素材（地图 / 数值表 / 精灵）不用给——程序会从 exe 与工作目录逐级向上找仓库根，
rem 见 game/include/game/asset_paths.hpp。
rem
rem 两个刻意的写法：
rem   * chcp 65001：本文件是 UTF-8，不切码页的话下面的中文提示在 cmd 里是乱码。
rem     >nul 是因为 chcp 自己会打一行 "Active code page: 65001"。
rem   * start "" "%EXE%"：让游戏自己占一个新控制台，于是它能把那个黑框藏掉
rem     （见 render/src/cli_entry.cpp），本窗口也随之立刻退出。
rem     第一个空引号是 start 的窗口标题参数，不能省——省了它会把路径当标题。
chcp 65001 >nul
setlocal

set "HERE=%~dp0"
set "EXE="
for %%p in (
  "build\render\Release\rts_render.exe"
  "build\render\Debug\rts_render.exe"
  "build\render\rts_render.exe"
  "build-Release\render\rts_render.exe"
  "build-Debug\render\rts_render.exe"
) do if not defined EXE if exist "%HERE%%%~p" set "EXE=%HERE%%%~p"

if not defined EXE goto :nobuild
start "" "%EXE%"
exit /b 0

:nobuild
echo 还没构建出前端。先在仓库目录里跑这两条：
echo.
echo   cmake -B build -DRTS_BUILD_RENDER=ON
echo   cmake --build build --config Release
echo.
echo 命令行下 PATH 里通常没有 cmake，完整路径见 README.md「本地环境」。
echo.
pause
exit /b 1
