// 平台入口。**这个 TU 刻意不包含 `raylib.h`**——理由见 render/cli.hpp。
//
// 它只做一件事：**把命令行与控制台都拉到 UTF-8**，然后交给 `render::cli_main`。
// 于是程序内部只有一种编码，`rts::path_from_utf8()` 的前提总是成立，
// 而不是「取决于这条路径是从命令行来的还是从编译期宏来的」。

#include "render/cli.hpp"

#include <cstddef>
#include <string>
#include <vector>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

std::string utf8_from_wide(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};   // need 含结尾 NUL，所以 1 表示空串
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

}  // namespace

// 用 `wmain` 而不是 `GetCommandLineW` + `CommandLineToArgvW`：后者要链 shell32，
// 且会自己重新解析一遍命令行（引号规则与 CRT 略有差别）。`wmain` 由 CRT 给出，
// 分词与 `main` 完全一致，且只用到 kernel32 里的 `WideCharToMultiByte`。
int wmain(int argc, wchar_t** wargv) {
    // 控制台也拉到 UTF-8。**这不是锦上添花**：报错信息里写满了中文
    // （「打不开这个文件」「元数据里没有标识符」「要画一个没登记的字符」），
    // 而它们是这类问题唯一的线索。控制台停在 cp936 的话，那些信息在屏幕上是乱码
    // ——等于把线索也弄没了。
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) args.push_back(utf8_from_wide(wargv[i]));
    return render::cli_main(args);
}

#else

// POSIX：`argv` 的字节就是文件系统的字节，而那边一律 UTF-8，无需转换。
int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    return render::cli_main(args);
}

#endif
