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
# 离线时加 -DFETCHCONTENT_FULLY_DISCONNECTED=ON 复用已下载的副本。
FetchContent_Declare(Catch2
    URL      https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
    URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c
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
)

target_link_libraries(rts_tests PRIVATE rts_core Catch2::Catch2WithMain)
rts_set_warnings(rts_tests TESTS)

# 按**标签**注册 ctest 条目，而不是用 catch_discover_tests 逐用例注册。
#
# 这条是踩过才知道的：用例名是中文，而 catch_discover_tests 的做法是解析
# `--list-tests` 的输出、再把用例名当命令行过滤器传回去。在 Windows 的中文 code page
# 下这条链路会把名字弄乱，ctest 于是报「No tests ran」——**测试被静默跳过**，
# 这比测试失败危险得多。标签是 ASCII，不受影响。
#
# 三个标签正好划分全部用例（每个 TEST_CASE 都带且只带其中一个）。若某个标签匹配不到
# 任何用例，Catch2 会以非零码退出，ctest 会红——所以"标签写错导致漏跑"也是可检出的。
#
# 挑单条用例跑仍按 CLAUDE.md 写的那样直接用二进制：
#     ./build/tests/Release/rts_tests "<用例名>"
foreach(_tag rng hash types)
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
# 代价可忽略：现有 5 条总耗时不到半秒。标签条目留着，用于快速定位是哪一块坏了。
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
    add_test(NAME determinism_bans
             COMMAND "${Python3_EXECUTABLE}" "${_checker}" "${CMAKE_SOURCE_DIR}/rts_core")
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
    # selftest.py 覆盖的是各条检查函数，够不到 validate.py 的 main()，
    # 而「目录改名后校验器什么都没校验却全绿」正是要防的那种失败。
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
