# game：玩家侧的非渲染部分。目前只有「关卡加载」与「场景装配」两块。
#
# **这个 target 无条件构建，包括在训练服务器上**，与 render/ 不同。理由是测试：
# render/ 默认不配置（RTS_BUILD_RENDER=OFF），挂在它下面的测试在默认构建里根本不存在、
# ctest 照样全绿。而 `地图与场景设计.md` 4.2.1 那几条规则正是「只值一行代码但错了
# 才发现」的那种，必须落在一个始终被测的地方。
#
# 代价是它进入了 GCC 侧的编译面。这个代价是小的：纯 C++ + 一个 header-only 的 JSON 库，
# 不链接任何图形库。换来的是那几条规则在两套工具链上都被测到。

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# nlohmann/json：header-only，符合 CLAUDE.md「优先 header-only，并用 FetchContent
# 引入，不要求队友在系统里预装」。
#
# 只拉**单头文件**（DOWNLOAD_NO_EXTRACT），不拉整个仓库：920 KB 对 42 MB。
# 我们只需要 json::parse，不需要它的 CMake 包、测试与文档。
#
# 两个 URL + URL_HASH + TIMEOUT 的写法照抄 tests/CMakeLists.txt，理由完全相同：
# 训练服务器到 github.com 的连通性是**间歇性**的（#39 实测），而 codeload 与 raw
# 同期正常。两个 URL 给出的字节我已比对过，SHA256 完全相同——兜底的安全性由
# URL_HASH 保证，不靠信任那个域名。**换版本时两个 URL 要一起改。**
FetchContent_Declare(nlohmann_json_single
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
             https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp
    URL_HASH SHA256=9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6
    TIMEOUT  20
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(nlohmann_json_single)

# 单头文件下载下来是平铺的，摆成 <nlohmann/json.hpp> 的形状，
# 这样将来若改用完整发行版，include 一行都不用动。
set(_json_inc "${CMAKE_CURRENT_BINARY_DIR}/third_party")
file(MAKE_DIRECTORY "${_json_inc}/nlohmann")
file(COPY "${nlohmann_json_single_SOURCE_DIR}/json.hpp"
     DESTINATION "${_json_inc}/nlohmann")

add_library(nlohmann_json_hdr INTERFACE)
# SYSTEM：不这么标，/W4 与 -Wconversion 下第三方头会刷出一片警告，
# 而 /WX 与 -Werror 会把它们变成错误——我们自己代码里的问题反而被淹掉。
target_include_directories(nlohmann_json_hdr SYSTEM INTERFACE "${_json_inc}")

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
    ${_game_self_contained_srcs}
)

target_include_directories(game PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")

# rts_core 是 PUBLIC：game 的公开头用了 rts::GridPos。
# JSON 是 PRIVATE：**依赖到 map_loader.cpp 为止**，这正是本模块存在的意义之一
# ——rts_core 与 render/ 都只看见 MapData，都不必引入 JSON。
target_link_libraries(game PUBLIC rts_core PRIVATE nlohmann_json_hdr)

rts_set_warnings(game)

# 测试要读夹具地图，把路径作为编译期常量给它，避免依赖 ctest 的工作目录。
set(GAME_TESTDATA_DIR "${CMAKE_CURRENT_LIST_DIR}/testdata" PARENT_SCOPE)
