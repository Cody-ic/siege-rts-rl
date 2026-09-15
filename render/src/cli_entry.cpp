// 平台入口。**这个 TU 刻意不包含 `raylib.h`**——理由见 render/cli.hpp。
//
// 它做三件事，都只在这一处做：
//
//   1. **把命令行与控制台都拉到 UTF-8**，然后交给 `render::cli_main`。于是程序
//      内部只有一种编码，`rts::path_from_utf8()` 的前提总是成立，而不是
//      「取决于这条路径是从命令行来的还是从编译期宏来的」。
//   2. **`args[0]` 换成 exe 的绝对路径**（`GetModuleFileNameW`）。素材自动发现
//      要拿它推「exe 在哪个目录」，而 CRT 给的 `argv[0]` 只是「调用时用的那个
//      名字」——从 PATH 调起来时它可能只有 `rts_render` 五个字，那样发现就少了
//      一个起点。
//   3. **双击启动时把控制台藏掉、并把致命错误弹成对话框**，见下面那两段。
//
// 这三件事都是**平台的**而不是程序逻辑的，所以它们全部收在这个 TU 里，
// `main.cpp` 一行 `#ifdef _WIN32` 都没有。

#include "render/cli.hpp"

#include <cstddef>
#include <string>
#include <vector>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

namespace {

std::string utf8_from_wide(const wchar_t* w) {
    if (w == nullptr || *w == L'\0') return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};   // need 含结尾 NUL，所以 1 表示空串
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring wide_from_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                         static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(),
                        need);
    return out;
}

// exe 的绝对路径。取不到时返回空串（调用方退回 CRT 的 `argv[0]`）。
std::string exe_path() {
    std::wstring buf(512, L'\0');
    for (int round = 0; round < 4; ++round) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                           static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        // 缓冲区不够时 `GetModuleFileNameW` 返回缓冲区大小本身（并截断），
        // 而**不是**所需长度——所以只能靠「返回值等于容量」判断，再翻倍重来。
        if (n < buf.size()) return utf8_from_wide(buf.c_str());
        buf.assign(buf.size() * 2, L'\0');
    }
    return {};
}

// 这个进程是不是「自己独占一个控制台」——也就是**双击启动**。
//
// 判据是控制台上挂着的进程数：从 cmd / Git Bash / ctest 里调起来时，父进程也挂在
// 同一个控制台上（≥ 2）；Explorer 双击时系统给我们新开一个控制台，只有我们自己。
//
// 为什么要判：这个程序是 CONSOLE 子系统的（`wmain`，且 `--verify-assets` 之类
// 要往 stdout 打东西），所以双击时会先弹一个黑框再开游戏窗口，很难看。
// 而**不能改成 WINDOWS 子系统**——那样命令行下的所有输出（包括报错）都没了，
// 几条 ctest 也跟着瞎。所以只在双击这一种情形下把黑框藏起来。
bool owns_console() {
    DWORD pids[4] = {};
    const DWORD n = GetConsoleProcessList(pids, 4);
    return n == 1;
}

}  // namespace

std::string render::pick_attacker_policy() {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"RL ONNX model\0*.onnx\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrTitle = L"选择攻方 RL 模型";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? utf8_from_wide(path) : std::string{};
}

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
    // `args[0]` 换成 exe 的绝对路径（见文件头第 2 条）。取不到就留着 CRT 给的。
    if (!args.empty()) {
        const std::string exe = exe_path();
        if (!exe.empty()) args[0] = exe;
    }

    // **双击启动**：把控制台黑框藏掉。判据见 `owns_console()`。
    //
    // 只在「没有任何参数」时藏：带参数一定是从命令行来的（Explorer 传不了参数），
    // 而那时输出是有人看的。两个条件都要——`owns_console()` 单独成立的情形还有
    // 「从快捷方式带参数启动」。
    const bool double_clicked = argc <= 1 && owns_console();
    if (double_clicked) {
        HWND console = GetConsoleWindow();
        if (console != nullptr) ShowWindow(console, SW_HIDE);
    }

    const int rc = render::cli_main(args);

    // 藏了控制台就等于**把 stderr 藏了**，于是「地图文件坏了」的表现会退化成
    // 「双击之后什么都没发生」。所以这条路径上的失败要弹一个对话框。
    //
    // 只在双击时弹：命令行下再弹一个要点确定的框，反而是打断。
    if (double_clicked && rc != 0) {
        const std::string& msg = render::last_fatal_message();
        const std::wstring text = wide_from_utf8(
            msg.empty() ? std::string("启动失败（退出码 ") + std::to_string(rc) + "）"
                        : msg);
        MessageBoxW(nullptr, text.c_str(), L"siege-rts-rl", MB_OK | MB_ICONERROR);
    }
    return rc;
}

#else

std::string render::pick_attacker_policy() { return {}; }

// POSIX：`argv` 的字节就是文件系统的字节，而那边一律 UTF-8，无需转换。
int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    return render::cli_main(args);
}

#endif
