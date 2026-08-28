# render：正式前端。**不是调试视图**——CLAUDE.md 明写「工作量要按正式前端估，
# 预算 5–8 人日，不要按『画几个方块』来排期」。
#
# 三条与本文件直接相关的约定：
#
#   1. **不变量 2**：渲染层是只读观察者。这里的落地方式不是约定，是类型——
#      SceneRenderer 只接受一份绘制列表（格坐标 + 精灵标识符 + 深度键），
#      它根本拿不到可变的仿真状态。
#   2. **不变量 3**：训练回路里没有渲染器。顶层默认 RTS_BUILD_RENDER=OFF，
#      于是这个目录连 CMake 层面都不参与训练构建。
#   3. 因此 **render/ 不在训练服务器上编**（CLAUDE.md:163 明写这是为了缩小 GCC 侧
#      代码面）。它是全项目唯一不吃双工具链约束的模块——raylib 不是 header-only，
#      若要求它在 GCC 侧也调通，成本会大得多。
#
# 与之配套的取舍：那些**该被测**的规则（地形展开、深度排序、走向推导）刻意放在
# game/ 而不是这里，因为 render/ 默认不配置、挂在它下面的测试在默认构建里根本不存在、
# ctest 照样全绿。见 game/CMakeLists.txt 开头。

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# raylib。取**源码**而非官方预编译包：组内跨 VS2022 与 VS18/2026（见 #28），
# 预编译的 msvc16 二进制有 ABI 风险，而源码构建把这个问题整个消掉。
#
# 代价是首次配置要下 42 MB（归档里含约 30 MB 示例资源，无法只取源码）。
# 它进 build/_deps、已被 .gitignore 覆盖，且只下一次；已有本地副本时用
# -DFETCHCONTENT_SOURCE_DIR_RAYLIB=<路径> 完全不走网络，同 Catch2 那条。
#
# 两个 URL + URL_HASH + TIMEOUT 的写法与理由同 tests/CMakeLists.txt。
# 两个 URL 给出的字节已下载比对，SHA256 完全相同。**换版本时两个 URL 要一起改。**
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    URL      https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
             https://codeload.github.com/raysan5/raylib/tar.gz/refs/tags/5.5
    URL_HASH SHA256=aea98ecf5bc5c5e0b789a76de0083a21a70457050ea4cc2aec7566935f5e258e
    TIMEOUT  20
)
FetchContent_MakeAvailable(raylib)

# raylib 的头按 SYSTEM 引入。不这么标，/W4 /WX 下第三方头会刷出一片警告并变成错误，
# 我们自己代码里的问题反而被淹掉。同 tests/CMakeLists.txt 对 Catch2 做的那一步。
if(TARGET raylib)
    get_target_property(_raylib_inc raylib INTERFACE_INCLUDE_DIRECTORIES)
    if(_raylib_inc)
        set_target_properties(raylib PROPERTIES
                              INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_raylib_inc}")
    endif()
endif()

# 头文件自足性守卫。做法与理由见 rts_core/CMakeLists.txt 里同名的那一段。
# 这里额外有用：render/ 的公开头包含 raylib.h，而它在不同平台上拉进来的东西不同,
# 漏 include 很可能只在一侧暴露。
file(GLOB _render_public_headers CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_LIST_DIR}/include/render/*.hpp")

set(_render_self_contained_srcs "")
foreach(_hdr IN LISTS _render_public_headers)
    get_filename_component(_stem "${_hdr}" NAME_WE)
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/self_contained/${_stem}.cpp")
    file(GENERATE OUTPUT "${_gen}" CONTENT
"// 自动生成，勿手改。见 render/CMakeLists.txt「头文件自足性守卫」。
#include \"render/${_stem}.hpp\"

namespace render::self_contained {
int probe_${_stem}() { return 0; }
}  // namespace render::self_contained
")
    set_source_files_properties("${_gen}" PROPERTIES GENERATED TRUE)
    list(APPEND _render_self_contained_srcs "${_gen}")
endforeach()

# 逻辑放静态库、只留一个薄 main，是为了将来能给它挂测试或第二个入口
# （#16 的 `AI vs AI` 观战模式就是第二个入口）。
add_library(render_lib STATIC
    src/sprite_atlas.cpp
    src/scene_renderer.cpp
    src/camera_controller.cpp
    src/text.cpp
    src/scene_overlay.cpp
    ${_render_self_contained_srcs}
)

target_include_directories(render_lib PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(render_lib
    PUBLIC  game raylib
    PRIVATE nlohmann_json_hdr)
rts_set_warnings(render_lib)

add_executable(rts_render src/main.cpp)
target_link_libraries(rts_render PRIVATE render_lib)
rts_set_warnings(rts_render)

# 截图模式的冒烟测试。
#
# 它要 GL 上下文，所以只在 RTS_BUILD_RENDER=ON 时注册——而那意味着你本来就在
# 构建一个 GUI 程序、机器上有 GL。训练服务器不配置 render/，因此不受影响。
#
# 查的是「跑得通且真画出了东西」：退出码 0 + 导出的 PNG 存在且非空。
# 没有它的话，渲染层的任何回归都只能靠人开窗看才能发现。
if(RTS_BUILD_TESTS)
    add_test(NAME render_smoke
             COMMAND rts_render
                     --map "${GAME_TESTDATA_DIR}/fixture_min.json"
                     --sprites "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d"
                     --screenshot "${CMAKE_CURRENT_BINARY_DIR}/smoke.png"
                     --size 640 360)

    # 素材目录写错时必须红。同 determinism_bans_empty_is_red 与
    # map_gen_validate_empty_is_red：**「什么都没画却退出 0」是最坏的那种绿。**
    add_test(NAME render_missing_assets_is_red
             COMMAND rts_render
                     --map "${GAME_TESTDATA_DIR}/fixture_min.json"
                     --sprites "${CMAKE_CURRENT_BINARY_DIR}/no_such_sprite_dir"
                     --screenshot "${CMAKE_CURRENT_BINARY_DIR}/never.png"
                     --size 64 64)
    set_tests_properties(render_missing_assets_is_red PROPERTIES WILL_FAIL TRUE)

    # 字体路径写错时必须红。
    #
    # 单独一条而不是靠上面那条兜着：素材与字体是两条独立的失败路径，
    # 而字体那条的默认表现是**画面照常出来、只是中文变成别的字**
    # （render/text.hpp 列了三种），所以「字体坏了会不会红」必须自己被测一次。
    add_test(NAME render_bad_font_is_red
             COMMAND rts_render
                     --map "${GAME_TESTDATA_DIR}/fixture_min.json"
                     --sprites "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d"
                     --screenshot "${CMAKE_CURRENT_BINARY_DIR}/never_font.png"
                     --size 64 64
                     --font "${CMAKE_CURRENT_BINARY_DIR}/no_such_font.ttf")
    set_tests_properties(render_bad_font_is_red PROPERTIES WILL_FAIL TRUE)

    # **.ttc（字体集合）必须红。** 这是三个坑里最隐蔽的一个：raylib 解析不了 .ttc 时
    # 会静默退回内置字体，而 `texture.id != 0`，所以「载入成功」这条判断为真，
    # 中文却全变成 `?`。检出靠的是「字形数必须正好等于要的个数」，
    # 而那条逻辑很容易在将来某次重构里被当成多余的断言删掉——所以钉一条测试。
    #
    # 用 EXISTS 守住：这个路径是 Windows 特有的，而 CMake 不该假定平台。
    # 找不到就不注册这条，`ctest -N` 里少一条是可见的，不是静默跳过一次断言。
    if(EXISTS "C:/Windows/Fonts/msyh.ttc")
        add_test(NAME render_ttc_font_is_red
                 COMMAND rts_render
                         --map "${GAME_TESTDATA_DIR}/fixture_min.json"
                         --sprites "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d"
                         --screenshot "${CMAKE_CURRENT_BINARY_DIR}/never_ttc.png"
                         --size 64 64
                         --font "C:/Windows/Fonts/msyh.ttc")
        set_tests_properties(render_ttc_font_is_red PROPERTIES WILL_FAIL TRUE)
    endif()
endif()
