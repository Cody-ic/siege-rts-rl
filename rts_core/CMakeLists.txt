# rts_core：全部游戏逻辑。
#
# 硬性要求（CLAUDE.md 不变量 1 与 3）：headless。不链接任何图形库、不依赖 Python、
# 不在 tick 热路径内向上回调。它必须能在完全无 GUI 的训练服务器上跑完整局游戏。
# 因此这个 target 的依赖列表**应当长期保持为空**——往里加东西前先确认它不破坏这条。

# 头文件自足性守卫。
#
# 起因：types.hpp 用了 `operator<=> = default` 却没 include <compare>，而它一直编得过
# ——三个测试文件都先 include 了 <catch2/...>，把 <compare> 顺带拉了进来。这类漏
# include 只在**该头被单独包含**时才暴露，而那正是 PR 2 里 world.hpp / sim.hpp 要走的
# 路（它们会以 types.hpp 为第一个 include）。更坏的是 libstdc++ 与 MSVC STL 的内部
# 包含图不同，同一个漏 include 可能只在一个平台上炸，表现成"Windows 过、Linux 不过"。
#
# 做法：给 include/rts/ 下的每个头各自生成一个**只包含它自己**的 TU，编进本 target。
# 用 GLOB CONFIGURE_DEPENDS 而不是手写名单——名单会被忘掉，而"忘掉"正是要防的东西
# （同样的理由见 tests/CMakeLists.txt 里 rts_tests_all 那条）。
file(GLOB _rts_public_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/include/rts/*.hpp")

set(_rts_self_contained_srcs "")
foreach(_hdr IN LISTS _rts_public_headers)
    get_filename_component(_stem "${_hdr}" NAME_WE)
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/self_contained/${_stem}.cpp")
    # 留一个有外部链接的符号：纯 include 的 TU 会产出空目标文件，静态库打包时
    # MSVC 会报 LNK4221。
    file(GENERATE OUTPUT "${_gen}" CONTENT
"// 自动生成，勿手改。见 rts_core/CMakeLists.txt「头文件自足性守卫」。
// 这个 TU 只包含一个头，用来保证该头不依赖任何先行的 include。
#include \"rts/${_stem}.hpp\"

namespace rts::self_contained {
int probe_${_stem}() { return 0; }
}  // namespace rts::self_contained
")
    set_source_files_properties("${_gen}" PROPERTIES GENERATED TRUE)
    list(APPEND _rts_self_contained_srcs "${_gen}")
endforeach()

add_library(rts_core STATIC
    src/rng.cpp
    src/terrain.cpp
    src/fog.cpp
    src/stats.cpp
    src/world.cpp
    src/mechanics.cpp
    src/flow.cpp
    src/replay.cpp
    src/unit_behavior.cpp
    src/obs_pack.cpp
    src/batched_env.cpp
    ${_rts_self_contained_srcs}
)

target_include_directories(rts_core PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/include"
)

rts_set_warnings(rts_core)
