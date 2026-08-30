// **全仓库唯一的 C++ 数值表入口。** JSON → `rts::StatsTable`。
//
// 为什么在 `game/` 而不是 `rts_core/`：`rts_core` 不读文件（`MapData` 那条既有
// 纪律，`rts/replay.hpp` 的 `replay_verify` 注释里写着），JSON 依赖到本模块为止。
// `rts_core` 只看见 `StatsTable`，两套工具链要各自编译的代码面因此不含 JSON 库。
//
// ## 表是**占位值**，这一点由文件自己声明
//
// `game/data/stats_placeholder.json` 是当前唯一的一份表，文件名与内嵌的 `_note`
// 都写明「占位、待标定」（CLAUDE.md「关于数值」：数值以占位常量放在 JSON 数据
// 文件中，调整不触碰代码——本文件就是那句话的「不触碰代码」那一半）。
//
// ## 严格校验，宁可载入失败
//
// 缺一个兵种、多一个未知字段、值越出下界，一律抛 `StatsFormatError`。
// 理由与 `MapLoader` 相同（存在即合法），外加一条数值表特有的：**漏一个兵种时
// 若给默认值（伤害 0、血量 1），那个兵种会「能跑但打不动」，症状离病因极远**
// ——看起来像战斗解算有 bug，实际是 JSON 里少了一行。
//
// 键以 `_` 开头的字段一律忽略（JSON 没有注释，`_note` 一族充当注释）。

#ifndef GAME_STATS_LOADER_HPP
#define GAME_STATS_LOADER_HPP

#include <stdexcept>
#include <string>
#include <string_view>

#include "rts/stats.hpp"

namespace game {

// 数值表文件在格式层面不合法（字段缺失、类型不对、值越出下界、有认不出的键）。
class StatsFormatError : public std::runtime_error {
public:
    explicit StatsFormatError(const std::string& what) : std::runtime_error(what) {}
};

class StatsLoader {
public:
    // 从文件读。路径不存在、读不动、或格式不合法都抛 StatsFormatError。
    // 路径按 UTF-8 解释（经 `rts/utf8_path.hpp`，同 `MapLoader`）。
    static rts::StatsTable from_file(const std::string& path);

    // 从内存里的 JSON 文本读。存在的理由同 `MapLoader::from_string`：
    // 让测试走与生产完全相同的那条路径。
    static rts::StatsTable from_string(std::string_view json_text,
                                       const std::string& origin = "<内存>");
};

}  // namespace game

#endif  // GAME_STATS_LOADER_HPP
