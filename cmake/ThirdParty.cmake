# 第三方依赖的集中声明。
#
# 为什么集中：`game/` 与 `render/` 都要读 JSON（前者读地图，后者读 `_sprite_meta.json`）。
# 把声明留在 `game/CMakeLists.txt` 里的话，`render/` 能用它只是因为顶层的
# `add_subdirectory` 顺序恰好是 game 在前——那是一条隐式依赖，改一次顺序就断，
# 而断掉时的报错（「找不到 target nlohmann_json_hdr」）不会指向真正的原因。
#
# Catch2 仍留在 `tests/` 里，因为只有它一个 target 用得到，且它随
# RTS_BUILD_TESTS 一起开关。

include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# nlohmann/json：header-only，符合 CLAUDE.md「优先 header-only，并用 FetchContent
# 引入，不要求队友在系统里预装」。
#
# 只拉**单头文件**（DOWNLOAD_NO_EXTRACT），不拉整个仓库：920 KB 对 42 MB。
# 我们只用 json::parse，不需要它的 CMake 包、测试与文档。
#
# 两个 URL + URL_HASH + TIMEOUT 的写法照抄 tests/CMakeLists.txt，理由完全相同：
# 训练服务器到 github.com 的连通性是**间歇性**的（#39 实测），而 codeload 与 raw
# 同期正常。两个 URL 给出的字节已下载比对，SHA256 完全相同——兜底的安全性由
# URL_HASH 保证，不靠信任那个域名。**换版本时两个 URL 要一起改**，
# 否则兜底会拉到别的版本、被 URL_HASH 判错，而报错只说哈希不符、不说是哪个 URL 给的。
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
set(_rts_json_inc "${CMAKE_BINARY_DIR}/third_party_include")
file(MAKE_DIRECTORY "${_rts_json_inc}/nlohmann")
file(COPY "${nlohmann_json_single_SOURCE_DIR}/json.hpp"
     DESTINATION "${_rts_json_inc}/nlohmann")

add_library(nlohmann_json_hdr INTERFACE)
# SYSTEM：不这么标，/W4 与 -Wconversion 下第三方头会刷出一片警告，
# 而 /WX 与 -Werror 会把它们变成错误——我们自己代码里的问题反而被淹掉。
# 同 tests/CMakeLists.txt 对 Catch2 做的那一步。
target_include_directories(nlohmann_json_hdr SYSTEM INTERFACE "${_rts_json_inc}")
