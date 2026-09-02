#include "game/display_names.hpp"

#include <cstdio>

namespace game {
namespace {

// 花名册里的中文展示名要与 CLAUDE.md 的三张对照表**逐字**一致。
// 「逐字」不是洁癖：那三张表是四人对齐的依据，而玩家读到的就是这里的串，
// 两边不一致时答辩材料里的名字与程序里的名字会对不上。

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

// 2026-08-31：`kCorridor`（「走廊」）已随 CorridorKind 一起删除。
constexpr std::string_view kLiterals[] = {
    kSep, kCell, kOutside, kNoBuild, kKeep, kHpFrac, kSpawn,
};

}  // namespace

std::string_view display_name(Compass c) noexcept {
    switch (c) {
        case Compass::N:  return "北";
        case Compass::NE: return "东北";
        case Compass::E:  return "东";
        case Compass::SE: return "东南";
        case Compass::S:  return "南";
        case Compass::SW: return "西南";
        case Compass::W:  return "西";
        case Compass::NW: return "西北";
    }
    return {};
}

Compass compass_of(rts::GridPos from, rts::GridPos to) noexcept {
    const int dx = to.i - from.i;   // +x = 东
    const int dy = to.j - from.j;   // +y = 南（`gj` 增大是屏幕下方）
    const int ax = dx < 0 ? -dx : dx;
    const int ay = dy < 0 ? -dy : dy;
    // 斜向只在两轴分量接近时才取：一个分量不到另一个的一半就当正方向，
    // 否则「几乎正北」也会被报成「东北」，方向提示就不好用了。
    const bool diag_x = ax * 2 > ay;
    const bool diag_y = ay * 2 > ax;
    if (diag_x && diag_y) {
        if (dy < 0) return dx > 0 ? Compass::NE : Compass::NW;
        return dx > 0 ? Compass::SE : Compass::SW;
    }
    if (ax > ay) return dx > 0 ? Compass::E : Compass::W;
    return dy > 0 ? Compass::S : Compass::N;
}

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

// 2026-08-31：`display_name(CorridorKind)` 已随「走廊」概念一起删除。

std::string_view display_name(WallKind k) noexcept {
    switch (k) {
        case WallKind::Wall: return "城墙";
        case WallKind::Gate: return "城门";
    }
    return {};
}

std::string_view display_name(rts::UnitType t) noexcept {
    switch (t) {
        case rts::UnitType::Archer:  return "戍卫弓手";
        case rts::UnitType::Spear:   return "铁壁枪卫";
        case rts::UnitType::Ranger:  return "逐风猎骑";
        case rts::UnitType::Scout:   return "游猎斥候";
        case rts::UnitType::Mason:   return "工匠";
        case rts::UnitType::Ghoul:   return "亡灵步兵";
        case rts::UnitType::Shade:   return "亡灵法师";
        case rts::UnitType::Knight:  return "鬼域骑士团";
        case rts::UnitType::Phoenix: return "不死鸟";
        case rts::UnitType::Wraith:  return "幽影窥使";
        case rts::UnitType::Ram:     return "攻城锤";
    }
    return {};
}

std::string_view display_name(rts::BldType t) noexcept {
    // 器物用描述名、阵营单位用风味名（CLAUDE.md 命名纪律）：所以建筑与器械这一栏
    // 全是「一看就懂用途」的名字，而上面那栏活的单位才承载身份。
    switch (t) {
        case rts::BldType::Keep:    return "领主堡垒";
        case rts::BldType::Wall:    return "城墙";
        case rts::BldType::Gate:    return "城门";
        case rts::BldType::Tower:   return "镇野箭楼";
        case rts::BldType::Flak:    return "蔽空弩楼";
        case rts::BldType::Watch:   return "瞭望塔";
        case rts::BldType::Barrack: return "兵营";
        case rts::BldType::Fence:   return "木栅";
        case rts::BldType::Quarry:  return "采石场";
        case rts::BldType::Lumber:  return "伐木场";
        case rts::BldType::Mine:    return "金矿场";
    }
    return {};
}

std::string_view display_name(rts::ObstacleType t) noexcept {
    switch (t) {
        case rts::ObstacleType::Stump:   return "树桩";
        case rts::ObstacleType::Sapling: return "幼树";
        case rts::ObstacleType::Rubble:  return "碎石";
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
        for (int i = 0; i < kWallKindCount; ++i) {
            v.push_back(display_name(static_cast<WallKind>(i)));
        }
        // 方位名（免费方向提示用）。同样是机械遍历——手抄一份就又开了一个
        // 「加了字忘了登记 ⇒ 画的时候抛」的入口。
        for (int i = 0; i < kCompassCount; ++i) {
            v.push_back(display_name(static_cast<Compass>(i)));
        }
        // 花名册三张表。**加进来的直接后果是字体要多载约 40 个码点**，
        // 而那正是它必须在这里出现的理由：PR D 一画实体就要显示单位名，
        // 而没登记的汉字会渲成图集里的第一个字形——「镇野箭楼」变「平平平平」，
        // 四个看着完全合理的汉字。等到那时才发现，就得反过来查为什么。
        for (int i = 0; i < rts::kUnitTypeCount; ++i) {
            v.push_back(display_name(rts::unit_at(i)));
        }
        for (int i = 0; i < rts::kBldTypeCount; ++i) {
            v.push_back(display_name(rts::bld_at(i)));
        }
        for (int i = 0; i < rts::kObstacleTypeCount; ++i) {
            v.push_back(display_name(rts::obstacle_at(i)));
        }
        return v;
    }();
    return kAll;
}

}  // namespace game
