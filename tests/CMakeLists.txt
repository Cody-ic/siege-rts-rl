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

# 把 CLAUDE.md 的确定性禁令变成一条真的会红的测试，而不是只写在文档里。
# 两条：一条扫 rts_core/，一条是检查器自己的自检——后者防的是"检查器退化成永远绿"。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
    set(_checker "${CMAKE_SOURCE_DIR}/tools/check_determinism_bans.py")
    add_test(NAME determinism_bans
             COMMAND "${Python3_EXECUTABLE}" "${_checker}" "${CMAKE_SOURCE_DIR}/rts_core")
    add_test(NAME determinism_bans_self_test
             COMMAND "${Python3_EXECUTABLE}" "${_checker}" --self-test)
else()
    message(WARNING "未找到 Python3：确定性禁令检查不会作为测试运行。"
                    "手动跑 python tools/check_determinism_bans.py rts_core")
endif()
