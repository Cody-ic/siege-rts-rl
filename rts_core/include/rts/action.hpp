// 战术层动作：攻方**每一个单位**每 4–8 tick 选一个。
//
// 规范来源是 `CLAUDE.md`「RL 设计决策（已定，勿擅自更改）」：
//
//   > **动作必须是离散的**：8 方向移动 + 停止、攻击最近/最弱敌人、攻击建筑/城墙。
//   > 不要改成连续坐标动作。**没有兵种专属动作**——所有单位共用同一套动作枚举，
//   > 差异体现在属性上（射程、破坏速率、能否对空），这样动作空间保持最小。
//
// 13 个，一个不多。**「驻守墙段槽位」不在这里**——它归 `rts/command.hpp`，
// 这样战术动作枚举一个字不动，「没有兵种专属动作」得以保住（登墙需要一个专属动作，
// 而攻方本来就没有登墙手段：花名册里不设云梯与攻城塔）。
//
// ## 谁消费它
//
// 只有**攻方**。守方的执行层是参数化脚本、直接读世界状态、不经网络
// （`守方AI与协同演化.md` 第 2 节，已并入 CLAUDE.md）。所以这个枚举不按侧参数化——
// 脚本不需要它，而给它一个 `Side` 参数会让人以为守方策略也吃这套动作。
//
// ## 一个必须先说清的陷阱：屏幕方位 vs 格坐标斜向
//
// 地图是等距投影的（`game/iso_projection.hpp`）：`x = (i−j)·tw/2`、`y = (i+j)·th/2`。
// 于是**格坐标的四条轴向，在屏幕上是四条斜线**，反之亦然：
//
// | 动作 | 格增量 (Δi, Δj) | 屏幕方向 | 在格坐标里是 |
// |---|---|---|---|
// | `MoveNE` `MoveSE` `MoveSW` `MoveNW` | 只动一个分量 | 斜 | **轴向** |
// | `MoveN` `MoveE` `MoveS` `MoveW` | 两个分量同时动 | 正 | **斜向** |
//
// 这一栏最右边才是「斜向能不能穿角」那条规则的适用对象。**按名字猜会恰好猜反**，
// 所以下面给了 `is_grid_diagonal()`，别自己判断 Δ。
//
// 命名取**屏幕方位**而不是格轴（不写成 `MoveIPlus` 之类），理由是它要与两处已有的
// 屏幕方位约定对齐：精灵朝向 `game::Facing{SE, SW, NE, NW}`，以及玩家在界面上
// 看到的方向。格增量由 `move_delta()` 给出，两者的一致性由测试拿真的
// `IsoProjection` 印证——不是靠这段注释。

#ifndef RTS_ACTION_HPP
#define RTS_ACTION_HPP

#include <cassert>
#include <cstdint>
#include <string_view>

#include "rts/types.hpp"

namespace rts {

// **`Stop` 必须是 0。** 两个理由，都不是风格：
//
//   * 零初始化的动作数组等于「全体停住」，那是唯一安全的默认值。
//     若 0 是某个移动方向，一处忘了填就变成全军朝那个方向走，而它看起来像个 bug
//     却不像个未初始化
//   * 动作掩码的实现里 0 通常是「兜底一定合法的那一个」。停住永远合法
//
// 8 个移动**必须连续**且顺序固定：`dir_index()` 与 `move_delta()` 都靠这条，
// 而「顺时针」这个性质让「反向 = (k+4) % 8」成立（有测试）。
enum class UnitAction : std::uint8_t {
    Stop = 0,

    // 屏幕方位，从正北起**顺时针**。格增量见 move_delta()。
    MoveN,        // (−1, −1)  格坐标斜向
    MoveNE,       // ( 0, −1)  格坐标轴向
    MoveE,        // (+1, −1)  格坐标斜向
    MoveSE,       // (+1,  0)  格坐标轴向
    MoveS,        // (+1, +1)  格坐标斜向
    MoveSW,       // ( 0, +1)  格坐标轴向
    MoveW,        // (−1, +1)  格坐标斜向
    MoveNW,       // (−1,  0)  格坐标轴向

    AtkNear,      // 攻击最近的敌方单位
    AtkWeak,      // 攻击最弱的敌方单位（血量绝对值最低者，不是比例）
    AtkBld,       // 攻击最近的建筑，不含墙与门；Phoenix 在射程内先选非木栅栏
    AtkWall,      // 攻击最近的墙段或城门
};

inline constexpr int kUnitActionCount = 13;
inline constexpr int kMoveDirCount = 8;

static_assert(static_cast<int>(UnitAction::AtkWall) == kUnitActionCount - 1);
static_assert(static_cast<int>(UnitAction::MoveN) == 1);
static_assert(static_cast<int>(UnitAction::MoveNW) == kMoveDirCount);

// `AtkBld` 与 `AtkWall` 分开、而不是合成一个「攻击最近的建筑」：
// 墙是攻方的**主要目标**（三阶段攻防的中段），而塔与矿场是**选择**
// （CLAUDE.md：拆采石场削上限、拆伐木场让防线修不动、拆金矿削兵力）。
// 合成一个的话，「攻其必救」这条核心战术就没有对应的动作可学——
// AI 只能打「最近的」，而最近的永远是墙。
//
// 反过来也不能按建筑类型各开一个动作：那就是兵种专属动作的另一种形态，
// 动作空间会随花名册膨胀。目标选择的**粒度**停在「墙 / 非墙」这一层。

// ——移动——

// 格增量。**表在这里，只有这一份。**
struct GridDelta {
    std::int8_t di = 0;
    std::int8_t dj = 0;

    friend constexpr bool operator==(GridDelta, GridDelta) noexcept = default;
};

constexpr bool is_move(UnitAction a) noexcept {
    const int v = static_cast<int>(a);
    return v >= static_cast<int>(UnitAction::MoveN) &&
           v <= static_cast<int>(UnitAction::MoveNW);
}

// 0..7，从正北起顺时针。非移动动作调用是编程错误。
constexpr int dir_index(UnitAction a) noexcept {
    assert(is_move(a));
    return static_cast<int>(a) - static_cast<int>(UnitAction::MoveN);
}

constexpr UnitAction move_of(int dir_index_0_to_7) noexcept {
    assert(dir_index_0_to_7 >= 0 && dir_index_0_to_7 < kMoveDirCount);
    return static_cast<UnitAction>(static_cast<int>(UnitAction::MoveN) +
                                   dir_index_0_to_7);
}

constexpr GridDelta move_delta(UnitAction a) noexcept {
    switch (a) {
        case UnitAction::MoveN:  return {-1, -1};
        case UnitAction::MoveNE: return { 0, -1};
        case UnitAction::MoveE:  return {+1, -1};
        case UnitAction::MoveSE: return {+1,  0};
        case UnitAction::MoveS:  return {+1, +1};
        case UnitAction::MoveSW: return { 0, +1};
        case UnitAction::MoveW:  return {-1, +1};
        case UnitAction::MoveNW: return {-1,  0};
        case UnitAction::Stop:
        case UnitAction::AtkNear:
        case UnitAction::AtkWeak:
        case UnitAction::AtkBld:
        case UnitAction::AtkWall:
            return {0, 0};
    }
    return {0, 0};
}

// **在格坐标里是斜向吗**——即两个分量同时改变。
// 屏幕上的正北/东/南/西才是这一类，见文件头那张表。
constexpr bool is_grid_diagonal(UnitAction a) noexcept {
    const GridDelta d = move_delta(a);
    return d.di != 0 && d.dj != 0;
}

// ——斜向穿角——
//
// **这条约定有下游消费者在等，所以先给一个值，而不是留空。**
//
// `tools/map_gen/` 的校验器（#31 / #33）已经被迫先取了最保守的一种，并写明
// 「接口定稿后回来对齐」。它现在有了一个可引用的名字，于是两侧不再各存一份假设。
//
// 取 `true`（两个正交邻格都可通行才允许斜向）的理由：
//
//   * 与校验器现取值一致，改动量为零
//   * `false` 会让单位从两段墙的对角缝里穿过去。墙是「高代价可通行」而非障碍
//     （CLAUDE.md 5.1），所以那不是「绕过障碍」而是**不拆墙就进城**，
//     三阶段攻防的中段直接消失
//
// **保守方向并不是永远安全的**，这一点校验器那边已经吃过：对「必须连通」型检查
// （走廊可达、无孤岛）保守 = 多报；对「必须存在一条通路」（初始城圈的缺口）
// 与「不得存在一条通路」（森林遮蔽通道）两类，极性是反的。所以这条要定死，
// 不能让两侧各取各的保守。
//
// **2026-08-30：从「暂取、仍需确认」改为已定**（`rts_core 接口契约.md` §4.2）。
// 原先挂在 ArLiangz 的确认上，而 1c 已由 @zhxxx233 接手推进（#57），
// 所以这条现在归接手人。**改动量不变**：要改仍是这一个常量 + 校验器对齐，
// 变的只是它有了默认值的主人，不再等一个不确定何时到来的确认。
inline constexpr bool kDiagonalNeedsBothOrthogonal = true;

// ——名字——
//
// ASCII 标识符，给报错、日志与 Python 侧用（动作空间要在 train/ 里可读地打印出来，
// 打整数等于让人自己数第几个）。**不是展示名**——这套动作不出现在玩家界面上。
constexpr std::string_view ident_of(UnitAction a) noexcept {
    switch (a) {
        case UnitAction::Stop:    return "Stop";
        case UnitAction::MoveN:   return "MoveN";
        case UnitAction::MoveNE:  return "MoveNE";
        case UnitAction::MoveE:   return "MoveE";
        case UnitAction::MoveSE:  return "MoveSE";
        case UnitAction::MoveS:   return "MoveS";
        case UnitAction::MoveSW:  return "MoveSW";
        case UnitAction::MoveW:   return "MoveW";
        case UnitAction::MoveNW:  return "MoveNW";
        case UnitAction::AtkNear: return "AtkNear";
        case UnitAction::AtkWeak: return "AtkWeak";
        case UnitAction::AtkBld:  return "AtkBld";
        case UnitAction::AtkWall: return "AtkWall";
    }
    return {};
}

constexpr UnitAction action_at(int i) noexcept { return static_cast<UnitAction>(i); }

// ——决策频率——
//
// CLAUDE.md：「每 4–8 tick 决策一次，不是每 tick」。区间的两端都写下来，
// 因为**具体取值仍待定**（它与单位速度耦合，而速度未标定），
// 但「不是每 tick」这条是硬的：每 tick 决策会让 credit assignment 链条长 4–8 倍。
//
// 放在这里而不是 train/ 侧：仿真要按这个周期打包观测、暂存动作，
// 两边各存一份的话，改了一边就静默错拍。
inline constexpr int kDecisionPeriodMin = 4;
inline constexpr int kDecisionPeriodMax = 8;

static_assert(kDecisionPeriodMin >= 2,
              "每 tick 决策会让 credit assignment 链条长一个数量级——CLAUDE.md 明令");
static_assert(kDecisionPeriodMin <= kDecisionPeriodMax);

}  // namespace rts

#endif  // RTS_ACTION_HPP
