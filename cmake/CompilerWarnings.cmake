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
            # C4242/C4244/C4267 隐式窄化；C4365 有符号/无符号不匹配。
            # 这类转换是确定性问题的常见来源（例如 float 意外降精度、索引类型截断）。
            target_compile_options(${target} PRIVATE /w14242 /w14254 /w14263 /w14265 /w14287)
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
