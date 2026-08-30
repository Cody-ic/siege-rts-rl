// 数值表：外生数值进入仿真的**唯一容器**，以及它的指纹。
//
// ## 为什么它必须存在，而且排在机制之前
//
// 全仓库曾有七处注释把它称作「那份**还不存在的** JSON 数值表」，每一处都在解释
// 「所以这个数值由调用方给」。1c 一动手它就必须存在——射程、伤害、破坏速率、
// 造价全在里面。而它是**外生输入**：它变了，回放就该失效。
//
// 问题在于回放头现有的三个诊断字段**一个都抓不到它**（`rts_core 接口契约.md`
// §1.1.2）：`platform_fp` 管浮点、`file_hash` 管文件完整性、`hash_tag` 也不管——
// 状态布局没变、喂入清单没变，只是数值不同。于是「改了一个数字」的症状会是
// 「第 N tick 状态不一致」，看到那句话的人会去查一个不存在的确定性缺陷。
//
// 解法照抄 `map_content_hash`，**两个落点，各担一职**：
//
//   * 指纹进 `state_hash()` —— **早报**：数值表变了，第 0 tick 就分歧
//   * 指纹进回放头 —— **诊断**：把「第 0 tick 不一致」翻译成「是数值表变了」
//
// ## 指纹由 `World` 自己算，不由调用方给——与 `map_content_hash` 刻意不对称
//
// `map_content_hash` 必须由调用方给，因为 `rts_core` 不读地图文件、算不出来。
// 数值表不同：表本身就在 `WorldInit` 里、就在手上。让调用方另外给一个指纹
// 等于给同一件事**两个真相来源**，而两个真相来源迟早只更新一个
// （同 `side` 不存在单位数组里的那条理由，`rts/world.hpp`）。
//
// ## 字段是随机制展开的，本文件只放第一批
//
// 「写战斗时才知道要哪几个」（§1.1.2）——本批是承伤 / 目标选择 / 移动 / 视野
// 这四块要消费的。**表里存的是「基础值 + 缩放参数」，缩放规则不烤进形状**：
// `§1.4`（等级提升哪几个数值）还没定，血量与伤害的缩放系数各存一个，
// 定成「只涨一个」时改的是一个数字、不是这个结构。
//
// ## 这里一个具体数值都没有
//
// 默认值一律是「一眼就能看出不是标定过的」那种（1 / 0），同当年 `InitialHp`
// 的纪律。真值在 `game/data/stats_placeholder.json`（占位，待标定），
// 经 `game::StatsLoader` 进来——`rts_core` 不读文件，沿用 `MapData` 那条路。

#ifndef RTS_STATS_HPP
#define RTS_STATS_HPP

#include <array>
#include <cstdint>
#include <string_view>

#include "rts/roster.hpp"

namespace rts {

// **改了本文件里任何结构的形状（加减字段、改类型、改喂入顺序）就把这个串进一格。**
//
// 与 `kWorldHashTag` 同一条纪律、不同的管辖：那个管 `state_hash` 的喂入清单，
// 这个管「指纹是怎么从表算出来的」。形状变了而这个串没变，旧回放会报成
// 「数值表变了」——方向仍然是对的（重录），但成因说错了；进一格则两边都对。
// 已进格的历史：Stats/1 → Stats/2（机制第二批：造价 / 耗时 / 产出 / 维修，
// 三个结构各加字段、`GlobalStats` 扩五项）。
inline constexpr std::string_view kStatsShapeTag = "Stats/2";

// 每兵种一行。**结构性属性不在这里**（能否对空、能否破坏结构、三轴定位归
// `rts/unit_behavior.hpp` 与 `rts/roster.hpp`）；这里只有会随标定变的数。
struct UnitStats {
    std::int64_t max_hp = 1;          // 1 级满血
    std::int64_t damage = 0;          // 每次出手的基础伤害（1 级）
    float range = 0.0f;               // 射程（格）。近战也走它，给个小值
    float speed = 0.0f;               // 移动速度（格 / tick）
    float vision = 0.0f;              // 视野半径（格）
    std::int32_t windup_ticks = 0;    // 攻击前摇：出手承诺到伤害落地的间隔
    std::int32_t cooldown_ticks = 1;  // 两次出手承诺之间的间隔
    // 对建筑 / 城墙 / 障碍的伤害倍率（千分比，1000 = 1.0×）。`Ram` 的「压倒性
    // 拆除效率」由它承载。注意 `Phoenix` 对墙为 0 **不在这里**——那是结构约束，
    // 在 `UnitBehavior::can_break_structure()`；这里只放会随标定变的倍率。
    std::int32_t vs_structure_permille = 1000;
    // AOE 半径（格）。0 = 单体。落点在前摇开始那一刻锁定成坐标
    // （CLAUDE.md「结构破坏规则」那条实现要求），半径只是数值。
    float aoe_radius = 0.0f;
    // ——机制第二批：征兵——
    // 造价只有金币（三资源各对应一条决策轴：金币管**人力**，CLAUDE.md）。
    // 攻方单位这两项无意义（攻方无经济，编成走 `Composition` 预算），诚实地填 0。
    std::int64_t cost_gold = 0;       // 征兵造价（1 级；「越高越贵」的曲线待 §1.4）
    std::int32_t train_ticks = 0;     // 征兵耗时
};

// 每建筑一行。只有 `Tower` / `Flak` 有攻击数值，其余那几列为 0——
// 「`Watch` 零战力」「`Flak` 仅对空」是结构约束，不靠这里的 0 承载
// （它们在 `roster.hpp` / 战斗解算的结构判定里），这里的 0 只是诚实的默认。
struct BldStats {
    std::int64_t max_hp = 1;
    std::int64_t damage = 0;
    float range = 0.0f;
    float vision = 0.0f;
    std::int32_t windup_ticks = 0;
    std::int32_t cooldown_ticks = 1;
    // ——机制第二批：建造与产出——
    // 「所有永久建筑都同时消耗石材与木材，配比不同」（CLAUDE.md 建筑花名册）。
    // 金币不在建筑造价里——它管人力，那是第三条决策轴。
    std::int64_t cost_stone = 0;
    std::int64_t cost_wood = 0;
    std::int32_t build_ticks = 0;     // 施工总工时（工匠在场才推进，见 tick_economy）
    // 每个结算周期的产出数额。**种类不在这里**：采集建筑走 `resource_of()`（结构），
    // `Keep` 恒产金币（兵力地板，CLAUDE.md 单列一节的护栏）。其余建筑填 0。
    std::int64_t income_amount = 0;
};

// 每障碍一行。产出**种类**是结构（`harvest_of()`，`rts/roster.hpp`），
// 产出**数额**才在这里。
struct ObstacleStats {
    std::int64_t max_hp = 1;
    std::int64_t yield_amount = 0;
};

// 全局参数。等级缩放的两个系数**都在**（千分比 / 每级）：
// `§1.4` 定成「只涨一个」时把另一个归零即可，形状不动。
struct GlobalStats {
    std::int32_t hp_permille_per_level = 0;    // 等级每 +1，血量 +x‰（相对 1 级）
    std::int32_t dmg_permille_per_level = 0;   // 同上，伤害
    // ——机制第二批：经济节律与维修——
    // 默认值仍守「一眼看出没标定」的纪律，但两个被除数除外：0 会除零，
    // 取 1 是「最小的合法值」而不是「看起来合理的值」。
    std::int32_t income_period_ticks = 1;      // 产出结算周期（tick）
    float mason_work_radius = 0.0f;            // 工匠有效施工/维修半径（格）
    std::int64_t repair_hp_per_work_tick = 1;  // 维修每工时恢复的血量
    std::int64_t repair_wood_per_1000hp = 0;   // 维修花费：每 1000 缺口血量的木材
    std::int32_t cancel_refund_permille = 0;   // 撤销工地的退款比例（千分比）
};

// 四组分法来自 `rts_core 接口契约.md` §1.1.2 的三条形状决定。
struct StatsTable {
    std::array<UnitStats, kUnitTypeCount> unit{};
    std::array<BldStats, kBldTypeCount> bld{};
    std::array<ObstacleStats, kObstacleTypeCount> obstacle{};
    GlobalStats global{};

    const UnitStats& of(UnitType t) const noexcept {
        return unit[static_cast<std::size_t>(t)];
    }
    const BldStats& of(BldType t) const noexcept {
        return bld[static_cast<std::size_t>(t)];
    }
    const ObstacleStats& of(ObstacleType t) const noexcept {
        return obstacle[static_cast<std::size_t>(t)];
    }

    // 顺序敏感（FNV-1a），逐字段喂——**不要**改成对整个结构 `feed_pod`：
    // 混合宽度的结构有填充字节，`has_unique_object_representations` 会把它拦下，
    // 而绕开那条断言的所有已知方法都是错的（`rts/hash.hpp`）。
    std::uint64_t fingerprint() const noexcept;
};

}  // namespace rts

#endif  // RTS_STATS_HPP
