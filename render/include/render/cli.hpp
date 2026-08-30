// 前端的真正入口。
//
// **为什么它要有一个头文件，而不是直接写在 `main.cpp` 里。**
//
// Windows 上的 `argv` 是 ANSI 代码页而不是 UTF-8（实测：「中文路径测试」6 个汉字在
// `argv` 里只占 12 字节，每字 2 字节 = cp936；UTF-8 会是 18 字节）。所以必须在入口
// 用 `wmain` 拿真正的 UTF-16 再转 UTF-8，否则含中文的 `--map` / `--screenshot`
// 会让 `rts::path_from_utf8()` 抛「No mapping for the Unicode character exists」。
//
// 而 `wmain` 那段要 `<windows.h>`，**它和 `raylib.h` 撞名**：`Rectangle`（GDI 函数
// vs raylib 结构体）、`CloseWindow`、`ShowCursor`、`DrawText`。可以靠
// `NOGDI` / `NOUSER` 一个个压下去，但那是在维护一份「哪些宏必须先定义」的清单，
// 而清单会随两边的版本漂移。
//
// 所以改成**两个翻译单元**：`cli_entry.cpp` 只见 `<windows.h>`，
// `main.cpp` 只见 `raylib.h`，两者靠本头文件里这一个函数声明相接。
// 冲突因此不是被压制的，是不存在。

#ifndef RENDER_CLI_HPP
#define RENDER_CLI_HPP

#include <string>
#include <vector>

namespace render {

// **约定：`args` 里的每一条都是 UTF-8**，`args[0]` 是程序名。
// 建立这条约定的地方是 `cli_entry.cpp`，全程序只有那一处做编码转换。
//
// **`args[0]` 在 Windows 上是 exe 的绝对路径**（取自 `GetModuleFileNameW`，
// 不是 CRT 给的 `argv[0]`）。素材自动发现要拿它推「exe 在哪个目录」，而
// `argv[0]` 只保证是「调用时用的那个名字」——从 PATH 里调起来时它可能只是
// `rts_render`，那样发现就少了一个起点。
int cli_main(const std::vector<std::string>& args);

// 最近一次致命错误的完整文本（UTF-8），没有则为空串。
//
// **存在的理由是双击启动。** 命令行下这些文本打在 stderr 上就够了，而双击时
// 控制台是被隐藏的（见 `cli_entry.cpp`），于是「地图文件坏了」的表现会退化成
// 「双击之后什么都没发生」——最查不出原因的一种失败。所以那一侧要能把这段文本
// 弹成一个对话框，而它只能从这里取。
const std::string& last_fatal_message();

}  // namespace render

#endif  // RENDER_CLI_HPP
