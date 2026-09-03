#include "rts/cli_args.hpp"

#include <cstddef>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// `CommandLineToArgvW` 住在这里，`WIN32_LEAN_AND_MEAN` 会把它挡掉。
#include <shellapi.h>

// **用 pragma 而不是在 CMake 里给 rts_core 挂 shell32**：rts_core 是训练
// 服务器那一侧要编的库，不该为一个 Windows 专属的入口辅助多一条链接依赖；
// 而 pragma 只在 MSVC 上生效，正好与「Windows 一律 MSVC」这条既定约束重合
// （`CLAUDE.md` 开发环境约定）。Linux 走下面的 POSIX 分支，碰不到这里。
#ifdef _MSC_VER
#pragma comment(lib, "Shell32.lib")
#endif

namespace {

std::string utf8_from_wide(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') return {};
    const int need =
        WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};   // need 含结尾 NUL，1 表示空串
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

}   // namespace

namespace rts {

std::vector<std::string> utf8_args(int argc, char** argv) {
    int wargc = 0;
    // **不复用窄 argv**：ANSI 代码页表示不了的字符在它里面已经变成 `?` 了，
    // 那一步不可逆，所以只能回到宽命令行重取一遍（理由见头文件）。
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv == nullptr) {
        // 退回窄 argv：编码可能不对，但空手更糟（调用方会当成「没给参数」）。
        std::vector<std::string> fallback;
        fallback.reserve(static_cast<std::size_t>(argc < 0 ? 0 : argc));
        for (int i = 0; i < argc; ++i) fallback.emplace_back(argv[i]);
        return fallback;
    }
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(wargc < 0 ? 0 : wargc));
    for (int i = 0; i < wargc; ++i) out.push_back(utf8_from_wide(wargv[i]));
    LocalFree(wargv);
    return out;
}

}   // namespace rts

#else

namespace rts {

std::vector<std::string> utf8_args(int argc, char** argv) {
    // POSIX：`argv` 是字节串，约定 UTF-8，原样搬过来。
    std::vector<std::string> out;
    out.reserve(static_cast<std::size_t>(argc < 0 ? 0 : argc));
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

}   // namespace rts

#endif
