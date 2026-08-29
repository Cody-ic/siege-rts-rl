include(FetchContent)

# CMP0135（CMake 3.24 起）：URL 下载解压后用解压时间而非归档内的时间戳，
# 否则 URL 变了但归档内时间戳更旧时不会重新构建。用 if(POLICY) 包住是因为顶层的
# cmake_minimum_required 是 3.20，而训练服务器上的 CMake 版本尚未确认——
# 不能为了一条警告就把最低版本抬到 3.24。
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# Catch2 经 FetchContent 引入，不要求队友在系统里预装（CLAUDE.md「关于第三方库」）。
# 用 URL + SHA256 而不是 GIT_TAG：tag 在理论上可以被移动，哈希不能，构建因此可复现。
# 离线时加 -DFETCHCONTENT_FULLY_DISCONNECTED=ON 复用已下载的副本；
# 已有本地副本时用 -DFETCHCONTENT_SOURCE_DIR_CATCH2=<路径> 完全不走网络。
#
# 第二个 URL 是兜底，`TIMEOUT` 是为了让兜底能在合理时间内接手。两者都来自
# 训练服务器上的实测（首次 Linux/GCC 构建）：那台机器到 `github.com` 的连通性是
# **间歇性**的（DNS 正常、`github.com:443` 的 TCP 直接不可达，而同期
# `codeload.github.com` 与 `pypi.org` 都通）。
#
# 不给 TIMEOUT 时，第一个 URL 不可达要等约 133 秒才轮到第二个（黑洞地址实测）；
# 给了 20 秒则 23 秒完成。而在网络正常的机器上这两处都不生效，行为一字不变。
#
# 兜底 URL 的安全性由 `URL_HASH` 保证，不靠信任那个域名：codeload 上那份归档
# 与 `github.com` 重定向过去的是同一个对象，已实测 SHA256 逐字节相同。
# **换 Catch2 版本时两个 URL 要一起改**，否则兜底会拉到旧版、被 URL_HASH 判错，
# 而报错信息只会说哈希不符、不会指出是哪个 URL 给的。
FetchContent_Declare(Catch2
    URL      https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
             https://codeload.github.com/catchorg/Catch2/tar.gz/refs/tags/v3.7.1
    URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c
    TIMEOUT  20
)
FetchContent_MakeAvailable(Catch2)

# 把 Catch2 的头文件标成 SYSTEM。不做这一步，/W4 下第三方头会刷出一片警告，
# 而 /WX 会把它们变成错误——我们自己代码里的问题反而被淹掉。
foreach(_t Catch2 Catch2WithMain)
    if(TARGET ${_t})
        get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
            set_target_properties(${_t} PROPERTIES
                                  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
        endif()
    endif()
endforeach()

add_executable(rts_tests
    rng_test.cpp
    hash_test.cpp
    types_test.cpp
    map_loader_test.cpp
    scene_model_test.cpp
    iso_projection_test.cpp
    display_names_test.cpp
    utf8_path_test.cpp
    roster_test.cpp
    action_test.cpp
    command_test.cpp
    obs_test.cpp
    terrain_test.cpp
    fog_test.cpp
    world_test.cpp
    replay_test.cpp
)

target_link_libraries(rts_tests PRIVATE rts_core game Catch2::Catch2WithMain)
rts_set_warnings(rts_tests TESTS)

# 夹具地图的路径作为编译期常量给测试，**不依赖 ctest 的工作目录**。
# 靠工作目录的话，从 build/ 里跑与从仓库根跑会得到不同结果，
# 而「找不到夹具」在某些写法下会退化成「跳过那条用例」——又是静默跳过。
target_compile_definitions(rts_tests PRIVATE
    GAME_TESTDATA_DIR="${GAME_TESTDATA_DIR}")

# ——中文路径的回归测试——
#
# 在**构建目录**里造一个名字含中文的目录，把夹具地图复制进去，再把那条路径
# 也烤成一个宏。于是「路径含非 ASCII 字符时能不能打开文件」变成一条受检约束，
# 而不是「取决于你把仓库放在哪儿」。
#
# 起因是一个真实缺陷：队友的仓库在 `.../计算机程序设计大作业/`，
# 他本地 4 条 ctest 全红，而同一份代码在我这边（纯 ASCII 路径）全绿。
# 症状是「打不开这个文件」而那个文件明明就在——见 rts/utf8_path.hpp。
#
# **刻意在构建目录里造，而不是往仓库里塞一个中文目录**：
#   * 仓库路径是每个人自己的，塞进去也只覆盖仓库内那一段
#   * 中文目录名进 git 会牵出 `core.quotepath`、跨平台文件名规范化一堆事
#   * 构建目录里造出来的目录名由 CMake 写、由 C++ 读，**两边都是 UTF-8**，
#     正好复现真实的那条链路
set(_utf8_probe "${CMAKE_CURRENT_BINARY_DIR}/中文路径测试")
file(MAKE_DIRECTORY "${_utf8_probe}")
file(COPY "${GAME_TESTDATA_DIR}/fixture_min.json" DESTINATION "${_utf8_probe}")
target_compile_definitions(rts_tests PRIVATE
    GAME_TESTDATA_UTF8_DIR="${_utf8_probe}")

# 按**标签**注册 ctest 条目，而不是用 catch_discover_tests 逐用例注册。
#
# 这条是踩过才知道的：用例名是中文，而 catch_discover_tests 的做法是解析
# `--list-tests` 的输出、再把用例名当命令行过滤器传回去。在 Windows 的中文 code page
# 下这条链路会把名字弄乱，ctest 于是报「No tests ran」——**测试被静默跳过**，
# 这比测试失败危险得多。标签是 ASCII，不受影响。
#
# 这些标签正好划分全部用例（每个 TEST_CASE 都带且只带其中一个）。若某个标签匹配不到
# 任何用例，Catch2 会以非零码退出，ctest 会红——所以"标签写错导致漏跑"也是可检出的。
#
# 挑单条用例跑仍按 CLAUDE.md 写的那样直接用二进制：
#     ./build/tests/Release/rts_tests "<用例名>"
foreach(_tag rng hash types map scene iso names utf8path roster action command obs
        terrain fog world replay)
    add_test(NAME rts_tests_${_tag} COMMAND rts_tests "[${_tag}]")
endforeach()

# 再注册一条**不带过滤器**的条目。
#
# 上面那份标签清单是一份白名单，而白名单只挡住了一半的错：标签**写错**能被抓到
# （Catch2 匹配不到任何用例会非零退出，实测 `rts_tests "[nosuchtag]"` 退出码 2），
# 但标签**没写进清单**抓不到——那条 ctest 条目根本不存在，ctest 仍然全绿。
#
# 这一条不是假设的风险：PR 2 会加 replay_test.cpp 与 determinism_test.cpp，
# 大概率带 [replay] / [determinism]。忘了改上面那行，就等于新写的回放测试一次都没跑过
# ——而回放测试是本项目主要的防 bug 手段，漏跑它的代价比漏跑一条单元测试大一个量级。
#
# 代价可忽略：整套 ctest 总耗时不到一秒。标签条目留着，用于快速定位是哪一块坏了。
#
# 这里刻意**不写条目总数**。原来写的是「现有 5 条」，而实际已经是 7 条——
# 条目会随每个 PR 增加，写死的数字等于保证它明天又错，而它对读者没有任何用处
# （要数就 `ctest -N`）。同一毛病在 CLAUDE.md 里也有一份，一并去掉了。
add_test(NAME rts_tests_all COMMAND rts_tests)

# 把 CLAUDE.md 的确定性禁令变成一条真的会红的测试，而不是只写在文档里。
# 两条：一条扫 rts_core/，一条是检查器自己的自检——后者防的是"检查器退化成永远绿"。
find_package(Python3 COMPONENTS Interpreter QUIET)

# 找不到 Python3 时**默认让配置失败**，而不是打一句 message(WARNING) 就过去。
#
# 原来的写法是后者，后果是：配置退出码 0、警告随日志翻页消失、ctest 从 5 条变 3 条，
# 一样全绿——**没有任何东西说守卫没了**。这和上面 catch_discover_tests 那段是同一条
# 原则（「测试被静默跳过比测试失败危险得多」），只是当初没把它应用到这个 Python 依赖上。
# 而最可能触发的地方恰好是那台至今没人编过的 Linux 服务器：`python3` 在不在 CMake
# 找得到的位置，取决于那台机器怎么配的。
#
# 现在「守卫不在」是一个需要主动声明的决定：确实没有 Python 的环境显式加
# -DRTS_REQUIRE_DETERMINISM_GUARD=OFF。
if(NOT Python3_FOUND AND RTS_REQUIRE_DETERMINISM_GUARD)
    message(FATAL_ERROR
            "未找到 Python3，确定性禁令检查无法注册为测试。\n"
            "  CLAUDE.md 的确定性禁令靠 tools/check_determinism_bans.py 变成会红的 ctest；"
            "少了它，违反禁令的代码会一路编过去，直到某次回放对不上。\n"
            "  确实没有 Python 的环境请显式关掉：-DRTS_REQUIRE_DETERMINISM_GUARD=OFF")
endif()

if(Python3_FOUND)
    set(_checker "${CMAKE_SOURCE_DIR}/tools/check_determinism_bans.py")
    # 扫 rts_core/ 与 game/ 两处。
    #
    # 加上 game/ 是 #20 的复核就留下的后续（「守卫目前只扫 rts_core/，应扩到
    # bindings/ 与 game/」），现在 game/ 存在了就一并做掉。它确实需要被扫：
    # 地图载入产出的是喂给仿真的地形数据，若哪天有人在这里用 `unordered_map`
    # 驱动一次遍历，症状会出现在仿真侧而根因在这里。
    #
    # `bindings/` 还不存在，等它建起来时再加进这一行。
    add_test(NAME determinism_bans
             COMMAND "${Python3_EXECUTABLE}" "${_checker}"
                     "${CMAKE_SOURCE_DIR}/rts_core" "${CMAKE_SOURCE_DIR}/game")
    add_test(NAME determinism_bans_self_test
             COMMAND "${Python3_EXECUTABLE}" "${_checker}" --self-test)

    # 检查器"扫到 0 个文件也返回绿"曾经是个洞：路径写错、目录改名、或把 rts_core/
    # 拆成子目录忘了改上面那行，这条 ctest 就静默通过、什么都没检查。脚本侧已改成失败，
    # 这条 WILL_FAIL 测试把它钉住——自检只覆盖 scan_text()，够不到 main() 这一层。
    set(_empty_probe "${CMAKE_CURRENT_BINARY_DIR}/determinism_empty_probe")
    file(MAKE_DIRECTORY "${_empty_probe}")
    add_test(NAME determinism_bans_empty_is_red
             COMMAND "${Python3_EXECUTABLE}" "${_checker}" "${_empty_probe}")
    set_tests_properties(determinism_bans_empty_is_red PROPERTIES WILL_FAIL TRUE)

    # tools/map_gen/ 的自检。与上面同一条原则：把 `地图与场景设计.md` 的规范
    # 变成会红的测试，而不是只留在文档里等人记得。
    #
    # 它自己也带一道防退化：跑完一条断言都没执行就判失败（脚本里的 count == 0），
    # 所以「用例表被清空」或「import 挂了」不会以全绿收场 —— 同 determinism 那条
    # 「扫到 0 个文件即失败」。
    #
    # 脚本用 sys.path[0]（脚本所在目录）互相 import，不依赖 ctest 的工作目录。
    add_test(NAME map_gen_self_test
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_SOURCE_DIR}/tools/map_gen/selftest.py")

    # 校验器的「一张地图都没读到必须红」。与上面那条 determinism 探针同一道钉子：
    # 「目录改名后校验器什么都没校验却全绿」正是要防的那种失败。
    #
    # 与之成对的「读到了一些、另一些路径不存在也必须红」**留在 selftest.py 里**
    # （它 patch sys.argv 直接调 validate.main()）。那一条在这里做不了：
    # 仓库现在一张地图文件都没有，所以「一张真图 + 一个坏路径」这个场景在 CLI
    # 层构造不出来，而少了那张真图，先命中的是「0 张即失败」——
    # **那样它就会因为错误的理由通过**，正是 selftest.py 开头那段要防的形态。
    set(_map_empty_probe "${CMAKE_CURRENT_BINARY_DIR}/map_gen_empty_probe")
    file(MAKE_DIRECTORY "${_map_empty_probe}")
    add_test(NAME map_gen_validate_empty_is_red
             COMMAND "${Python3_EXECUTABLE}"
                     "${CMAKE_SOURCE_DIR}/tools/map_gen/validate.py"
                     "${_map_empty_probe}")
    set_tests_properties(map_gen_validate_empty_is_red PROPERTIES WILL_FAIL TRUE)
else()
    message(WARNING "未找到 Python3：确定性禁令检查不会作为测试运行"
                    "（RTS_REQUIRE_DETERMINISM_GUARD=OFF）。"
                    "手动跑 python tools/check_determinism_bans.py rts_core")
endif()
