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

# Stable across rebuilding the same sources, unlike PE/ELF file hashes. Changes
# trigger reconfiguration so an old extension cannot claim the current checkout.
file(GLOB_RECURSE _simulation_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/rts_core/*.cpp" "${CMAKE_SOURCE_DIR}/rts_core/*.hpp"
    "${CMAKE_SOURCE_DIR}/game/src/*.cpp" "${CMAKE_SOURCE_DIR}/game/include/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")
list(SORT _simulation_sources)
set(_simulation_fingerprint "${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION}")
foreach(_source IN LISTS _simulation_sources)
    file(SHA256 "${_source}" _source_sha)
    file(RELATIVE_PATH _relative "${CMAKE_SOURCE_DIR}" "${_source}")
    string(APPEND _simulation_fingerprint "\n${_relative}:${_source_sha}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_simulation_sources})
string(SHA256 _simulation_sha "${_simulation_fingerprint}")
target_compile_definitions(rts_native PRIVATE
    RTS_SIMULATION_FINGERPRINT="${_simulation_sha}" RTS_NATIVE_BUILD_MODE="$<CONFIG>")

# 链接 `game` 是为了复用 `MapLoader` / `StatsLoader` / `make_world_init`
# ——绑定层因此不必暴露 `WorldInit` 的几十个字段（见 module.cpp 文件头）。
# `game` 在默认构建里，所以这条依赖不把任何新东西拖进 GCC 编译面。
target_link_libraries(rts_native PRIVATE rts_core game)
if(RTS_WITH_ONNX)
    rts_copy_onnx_runtime(rts_native)
endif()

# **这里原以为警告设置会由 `rts_core` 的 INTERFACE 传下来，是错的**：
# `rts_set_warnings()` 用 `target_compile_options(... PRIVATE ...)`（见
# `cmake/CompilerWarnings.cmake`），`PRIVATE` 按定义不通过 `target_link_libraries`
# 传播。缺这一行的实测后果不是"警告变少"，是**编译直接失败**——本文件里的
# `/utf-8` 从未生效，MSVC 在中文 code page（936）下把本仓库头文件的中文注释
# 当 GBK 解析，字节错位到语法都不成立（`rts/types.hpp` 报一串 C4430/C2059）。
# GCC 侧从来没暴露这个问题，因为 Linux 的 UTF-8 环境不存在"code page"这个概念——
# 这正是"两套工具链的分歧尽早在本地暴露"那条设计要防的事，结果它自己漏了一次。
rts_set_warnings(rts_native)
set_target_properties(rts_native PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bindings")

message(STATUS "bindings: Python3 = ${Python3_EXECUTABLE} (${Python3_VERSION})")

# Explicit opt-in: normal game builds don't require PyTorch. When enabled, a
# missing torch must fail configuration rather than silently skip learner tests.
option(RTS_TEST_TRAINING "Run real CPU PyTorch training and restart tests" OFF)
set(RTS_TRAIN_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH "Python with torch, numpy and the binding's ABI")
if(RTS_TEST_TRAINING)
    if(NOT RTS_BUILD_TESTS)
        message(FATAL_ERROR "RTS_TEST_TRAINING requires RTS_BUILD_TESTS")
    endif()
    execute_process(COMMAND "${RTS_TRAIN_PYTHON}" -c "import torch, numpy"
                    RESULT_VARIABLE _training_python_ok)
    if(NOT _training_python_ok EQUAL 0)
        message(FATAL_ERROR "RTS_TRAIN_PYTHON must provide torch and numpy")
    endif()
    foreach(_training_test ppo_smoke rl_runtime rl_suite campaign_binding macro_policy macro_train macro_evaluate macro_imitation macro_compare)
        add_test(NAME "${_training_test}"
                 COMMAND "${RTS_TRAIN_PYTHON}" "${CMAKE_SOURCE_DIR}/tests/${_training_test}_test.py")
        set_tests_properties("${_training_test}" PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:rts_native>;PYTHONIOENCODING=utf-8"
            TIMEOUT 180 RUN_SERIAL TRUE)
    endforeach()
endif()
