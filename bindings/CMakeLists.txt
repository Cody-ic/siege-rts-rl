# Python 绑定（不变量 3：in-process，不走 IPC）。
#
# **默认不构建**（顶层 `RTS_BUILD_BINDINGS` 默认 OFF），与 `render/` 同一条理由的
# 镜像：那一条是「训练构建不该配置前端」，这一条是「玩游戏与跑 ctest 的人不该
# 被要求装 Python 开发头」。全组在 Windows 上开发，多数人机器里没有 `Python.h`。
#
# 关键的一条：**它需要的是「带开发头的 Python」，而那未必是 `python3`。**
# 训练服务器上系统 `python3`（3.12）**没有** `Python.h`，而
# `/data0/am_data/miniforge3/bin/python`（同样 3.12）**有**，还自带
# torch 2.8.0+cu128 与 numpy。所以配置时要显式指路：
#
#   cmake -B build-py -DRTS_BUILD_BINDINGS=ON \
#         -DPython3_EXECUTABLE=/data0/am_data/miniforge3/bin/python
#
# 不指的话 `find_package(Python3 ... Development)` 会失败，而失败信息说的是
# 「找不到 Development 组件」——那句话读起来像「没装 Python」，
# 实际是「找到的那个 Python 没有开发头」。所以下面把这条写进报错里。

find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# pybind11 经 FetchContent，与 Catch2 / nlohmann 同一套路数（`CLAUDE.md`
# 「优先 header-only，并用 FetchContent/vendored 方式引入」）。
#
# `FETCHCONTENT_SOURCE_DIR_PYBIND11` 可指向本地副本离线复用——服务器上
# `~/shared/deps/pybind11-v3.0.1` 已经放好，`tools/server/build.sh` 会指它。
# **PyPI 在那台机器上慢到不可用（7 KB/s），而 codeload 有 766 KB/s**，
# 所以这里刻意走 GitHub 而不是 `pip install pybind11`。
include(FetchContent)
FetchContent_Declare(
    pybind11
    URL https://codeload.github.com/pybind/pybind11/tar.gz/refs/tags/v3.0.1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(pybind11)

pybind11_add_module(rts_native src/module.cpp)

# 链接 `game` 是为了复用 `MapLoader` / `StatsLoader` / `make_world_init`
# ——绑定层因此不必暴露 `WorldInit` 的几十个字段（见 module.cpp 文件头）。
# `game` 在默认构建里，所以这条依赖不把任何新东西拖进 GCC 编译面。
target_link_libraries(rts_native PRIVATE rts_core game)

# 第三方生成的代码不吃我们的 `-Werror`（同 `render/` 对 raylib 的处理）。
# 我们自己那一个 .cpp 仍然吃——它在 `target_sources` 里，警告设置由
# `rts_core` 的 INTERFACE 传下来。
set_target_properties(rts_native PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bindings")

message(STATUS "bindings: Python3 = ${Python3_EXECUTABLE} (${Python3_VERSION})")
