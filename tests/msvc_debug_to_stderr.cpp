// MSVC Debug 下的两类运行时诊断**默认都弹模态对话框**：
//
//   * `/RTC1` 的检查失败（「Run-Time Check Failure #2 - Stack around the
//     variable 'x' was corrupted」）
//   * 标准库的 `assert` / `_CrtDbgReport`（「xmemory line 209: invalid argument」）
//
// 人手边跑只是烦，**在 `ctest` 下它是永久挂住**——那条测试既不通过也不失败，
// 进程等一个永远不会来的点击。「挂住」比「失败」难查得多，而本仓库通篇在防
// 这一类静默（`determinism_bans_empty_is_red`、`map_gen_validate_empty_is_red`、
// `cmake_configure_is_current` 是同一条纪律）。
//
// 这里把两条都改成写 stderr 再退出。除了不挂住，还有一个实际收益：
// **RTC 的回调本来就收到 filename 与 line，默认对话框却只显示变量名**——
// 而变量名往往不够定位（同名局部变量可能出现在十几个用例里，且真正越界的
// 常常是它**旁边**那个变量）。
//
// **两条路互不覆盖，必须都装**：`/RTC` 走 `_RTC_SetErrorFuncW`，
// `assert` 走 `_CrtSetReportMode`。只装一条的症状是修好一个弹窗、下一个又弹。
//
// 只在 MSVC + Debug 下有内容；其他工具链下这个 TU 是空的。

#if defined(_MSC_VER) && defined(_DEBUG)

#include <crtdbg.h>
#include <rtcapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace {

int __cdecl rtc_to_stderr(int err_type, const wchar_t* filename, int line,
                          const wchar_t* module_name, const wchar_t* format, ...) {
    (void)module_name;
    // **必须展开 `format`，那才是 MSVC 自己的那句话**（对话框里显示的就是它）。
    //
    // 第一版图省事，只打了 `_RTC_GetErrDesc(err_type)` 查出来的类型名，结果它
    // 把一次**栈损坏**报成了「Cast to smaller type causing loss of data」——
    // `err_type` 的编号与 `_RTC_ErrorNumber` 枚举并不是同一套下标。那句错误的
    // 描述把一次排查带偏了好几轮（去找根本不存在的窄化转换）。
    // **诊断工具给错方向比不给方向更坏**，所以现在两样都打：原始编号 +
    // MSVC 的原文。
    std::fprintf(stderr, "\n=== MSVC /RTC 运行时检查失败 ===\n");
    std::fprintf(stderr, "  编号: %d（不要拿它去查 _RTC_ErrorNumber，下标不同）\n",
                 err_type);
    std::fprintf(stderr, "  位置: %ls:%d\n",
                 filename != nullptr ? filename : L"(未知文件)", line);
    if (format != nullptr) {
        std::va_list args;
        va_start(args, format);
        std::fprintf(stderr, "  原文: ");
        // 无 `std::` 前缀：MSVC 不把宽字符那一族放进 `std`（加了会 C2039）。
        vfwprintf(stderr, format, args);
        va_end(args);
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "================================\n\n");
    std::fflush(stderr);
    // **不返回**：返回 0 的约定是「继续跑」，而栈已经坏了，继续只会在别处以
    // 更难解释的方式崩。直接退出，让 ctest 判失败。
    std::_Exit(3);
}

struct InstallDebugHandlers {
    InstallDebugHandlers() noexcept {
        _RTC_SetErrorFuncW(&rtc_to_stderr);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
};

// 全局对象，构造早于 `main`，因此早于任何测试用例。
const InstallDebugHandlers g_install_debug_handlers;

}  // namespace

#endif  // _MSC_VER && _DEBUG
