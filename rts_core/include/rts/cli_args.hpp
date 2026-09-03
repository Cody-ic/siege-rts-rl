#pragma once

// 把平台给的命令行转成 UTF-8。**每个吃命令行路径的 exe 都必须先过这一道。**
//
// ## 为什么需要它（2026-09-03，一次真实崩溃）
//
// `rts/utf8_path.hpp` 的前提是「程序内部只有 UTF-8 一种编码」，而它自己的注释
// 列出了 UTF-8 仅有的三个来源：CMake 写的宏、`/utf-8` 下的源码字面量、以及
// **从宽命令行转出来的 argv**。第三条是有前提的——Windows 的窄
// `main(int, char**)` 给的是**当前 ANSI 代码页**（本项目的机器上是 GBK），
// 不是 UTF-8。
//
// 把 GBK 字节直接喂给 `path_from_utf8()` 会构造一个内容非法的 `u8string`，
// 而 MSVC 上的症状**不是「文件打不开」，是进程直接 `__fastfail`
// （退出码 0xC0000409 = 3221226505）**——最不容易联想到编码的一种失败。
//
// 这不是假设：`tools/calibration_runner` 用裸 `main` 收 argv，于是仓库路径
// 含中文时（本仓库正是）它一跑就崩，`calibration_runner_smoke` 那条 ctest
// 因此对**所有把仓库放在非 ASCII 路径下的人**恒红，而在 ASCII 路径下恒绿。
// 实测判据：同一个 exe、同一批参数，`--maps /tmp/asciipool` 正常退出，
// `--maps /tmp/中文池` 退 3221226505。
//
// ## 为什么放在 rts_core 而不是各 exe 各写一份
//
// `utf8_path.hpp` 末尾那段写着「三份拷贝里只要有一份忘了走 `path_from_utf8`，
// 那一份就在中文路径下坏掉……所以『怎么正确打开一个文件』只留一个答案」。
// 「怎么正确拿到命令行」是同一件事的上游，同样只留一个答案。
//
// **`render/src/cli_entry.cpp` 目前另有一份等价实现**，它还顺带做了两件
// 平台活（双击时藏控制台、致命错误弹对话框），所以没有一并改过来；
// 那一份与本函数的重复面仅限 `CommandLineToArgvW` + `WideCharToMultiByte`
// 这二十行，改动它属于 `render/` 的活，记在这里免得下次有人以为漏了。

#include <string>
#include <vector>

namespace rts {

// 返回 UTF-8 的命令行参数（含 `argv[0]`）。
//
// **Windows**：忽略传进来的窄 `argv`，改从 `GetCommandLineW()` 取宽命令行再转
// UTF-8——不是「把 GBK 转成 UTF-8」，而是**根本不经过窄那一层**，因为 ANSI 代码页
// 表示不了的字符在窄 argv 里已经被替换成 `?` 了，那一步是不可逆的。
// **POSIX**：`argv` 本来就是字节串、约定 UTF-8，原样搬过来。
//
// 取不到宽命令行时（理论上不会）退回窄 `argv`，宁可编码不对也不要空手。
std::vector<std::string> utf8_args(int argc, char** argv);

}   // namespace rts
