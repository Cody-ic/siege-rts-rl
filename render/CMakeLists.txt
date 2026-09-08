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
    src/menu_view.cpp
    src/chronicle_view.cpp
    src/battle_atmosphere.cpp
    ${_render_self_contained_srcs}
)

target_include_directories(render_lib PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(render_lib
    PUBLIC  game raylib
    PRIVATE nlohmann_json_hdr)
rts_set_warnings(render_lib)

add_executable(rts_render src/main.cpp src/cli_entry.cpp)
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
    add_executable(weapon_probe src/weapon_probe.cpp)
    target_link_libraries(weapon_probe PRIVATE render_lib)
    rts_set_warnings(weapon_probe)
    add_test(NAME render_weapon_shots COMMAND weapon_probe
        "${CMAKE_SOURCE_DIR}/game/data/demo_skirmish.json"
        "${CMAKE_SOURCE_DIR}/game/data/stats_placeholder.json"
        "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d"
        "${CMAKE_CURRENT_BINARY_DIR}/weapon")
    add_test(NAME render_range_shot COMMAND rts_render --battle --inspect 80,94
        --map "${CMAKE_SOURCE_DIR}/game/data/maps/pool/gen_01001000.json"
        --stats "${CMAKE_SOURCE_DIR}/game/data/stats_placeholder.json"
        --sprites "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d"
        --screenshot "${CMAKE_CURRENT_BINARY_DIR}/range.png" --size 1500 1000)
    add_test(NAME render_flak_range_shot COMMAND rts_render --battle --inspect 76,76
        --map "${CMAKE_SOURCE_DIR}/game/data/maps/pool/gen_01001000.json"
        --stats "${CMAKE_SOURCE_DIR}/game/data/stats_placeholder.json"
        --sprites "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d"
        --screenshot "${CMAKE_CURRENT_BINARY_DIR}/flak-range.png" --size 1500 1000)
    add_test(NAME render_smoke
             COMMAND rts_render
                     --map "${GAME_TESTDATA_DIR}/fixture_min.json"
                     --sprites "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d"
                     --screenshot "${CMAKE_CURRENT_BINARY_DIR}/smoke.png"
                     --size 640 360)

    # ——主菜单那一屏，**并且不给任何路径参数**——
    #
    # 两件事一次测掉，而它们都是「双击 exe」这条路径独有的：
    #
    #   1. **素材自动发现**（`game::discover_assets`）。命令行下每个人都给全了
    #      三条路径，所以这条代码路径**只有双击的人会走**——也就是最不会有人
    #      发现它坏了的那一条。
    #   2. **菜单与说明的文案有没有登记进字体码点集合**。漏登记的表现是画到它的
    #      那一帧直接抛（render/text.hpp 第 2 个坑），而字体构造期就会检查全部
    #      覆盖串，所以这条即使只画主菜单，也把「说明屏那十几行」一并验了。
    #
    # `WORKING_DIRECTORY` 显式设成仓库根：发现是从工作目录向上找的，而**构建目录
    # 完全可以在仓库之外**（`cmake -B ../build`）。不设的话这条测试会在那种布局下
    # 无端变红，而红的原因与它要测的东西无关。
    add_test(NAME render_menu_defaults
             COMMAND rts_render --menu
                     --screenshot "${CMAKE_CURRENT_BINARY_DIR}/menu.png"
                     --size 900 560)
    set_tests_properties(render_menu_defaults PROPERTIES
                         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # 操作说明那一屏。**它是版面最容易崩的一屏**（两栏 + 十几行 + 底部还有一个
    # 菜单项），而玩家进它的路径是「按 Esc 再选一项」——没有这条 ctest，
    # 它坏了要等有人真的去点才发现。
    add_test(NAME render_help_shot
             COMMAND rts_render --screen help
                     --screenshot "${CMAKE_CURRENT_BINARY_DIR}/help.png"
                     --size 1100 700)
    set_tests_properties(render_help_shot PROPERTIES
                         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # 屏名拼错必须红。少了它，`--screen hlep` 会安静地拍一张主菜单——
    # 而那张图看起来完全正常，正是「该红却绿」。
    add_test(NAME render_bad_screen_is_red
             COMMAND rts_render --screen no_such_screen
                     --screenshot "${CMAKE_CURRENT_BINARY_DIR}/never_screen.png"
                     --size 64 64)
    set_tests_properties(render_bad_screen_is_red PROPERTIES
                         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" WILL_FAIL TRUE)

    # 对局那一屏：推 200 tick（10 秒，够走完建造阶段、生出第一波）再拍。
    # 覆盖的是 HUD 与实体绘制这条链路——`render_smoke` 走的是地图查看器，
    # 一个单位都没有。
    add_test(NAME render_battle_shot
             COMMAND rts_render --battle --ticks 200
                     --map "${CMAKE_SOURCE_DIR}/game/data/demo_skirmish.json"
                     --stats "${CMAKE_SOURCE_DIR}/game/data/stats_placeholder.json"
                     --sprites "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d"
                     --screenshot "${CMAKE_CURRENT_BINARY_DIR}/battle.png"
                     --size 900 560)

    # 素材目录写错时必须红。同 determinism_bans_empty_is_red 与
    # map_gen_validate_empty_is_red：**「什么都没画却退出 0」是最坏的那种绿。**
    add_test(NAME render_missing_assets_is_red
             COMMAND rts_render
                     --map "${GAME_TESTDATA_DIR}/fixture_min.json"
                     --sprites "${CMAKE_CURRENT_BINARY_DIR}/no_such_sprite_dir"
                     --screenshot "${CMAKE_CURRENT_BINARY_DIR}/never.png"
                     --size 64 64)
    set_tests_properties(render_missing_assets_is_red PROPERTIES WILL_FAIL TRUE)

    # 素材校验：元数据声明的每一张精灵都必须真的载入得上。
    #
    # 与 tools/sprite_gen/check_assets.py **互补而非重复**：那个脚本查 assets.json
    # 的模型路径与主体共用，不看渲染产物；这条查渲染产物，而且跑的是
    # **C++ 读取器那一侧**的文件名规则。两侧各自的检查都抓不到「两侧约定漂移」，
    # 而换素材包时那正是最容易发生的事——两边都自认为对。
    #
    # #41 把素材从 300 张扩到 476 张（新增 attack / work 状态）之后，
    # 「声明了某状态但没渲出来」这块地带就不再是可以不管的了：
    # 没有这条，它最早暴露的时机是动画第一次播到那个状态。
    add_test(NAME render_verify_assets
             COMMAND rts_render --verify-assets
                     --sprites "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d")

    # ——含非 ASCII 字符的路径——
    #
    # raylib 的文件读写**全部**走窄 `fopen`（`utils.c` 的 `LoadFileData` /
    # `SaveFileData`，已核源码：零处 `_wfopen`），所以 `LoadTexture` / `LoadFontEx` /
    # `ExportImage` 在路径含中文时会失败。本目录因此改走「自己读字节 + 内存版 API」，
    # 而这条 ctest 是那件事的回归测试。病因见 `rts/utf8_path.hpp`。
    #
    # 一次跑通三条 I/O：读地图（`game/`）、读精灵与元数据、**写**截图 PNG。
    # 三条都在中文目录里。
    #
    # 做成 ctest 而不是手跑：从 Git Bash 传含中文的 argv 会被 MSYS 的命令行转换弄坏
    # （实测程序根本起不来，退出码 127），而 CMake 写进 ctest 的参数不经 shell。
    set(_u8 "${CMAKE_CURRENT_BINARY_DIR}/中文路径测试")
    file(MAKE_DIRECTORY "${_u8}")
    # `configure_file(... COPYONLY)` 而不是 `file(COPY)`：前者把输入登记进重新配置
    # 的依赖表。理由与踩过的那一次见 `tests/CMakeLists.txt` 同一处。
    configure_file("${GAME_TESTDATA_DIR}/fixture_min.json"
                   "${_u8}/fixture_min.json" COPYONLY)
    # 复制**真的**元数据 + 全部 `idle` 精灵（140 张，约 8 MB），而不是手写一份裁剪版。
    #
    # 手写过一版，两个问题：一是要在 CMake 里抄 canvas / ground_anchor 的数字，
    # 那**直接违反 §7「像素几何只有一个来源」**；二是抄多少个实体取决于夹具地图
    # 用到哪些地形，夹具一改这条测试就报「元数据里没有标识符 X」。
    #
    # 只取 `idle` 而不是全部 476 张：这条测试渲的是一张静态地图，
    # `SceneRenderer::preload` 也只载入 idle。`attack` / `move` 那些由
    # `render_verify_assets` 覆盖（那条走真目录，不需要复制）。
    file(GLOB _u8_idle "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d/*_idle_*.png")
    file(COPY ${_u8_idle} DESTINATION "${_u8}")
    configure_file("${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d/_sprite_meta.json"
                   "${_u8}/_sprite_meta.json" COPYONLY)
    # 资源点标记（`decal_*.png`）不在元数据里、也不匹配上面那条 `*_idle_*` 的
    # glob——`register_decal_image()` 直接从 `sprite_dir` 拼路径读它们
    # （main.cpp 的 `register_resource_decals`），复制不到就是这条测试自己在
    # 复现「换了个精灵目录、忘了带资源点贴图」这种真实失败，得单独补一条。
    file(GLOB _u8_decal "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d/decal_*.png")
    file(COPY ${_u8_decal} DESTINATION "${_u8}")

    add_test(NAME render_utf8_path
             COMMAND rts_render
                     --map "${_u8}/fixture_min.json"
                     --sprites "${_u8}"
                     --screenshot "${_u8}/截图.png"
                     --size 320 200)

    # 上面那条的防退化。**一条只会绿的检查等于没有检查**——同
    # determinism_bans_empty_is_red 与 map_gen_validate_empty_is_red。
    #
    # 夹具目录里声明了两张精灵、一张 PNG 都没有，所以 `--verify-assets` 必须非零退出。
    # 它刻意带真的 px_per_tile / tile，为的是**通过前面几道检查、走到载入 PNG 这一步
    # 才失败**——否则这条测试会因为「元数据格式不对」而红，看着一样绿，
    # 实际根本没验到我们想验的那一步。
    add_test(NAME render_verify_assets_missing_is_red
             COMMAND rts_render --verify-assets
                     --sprites "${CMAKE_CURRENT_LIST_DIR}/testdata/declared_but_missing")
    set_tests_properties(render_verify_assets_missing_is_red PROPERTIES WILL_FAIL TRUE)

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

if(RTS_BUILD_TESTS)
    foreach(_chapter RANGE 0 7)
        add_test(NAME render_journal_${_chapter}
            COMMAND rts_render --menu --journal-page ${_chapter}
                --screenshot "${CMAKE_CURRENT_BINARY_DIR}/journal_${_chapter}.png" --size 1100 700)
        add_test(NAME render_journal_bottom_${_chapter}
            COMMAND rts_render --menu --journal-page ${_chapter} --journal-bottom
                --screenshot "${CMAKE_CURRENT_BINARY_DIR}/journal_bottom_${_chapter}.png" --size 960 540)
    endforeach()
    foreach(_ending RANGE 1 2)
        add_test(NAME render_journal_ending_${_ending}
            COMMAND rts_render --menu --journal-page 7 --journal-ending ${_ending} --journal-bottom
                --screenshot "${CMAKE_CURRENT_BINARY_DIR}/journal_ending_${_ending}.png" --size 960 540)
    endforeach()
    add_test(NAME render_journal_requires_screenshot COMMAND rts_render --journal-page 7)
    set_tests_properties(render_journal_requires_screenshot PROPERTIES WILL_FAIL TRUE)
    add_test(NAME render_journal_rejects_ninth COMMAND rts_render --journal-page 8 --screenshot never_ninth.png)
    set_tests_properties(render_journal_rejects_ninth PROPERTIES WILL_FAIL TRUE)
endif()

if(RTS_BUILD_TESTS)
    add_test(NAME render_developer_wave COMMAND rts_render --developer-wave 70
        --map "${CMAKE_SOURCE_DIR}/game/data/maps/pool/gen_01001000.json"
        --sprites "${CMAKE_SOURCE_DIR}/tools/sprite_gen/out_3d"
        --stats "${CMAKE_SOURCE_DIR}/game/data/stats_placeholder.json"
        --screenshot "${CMAKE_CURRENT_BINARY_DIR}/developer-wave.png" --size 1200 800)
    add_test(NAME render_developer_rejects_interactive_cli COMMAND rts_render --developer-wave 70)
    set_tests_properties(render_developer_rejects_interactive_cli PROPERTIES WILL_FAIL TRUE)
endif()

if(RTS_BUILD_TESTS)
    add_test(NAME render_developer_panel COMMAND rts_render --screen developer --screenshot "${CMAKE_CURRENT_BINARY_DIR}/developer-panel.png" --size 960 540)
    foreach(entry RANGE 0 24)
        add_test(NAME render_guide_${entry} COMMAND rts_render --guide-entry ${entry} --screenshot "${CMAKE_CURRENT_BINARY_DIR}/guide-${entry}.png" --size 960 540)
    endforeach()
endif()

if(RTS_BUILD_TESTS)
    foreach(res IN ITEMS 1920x1080 2559x1368 2560x1440 3840x2160 3440x1440)
        string(REPLACE "x" ";" dimensions "${res}")
        list(GET dimensions 0 width)
        list(GET dimensions 1 height)
        add_test(NAME render_guide_resolution_${res} COMMAND rts_render --guide-entry 15
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/guide-${res}.png" --size ${width} ${height})
    endforeach()
endif()

if(RTS_BUILD_TESTS)
    foreach(entry RANGE 0 24)
        add_test(NAME render_guide_level_${entry} COMMAND rts_render --guide-entry ${entry} --guide-level 10
            --screenshot "${CMAKE_CURRENT_BINARY_DIR}/guide-level-${entry}.png" --size 960 540)
    endforeach()
    add_test(NAME render_guide_level_costs COMMAND rts_render --guide-entry 8 --guide-level 10 --guide-bottom
        --screenshot "${CMAKE_CURRENT_BINARY_DIR}/guide-level-costs.png" --size 2559 1368)
    add_test(NAME render_guide_level_boundary COMMAND rts_render --guide-entry 0 --guide-level 9999 --guide-bottom
        --screenshot "${CMAKE_CURRENT_BINARY_DIR}/guide-level-boundary.png" --size 960 540)
endif()
