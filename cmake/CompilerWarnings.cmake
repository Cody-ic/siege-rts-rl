# 编译选项集中在这一个文件里，每条都写明理由。
#
# 其中浮点相关的几条是 CLAUDE.md「确定性要求」的硬性规定，不是口味问题：
# 确定性回放测试是本项目主要的防 bug 手段，任何允许编译器重排浮点求值顺序的开关
# 都会让同一份输入产出不同结果，从而毁掉回放测试。

# rts_set_warnings(<target> [TESTS])
#
# 只对第一方 target 调用。第三方依赖（Catch2）保持其默认选项，且其头文件按 SYSTEM
# 引入——否则 /W4 下第三方头会刷出一片警告，把我们自己的问题淹掉。
#
# 传 TESTS 时会去掉隐式转换类警告：Catch2 的断言宏在调用处展开，会产生大量此类噪声。
function(rts_set_warnings target)
    set(strict_conversions ON)
    if("${ARGV1}" STREQUAL "TESTS")
        set(strict_conversions OFF)
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            # 关掉 MSVC 的宽松写法。MSVC 默认接受一批非标准代码（缺失的 typename、
            # 绑定右值到非常量引用等），这些代码到服务器 GCC 上会直接编译失败。
            /permissive-
            # 不加这条 __cplusplus 恒为 199711L，所有基于它的特性检测全部失效。
            /Zc:__cplusplus
            /Zc:preprocessor
            # 显式写出 precise。**绝不可换成 /fp:fast**——它允许重排浮点运算，
            # 同一表达式在不同优化下得到不同结果，回放立刻不可复现。
            /fp:precise
            # 源码是 UTF-8（注释含中文）。不加这条在中文 code page 下会报 C4819，
            # 且 /WX 会把它变成错误。
            /utf-8
        )
        if(strict_conversions)
            # 与 GCC 侧 -Wconversion -Wsign-conversion 对齐的那一组。这类转换是确定性
            # 问题的常见来源（float 意外降精度、索引类型截断）。逐条写清是什么，
            # 因为这份清单曾经和注释对不上（见下）：
            #   C4242  赋值中的隐式窄化
            #   C4254  位域转换丢位
            #   C4287  无符号/负常量不匹配
            #   C4365  有符号/无符号不匹配 —— **-Wsign-conversion 的 MSVC 对应物**
            # 另有 C4244 / C4267（隐式窄化的另两种形态）由 /W4 自带，不必列。
            #
            # C4365 是 off-by-default 的，必须显式抬到 W1。此前注释点了它的名却没开，
            # 于是 MSVC 侧比 GCC 侧松：本地干净，而服务器上 -Wsign-conversion + -Werror
            # 会直接编译失败——正是「关扩展 + 严格 MSVC 让两套工具链的分歧尽早在本地暴露」
            # 这套设计要防的那件事，结果它自己漏了。
            #
            # 同时删掉曾经列在这里的 C4263 / C4265（成员函数没覆盖基类虚函数、
            # 有虚函数但析构非虚）：本项目从结构上禁止虚函数分派，它们永远不会触发。
            target_compile_options(${target} PRIVATE /w14242 /w14254 /w14287 /w14365)
        endif()
        if(RTS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow
            # 禁 FMA 合并。GCC 在 C++ 下默认 -ffp-contract=fast，会把 a*b+c 合成一条
            # FMA 指令，其中间结果不舍入，于是同一份代码在支持/不支持 FMA 的机器上
            # 结果不同。**注意：任何情况下都不要加 -ffast-math。**
            -ffp-contract=off
        )
        if(strict_conversions)
            target_compile_options(${target} PRIVATE -Wconversion -Wsign-conversion)
        endif()
        if(RTS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
