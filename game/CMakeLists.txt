# game：玩家侧的非渲染部分。目前只有「关卡加载」与「场景装配」两块。
#
# **这个 target 无条件构建，包括在训练服务器上**，与 render/ 不同。理由是测试：
# render/ 默认不配置（RTS_BUILD_RENDER=OFF），挂在它下面的测试在默认构建里根本不存在、
# ctest 照样全绿。而 `地图与场景设计.md` 4.2.1 那几条规则正是「只值一行代码但错了
# 才发现」的那种，必须落在一个始终被测的地方。
#
# 代价是它进入了 GCC 侧的编译面。这个代价是小的：纯 C++ + 一个 header-only 的 JSON 库，
# 不链接任何图形库。换来的是那几条规则在两套工具链上都被测到。

# nlohmann/json 的声明在 cmake/ThirdParty.cmake，由顶层 include。
# 移出去的理由：render/ 也要读 JSON（`_sprite_meta.json`），留在这里的话它能用
# 只是因为 add_subdirectory 顺序恰好是 game 在前——那是一条隐式依赖。

# 头文件自足性守卫。做法与理由见 rts_core/CMakeLists.txt 里同名的那一段
# （types.hpp 曾漏 <compare> 却一直编得过，因为测试先 include 了 Catch2）。
# 用 GLOB CONFIGURE_DEPENDS 而不是手写名单——名单会被忘掉，而"忘掉"正是要防的东西。
file(GLOB _game_public_headers CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_LIST_DIR}/include/game/*.hpp")

set(_game_self_contained_srcs "")
foreach(_hdr IN LISTS _game_public_headers)
    get_filename_component(_stem "${_hdr}" NAME_WE)
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/self_contained/${_stem}.cpp")
    file(GENERATE OUTPUT "${_gen}" CONTENT
"// 自动生成，勿手改。见 game/CMakeLists.txt「头文件自足性守卫」。
#include \"game/${_stem}.hpp\"

namespace game::self_contained {
int probe_${_stem}() { return 0; }
}  // namespace game::self_contained
")
    set_source_files_properties("${_gen}" PROPERTIES GENERATED TRUE)
    list(APPEND _game_self_contained_srcs "${_gen}")
endforeach()

add_library(game STATIC
    src/map_data.cpp
    src/map_loader.cpp
    src/scene_model.cpp
    src/display_names.cpp
    src/world_builder.cpp
    src/stats_loader.cpp
    ${_game_self_contained_srcs}
)

target_include_directories(game PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")

# rts_core 是 PUBLIC：game 的公开头用了 rts::GridPos。
# JSON 是 PRIVATE：**依赖到 map_loader.cpp 为止**，这正是本模块存在的意义之一
# ——rts_core 与 render/ 都只看见 MapData，都不必引入 JSON。
target_link_libraries(game PUBLIC rts_core PRIVATE nlohmann_json_hdr)

rts_set_warnings(game)

# 测试要读夹具地图，把路径作为编译期常量给它，避免依赖 ctest 的工作目录。
#
# 用 `file(REAL_PATH)` 规范化一遍。这不是洁癖：从 Git Bash 调用 cmake 时
# `CMAKE_CURRENT_LIST_DIR` 可能是 MSYS 形式（`/Users/...`），而测试二进制是原生
# Windows 程序。REAL_PATH 会把它解成绝对真实路径。
#
# **但这一步不是那个 bug 的修法。** 真正让队友 4 条 ctest 变红的是**编码**而不是形式：
# 他的仓库在 `.../计算机程序设计大作业/`，而 MSVC 的窄路径按 ANSI 代码页解释、
# CMake 烤进来的是 UTF-8。规范化后仍然打不开。修法在 `rts/utf8_path.hpp`，
# 并由 `game_testdata_utf8_path` 那条 ctest 钉住（见 tests/CMakeLists.txt）。
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/testdata" _game_testdata)
set(GAME_TESTDATA_DIR "${_game_testdata}" PARENT_SCOPE)

# 数值表所在的数据目录（`stats_placeholder.json`）。与 testdata 分开：
# testdata 是**测试专用**夹具，data/ 是**交付物**的一部分（交付清单要求
# 「软件源代码包括输入文件」，JSON 数值表被 CLAUDE.md 点名列在其中）。
file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/data" _game_data)
set(GAME_DATA_DIR "${_game_data}" PARENT_SCOPE)
