#include "game/display_names.hpp"

#include <cstdio>

namespace game {
namespace {

// 花名册里的中文展示名要与 CLAUDE.md 的对照表一致。这里只有地形与几个属性枚举
// ——单位与建筑的花名册还在 `rts_core` 那边未定稿（#28），等它定了再在这里加一批。

// 别在这些 switch 上加 `default:`。整个完备性保证就靠「没有 default」——
// 加了之后 MSVC 从 C4062 换成 C4061、GCC 的 -Wswitch 也不再报，
// 于是漏一个枚举值就变成静默返回一个空串。而空串在画面上是「什么都没有」，
// 是最难注意到的一种错。（C4061「有 default 但没列全」刻意没开，理由见
// cmake/CompilerWarnings.cmake——那种写法本身是正当的。）

// `describe_cell()` 用到的字面量，集中在这里。
//
// **集中的理由不是整洁，是它们同时要被 `all_display_strings()` 列出来。**
// 若在 describe_cell 里直接写字面量，那么加一句文案就得记得同时改两处，
// 而忘记的后果是字体少载几个码点、那句话渲成一串 `?`。
// 现在两边都引用同一组常量，能忘的只剩「加了常量忘了加进 kLiterals」——
// 而那条被测试钉住了（见头文件）。

// 项与项之间的分隔符。**是 ASCII 空格，不是全角空格**——实测全角空格 U+3000
// 在 simhei 下经 raylib 载入后宽度为 0，用它当分隔符各项会糊成一片
// （详见 render/src/text.cpp 顶部）。三个空格在中文之间够宽。
constexpr std::string_view kSep      = "   ";
constexpr std::string_view kCell     = "格";
constexpr std::string_view kOutside  = "光标不在地图内";
constexpr std::string_view kNoBuild  = "禁建";
constexpr std::string_view kKeep     = "领主堡垒";
constexpr std::string_view kHpFrac   = "残血";
constexpr std::string_view kSpawn    = "集结点";
constexpr std::string_view kCorridor = "走廊";

constexpr std::string_view kLiterals[] = {
    kSep, kCell, kOutside, kNoBuild, kKeep, kHpFrac, kSpawn, kCorridor,
};

}  // namespace

std::string_view display_name(Terrain t) noexcept {
    switch (t) {
        case Terrain::Plain:  return "平地";
        case Terrain::Rock:   return "岩壁";
        case Terrain::Forest: return "密林";
        case Terrain::Water:  return "水域";
        case Terrain::Bridge: return "桥";
    }
    return {};   // 不可达；仅为了让编译器闭嘴，不是兜底分支
}

std::string_view display_name(ResourceType t) noexcept {
    switch (t) {
        case ResourceType::Stone: return "石材";
        case ResourceType::Wood:  return "木材";
        case ResourceType::Gold:  return "金币";
    }
    return {};
}

std::string_view display_name(ResourceTier t) noexcept {
    // 「城内 / 城外」而不是 inner / outer 的直译：这两个词在玩家侧的含义是
    // 「保底收入」与「要派兵争夺」（CLAUDE.md「资源分布形态」），
    // 而「内层 / 外层」读不出这个差别。
    switch (t) {
        case ResourceTier::Inner: return "城内";
        case ResourceTier::Outer: return "城外";
    }
    return {};
}

std::string_view display_name(CorridorKind k) noexcept {
    switch (k) {
        case CorridorKind::Open:    return "开阔平原";
        case CorridorKind::Defile:  return "隘口";
        case CorridorKind::Forest:  return "林地";
        case CorridorKind::Economy: return "经济侧";
    }
    return {};
}

std::string_view display_name(WallKind k) noexcept {
    switch (k) {
        case WallKind::Wall: return "城墙";
        case WallKind::Gate: return "城门";
    }
    return {};
}

std::string describe_cell(const MapData& map, rts::GridPos p) {
    const int x = p.i;
    const int y = p.j;
    if (!map.in_bounds(x, y)) return std::string(kOutside);

    // 「格 (3, 2)」——坐标用 [x, y]，与地图文件的 `pos` 约定一致（6.2）。
    // 这里的 x/y 就是 GridPos 的 i/j，`MapData` 的访问器也按这个顺序收参数，
    // 所以整条链上没有一处需要交换，也就没有一处能悄悄写反。
    char head[48];
    std::snprintf(head, sizeof(head), "(%d, %d)", x, y);

    std::string out(kCell);
    out += " ";
    out += head;
    out += kSep;
    out += display_name(map.terrain_at(x, y));

    if (map.no_build_at(x, y)) {
        out += kSep;
        out += kNoBuild;
    }
    if (map.keep().i == p.i && map.keep().j == p.j) {
        out += kSep;
        out += kKeep;
    }
    if (const WallSegment* w = map.wall_at(x, y)) {
        out += kSep;
        out += display_name(w->kind);
        // 残血只在真的残破时显示。恒显示「残血 100%」是噪声，
        // 而 2.3 要求初始城圈**是**残破的，所以这一支会真的走到。
        if (w->hp_frac < 1.0f) {
            char hp[32];
            std::snprintf(hp, sizeof(hp), " %d%%",
                          static_cast<int>(w->hp_frac * 100.0f + 0.5f));
            out += " ";
            out += kHpFrac;
            out += hp;
        }
    }
    for (const ResourceNode& r : map.resources()) {
        if (r.pos.i == p.i && r.pos.j == p.j) {
            out += kSep;
            out += display_name(r.type);
            out += "(";
            out += display_name(r.tier);
            out += ")";
        }
    }
    for (const SpawnPoint& s : map.spawns()) {
        if (s.pos.i == p.i && s.pos.j == p.j) {
            char id[24];
            std::snprintf(id, sizeof(id), " %d", s.id);
            out += kSep;
            out += kSpawn;
            out += id;
            out += " ";
            out += display_name(s.corridor);
            out += kCorridor;
        }
    }
    return out;
}

const std::vector<std::string_view>& all_display_strings() {
    // 函数内静态：首次调用时构建一次。
    // **逐枚举按 count 遍历，不手抄名单**——理由见头文件。
    static const std::vector<std::string_view> kAll = [] {
        std::vector<std::string_view> v;
        for (std::string_view s : kLiterals) v.push_back(s);
        for (int i = 0; i < kTerrainCount; ++i) {
            v.push_back(display_name(static_cast<Terrain>(i)));
        }
        for (int i = 0; i < kResourceTypeCount; ++i) {
            v.push_back(display_name(static_cast<ResourceType>(i)));
        }
        for (int i = 0; i < kResourceTierCount; ++i) {
            v.push_back(display_name(static_cast<ResourceTier>(i)));
        }
        for (int i = 0; i < kCorridorKindCount; ++i) {
            v.push_back(display_name(static_cast<CorridorKind>(i)));
        }
        for (int i = 0; i < kWallKindCount; ++i) {
            v.push_back(display_name(static_cast<WallKind>(i)));
        }
        return v;
    }();
    return kAll;
}

}  // namespace game
