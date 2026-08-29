// **全仓库唯一的 C++ 地图入口。**
//
// 为什么要收在一处，理由与 Python 侧 `tools/map_gen/mapfile.py` 完全相同（#31）：
//
// > 6.3 的 `content_hash` 必须在生成端与校验端算得一模一样，而两处各写一遍正是
// > 这类需求最典型的失败方式 —— 它失败时的症状（回放静默偏离、跨平台误判为两张图）
// > 恰恰是最难查的那种。
//
// C++ 侧还多一层：它**将来必须与 Python 侧逐字节一致**。见下面 verify_content_hash
// 处的说明——本版刻意不做校验，但那笔账要还。

#ifndef GAME_MAP_LOADER_HPP
#define GAME_MAP_LOADER_HPP

#include <stdexcept>
#include <string>
#include <string_view>

#include "game/map_data.hpp"

namespace game {

// 地图文件在**格式**层面就不合法（字段缺失、行长对不上、坐标越界……）。
//
// 与 `地图与场景设计.md` 第 8 节那 15 条语义校验是两回事：那 15 条查「这张图设计得
// 对不对」，本异常查「这份文件读不读得成一张图」。前者失败说明地图要改，
// 后者失败说明文件坏了。分类同 Python 侧的 `MapFormatError`。
class MapFormatError : public std::runtime_error {
public:
    explicit MapFormatError(const std::string& what) : std::runtime_error(what) {}
};

class MapLoader {
public:
    // 从文件读。路径不存在、读不动、或格式不合法都抛 MapFormatError。
    static MapData from_file(const std::string& path);

    // 从内存里的 JSON 文本读。
    //
    // 存在的理由不是「方便」，是**让测试走与生产完全相同的那条路径**：
    // 单元测试不必为每个用例准备一个夹具文件，于是也就不会有人为了省事
    // 绕开 MapLoader 直接手搭一个 MapData（那会让「存在即合法」这条失效）。
    static MapData from_string(std::string_view json_text,
                               const std::string& origin = "<内存>");

    // ── `content_hash` 本版刻意不校验，这是有意的推迟而不是遗漏 ──────────────
    //
    //   * 渲染不需要它。6.3 要求的是**回放文件头**记录 `map_id` 与 `content_hash`，
    //     那是 rts_core / 回放那一侧的事。
    //   * 一旦校验，就必须证明 C++ 的规范序列化与 `mapfile.py` 的 `_CANONICAL_KWARGS`
    //     （sort_keys / ensure_ascii=False / indent=2 / separators=(",", ": ")）
    //     **逐字节相同**，否则同一张图两边算出两个哈希——而那正是本文件开头
    //     要防的那种失败。这需要一条跨语言的比对测试，不是顺手能做对的。
    //
    // 实现它的人：判据在 `地图与场景设计.md` 6.3，参照实现在 `mapfile.py` 的
    // `canonical_dumps` / `compute_content_hash`，而**验收标准是「同一张图 C++ 与
    // Python 算出同一个字符串」**，不是「C++ 自己前后一致」。
};

}  // namespace game

#endif  // GAME_MAP_LOADER_HPP
