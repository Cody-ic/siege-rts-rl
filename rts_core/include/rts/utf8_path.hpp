// UTF-8 字节串 → `std::filesystem::path`。**整个仓库的文件打开都要经过这里。**
//
// 存在的理由是一个已经让队友 4 条 ctest 变红的真实缺陷，而它的症状是
// **「打不开这个文件」而那个文件明明就在**——与 #39 里 `.gitattributes` 漏 `*.sh`
// 导致 `bad interpreter: /usr/bin/env` 是同一族的错：报错指向的东西是好的。
//
// ## 病因
//
// MSVC 的 `std::ifstream(const std::string&)` 与 `std::filesystem::path` 的**窄构造**
// 都把字节按**当前 ANSI 代码页**（简体中文机器上是 cp936）解释，而我们喂给它的是
// **UTF-8**——CMake 烤进 `GAME_TESTDATA_DIR` 的是 UTF-8，源码字面量在 `/utf-8` 下也是
// UTF-8。两者只在纯 ASCII 时恰好相同，于是「路径里有中文」就打不开。
//
// 而队友的仓库放在 `.../计算机程序设计大作业/`，所以这不是假想的场景。
//
// ## 实测（`/utf-8`、路径作为编译期宏，与 `tests/` 的形式一致）
//
// | 打开方式 | 纯 ASCII 路径 | 含中文路径 |
// |---|---|---|
// | `ifstream(std::string)` | 成功 | **失败** |
// | `ifstream(fs::path 窄构造)` | 成功 | **失败** |
// | `ifstream(fs::path 由 u8string 构造)` | 成功 | **成功** |
//
// ## 两条容易走错的岔路
//
// 1. **这不是 CMake 问题，路径怎么规范化都修不掉。** 曾被归因为「MSYS 虚拟路径」
//    （Git Bash 下 CMake 拿到 `/Users/...` 形式）。那**是另一个独立问题**、也确实存在，
//    但即使把路径规范成 `C:/...`，只要里面有中文，窄构造照样失败。
// 2. **`std::filesystem::u8path()` 在 C++20 里已弃用**，用 `path(std::u8string)` 构造。
//
// ## POSIX 侧
//
// `path` 的原生编码就是字节串，UTF-8 直通，所以下面这一份实现两个平台通用，
// 不需要 `#ifdef`。写成 header-only 是因为它只有一行、且 `rts_core` 的
// 头文件自足性守卫会自动把它纳入检查。

#ifndef RTS_UTF8_PATH_HPP
#define RTS_UTF8_PATH_HPP

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace rts {

// 把一串 UTF-8 字节变成 `std::filesystem::path`。
//
// **不要写 `std::ifstream in(some_std_string)`，也不要写 `fs::path(some_std_string)`**
// ——见文件头。所有打开文件的地方都应当是：
//
//     std::ifstream in(rts::path_from_utf8(p), std::ios::binary);
inline std::filesystem::path path_from_utf8(std::string_view utf8) {
#ifdef _WIN32
    // `path` 的原生编码是 UTF-16，所以必须**显式说明**输入是 UTF-8。
    //
    // reinterpret_cast 在这里是正确用法而不是绕过类型系统：`char8_t` 与 `char`
    // 的对象表示相同，而我们**确实知道**这些字节是 UTF-8——来源只有三处，
    // 全都是 UTF-8：CMake 写的宏、`/utf-8` 下的源码字面量、
    // 以及 `render/src/cli_entry.cpp` 从 `wmain` 转出来的命令行。
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
#else
    // POSIX：`path` 的原生编码就是字节串，UTF-8 直通，窄构造已经是对的。
    //
    // **刻意不在这边也走 u8string**：那个构造要 libstdc++ 的 `char8_t` 支持，
    // 而训练服务器的 GCC 版本至今没确认（`训练服务器环境.md` 第 5 节）。
    // 走窄构造就把这条依赖整个消掉了，而且这一侧本来也没有需要修的东西。
    return std::filesystem::path(std::string(utf8));
#endif
}

// `path` → UTF-8 字节串。报错信息里要回显路径时用。
//
// 直接 `p.string()` 在 MSVC 上会转成 ANSI 代码页，于是**报错信息里的中文变成乱码**
// ——而报错信息正是这类问题唯一的线索，糊掉它等于把这个 helper 的意义抵消掉一半。
inline std::string utf8_from_path(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::u8string u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
#else
    return p.string();   // POSIX：原生编码即字节串，见上
#endif
}

// 把整个文件读成字节。`*ok` 为 false 表示打不开或读不完。
//
// **为什么这个函数在 `rts_core` 的基础层，而不是各用各的。**
// 目前有三处要按字节读文件，而它们分属三个模块：
//
//   * `render/` 的精灵 PNG 与字体 TTF ——
//     **必须**自己读，因为 raylib 的文件读取全部走窄 `fopen`
//     （`utils.c` 的 `LoadFileData`，已核源码：零处 `_wfopen`），
//     所以 `LoadTexture` / `LoadFontEx` / `ExportImage` 在含中文的路径上全失败。
//     绕法是读字节 + 内存版 API（`LoadImageFromMemory` 等）
//   * `game/` 的地图 JSON
//   * **回放文件**（尚未实现，但 `CLAUDE.md` 把回放测试定为主要防 bug 手段，
//     它一定要读写文件）
//
// 三份拷贝里只要有一份忘了走 `path_from_utf8`，那一份就在中文路径下坏掉，
// 而症状是「打不开这个文件」——最不容易联想到编码的一种报错。
// 所以「怎么正确打开一个文件」只留一个答案。
inline std::vector<unsigned char> read_file_bytes(std::string_view utf8_path, bool* ok) {
    *ok = false;
    std::ifstream in(path_from_utf8(utf8_path), std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    if (size < 0) return {};
    in.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) return {};
    *ok = true;
    return bytes;
}

// 把字节写进文件。返回是否成功。
//
// 与上面同一个理由存在：`ExportImage`（截图模式）与将来的回放**写入**也走 raylib /
// 窄路径。raylib 侧的绕法是 `ExportImageToMemory` + 这个函数。
inline bool write_file_bytes(std::string_view utf8_path, const unsigned char* data,
                             std::size_t size) {
    std::ofstream out(path_from_utf8(utf8_path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (size > 0) {
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(out);
}

}  // namespace rts

#endif  // RTS_UTF8_PATH_HPP
