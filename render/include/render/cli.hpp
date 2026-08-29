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
int cli_main(const std::vector<std::string>& args);

}  // namespace render

#endif  // RENDER_CLI_HPP
