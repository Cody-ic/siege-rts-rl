# siege-rts-rl 顶层构建配置
#
# 设计依据见 CLAUDE.md「架构：三层隔离」与「确定性要求」。这里只解释与那两节
# 直接对应的构建选择，不重复文档内容。

cmake_minimum_required(VERSION 3.20)

# 「用户显式给过构建类型没有」这个信息**只在 project() 之前存在**，所以先记一笔。
#
# project() 会经 Modules/CMakeInitializeConfigs.cmake 把平台默认值写进缓存，而
# Modules/Platform/Windows-MSVC.cmake 把那个默认值定成 Debug。于是在 project() 之后，
# 「用户没给」和「平台给了 Debug」这两种情况长得一模一样，任何写法都分不开。
#
# 这条曾经写错过：判空的守卫放在 project() 之后，于是它**只在 Linux/GCC 上有效**
# （那边平台默认值是空），而在全组每天用的 Windows 上是死代码——`cmake -B build`
# 拿到的是 Debug。当时没发现是因为验证只走了 VS 多配置生成器这条路，那条分支本来就该跳过。
if(DEFINED CMAKE_BUILD_TYPE)
    set(_rts_build_type_given TRUE)
endif()

project(siege_rts_rl
        VERSION 0.1.0
        DESCRIPTION "不对称波次生存 RTS：玩家守城，RL 驱动攻方"
        LANGUAGES CXX)

# 默认 Release。CLAUDE.md 要求性能相关改动一律在 Release 下测量，Debug 构建的仿真
# 吞吐不具参考意义。
#
# 判「多配置」放在 project() 之后，因为 CMAKE_CONFIGURATION_TYPES 也是那时才有值；
# 判「用户给过」用上面那个哨兵，所以显式的 -DCMAKE_BUILD_TYPE=Debug 不会被 FORCE 顶掉
# （复配时缓存里的值在 project() 前就已 DEFINED，因此也不会回弹成 Release）。
if(NOT _rts_build_type_given AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 关掉编译器扩展（GNU/MSVC 方言）。全组本地是 MSVC、训练在服务器 GCC，而 MSVC 是三大
# 编译器中与标准分歧最多的一个；关扩展能让两套工具链的分歧尽早在本地暴露，而不是等
# 推到服务器才第一次发现。
set(CMAKE_CXX_EXTENSIONS OFF)

option(RTS_BUILD_TESTS  "构建测试（Catch2 经 FetchContent 拉取）" ON)
option(RTS_BUILD_RENDER "构建前端 render/" OFF)
option(RTS_WARNINGS_AS_ERRORS "把警告当错误（仅第一方代码）" ON)
# 默认 ON：找不到 Python3 时让配置失败，而不是把确定性守卫静默摘掉。
# 「守卫不在」必须是一个需要主动声明的决定，理由见 tests/CMakeLists.txt。
option(RTS_REQUIRE_DETERMINISM_GUARD "找不到 Python3 时让配置失败（而非静默跳过确定性守卫）" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(CompilerWarnings)

add_subdirectory(rts_core)

# 不变量 2 与 3 的构建侧落地：RTS_BUILD_RENDER 默认 OFF，于是训练构建**根本不配置**
# render/ 这个子目录——不是"编译了但不链接"，而是它连 CMake 层面都不参与。
# 这条同时缩小了需要在服务器 GCC 上编译的代码面。
if(RTS_BUILD_RENDER)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/render/CMakeLists.txt")
        add_subdirectory(render)
    else()
        message(FATAL_ERROR "RTS_BUILD_RENDER=ON，但 render/ 尚未建立。前端还没开工。")
    endif()
endif()

if(RTS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
