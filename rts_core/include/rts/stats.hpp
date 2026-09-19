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
// 三个结构各加字段、`GlobalStats` 扩五项）；Stats/2 → Stats/3（机制第三批：
// 驻守与高度优势，`GlobalStats` 扩四项）；Stats/3 → Stats/4（机制第四批：
// 冲锋与齐射，`BldStats` 加 AOE 半径、`GlobalStats` 扩三项）；
// Stats/4 → Stats/5（机制第五批：在途弹丸，`UnitStats` 与 `BldStats` 各加
// 弹丸速度）；Stats/5 → Stats/6（AOE 溅射折扣：`UnitStats` 加
// `splash_dmg_permille`，主目标满伤、圈内其余单位打折——见该字段注释）；
// Stats/6 → Stats/7（建筑等级上限：`BldStats` 加三个升级字段，`GlobalStats`
// 加 `building_level_cap_divisor`）；Stats/7 → Stats/8（**§1.4 落地：等级缩放
// 从线性改成各开一份平方根**，见 `rts/combat_math.hpp` 的 `level_permille`）；
// Stats/8 → Stats/9（兵种等级上限：`GlobalStats` 加 `train_ticks_permille_per_level`
// 与 `unit_upgrade_radius`）；Stats/9 → Stats/10（编队系统移除：就地升级机制
// 删除，`unit_upgrade_radius` 失去唯一消费者，删字段）；Stats/10 → Stats/11（拆除
// 成品建筑，`cancel_refund_permille` 改为 `demolish_refund_permille`）；
// Stats/11 → Stats/12（**建筑升级定价从「每级一个常数」改成「累计 ∝ √B(L)」**，
// 非 `Keep` 十座；见 `BldStats::upgrade_cost_stone` 与
// `World::bld_upgrade_cost_stone()`）。
//
// **「形状没变而必须进格」现在有两格了（Stats/7 → 8、Stats/11 → 12），
// 两次都只改语义。** 下面那段原写「本文件唯一一次」，Stats/12 让它过期——
// 而这正好说明它不是特例、是会复发的一类，所以那段理由更该留着。那次
// 改的是 `level_permille` 怎么用这两个系数（语义），字段一个没加减。若不进格，
// `fingerprint()` 算出来一模一样，于是旧回放**不报 `StatsMismatch` 而静默算出
// 不同的结果**——那是本仓库通篇最防的一类失效（「布局改了」与「跑歪了」不可
// 区分）。**下一个只改语义不改字段的人照此办理。**
// Stats/13: non-Keep upgrades have a rising materials floor (40% + 10% per prior level).
// Stats/14: fortifications have independent, staged durability and capped upgrade prices.
inline constexpr std::string_view kStatsShapeTag = "Stats/14";

constexpr bool is_fortification(BldType type) noexcept {
    return type == BldType::Wall || type == BldType::Gate || type == BldType::Fence;
}

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
    // ——机制第五批：在途弹丸——
    // 弹丸飞行速度（格 / tick）。**<= 0 = 瞬时命中**——「单体伤害在落地帧瞬时
    // 结算」那条书面近似（契约 §1.1.1）没有被删，而是降级成 0 速度的退化形态，
    // 于是没配这个数的表行为一字不变（同 windup_ticks == 0 当场落地的先例）。
    // **谁放弹丸是结构**（`UnitBehavior::launches_projectile()`：Ranged × 非空中），
    // 给近战兵种配了速度也不放——这里只是弹丸真放出来之后飞多快。
    float proj_speed = 0.0f;
    // ——机制第二批：征兵——
    // 造价只有金币（三资源各对应一条决策轴：金币管**人力**，CLAUDE.md）。
    // 攻方单位这两项无意义（攻方无经济，编成走 `Composition` 预算），诚实地填 0。
    std::int64_t cost_gold = 0;       // 征兵造价（1 级；「越高越贵」的曲线待 §1.4）
    std::int32_t train_ticks = 0;     // 征兵耗时
    // ——AOE 溅射折扣——
    // 圈内**主目标**（承诺时锁定的那个）恒吃满伤害；圈内其余单位（不分敌我）
    // 按这个千分比打折，1000 = 与主目标同倍率。**默认 1000 是诚实默认，不是
    // 已标定值**——多数兵种 `aoe_radius == 0`，这个字段本就用不上；只有真的
    // 开了 AOE 的兵种（目前只有 `Ram`）才该把它调低于 1000。
    // **只管单位，不管建筑/障碍**——CLAUDE.md「结构破坏规则」原文对静止的
    // 建筑要求「全额命中且可波及相邻墙段」，建筑侧的溅射不打折是设计而非
    // 漏做；这里加字段前先确认过那句话没被这次改动波及。
    // `StatsLoader` 拦一条不变量：`aoe_radius > 0` 时这个数必须 < 1000
    // （不能等于 1000——"主目标和溅射伤害一样"正是这个字段要修的问题）。
    std::int32_t splash_dmg_permille = 1000;
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
    // ——机制第四批：齐射——
    // AOE 半径（格），0 = 单体。`Tower` 的「齐射覆盖（克制步兵一拥而上啃墙）」
    // 由它承载；落点在前摇开始锁定（与 `Ram` 同一条承诺规则）。
    // **对空建筑（`Flak`）结构上忽略它**——「AA 只做单体狙击型」是结构不是数值，
    // 表里配了也不齐射（同「表不能把瞭望塔配成印钞机」的先例，src/mechanics.cpp）。
    float aoe_radius = 0.0f;
    // ——机制第五批：在途弹丸——
    // 同 `UnitStats::proj_speed`（<= 0 = 瞬时命中）。建筑不会近战，
    // 开火即弹丸——`Tower` 的齐射箭雨与 `Flak` 的狙击弩矢都真的在飞。
    float proj_speed = 0.0f;
    // ——建筑等级上限（守方升级轴的第一个输出）——
    //
    // Wall/Gate/Fence use fortification_upgrade_cost() (staged, then capped).
    // The historical pricing explanation below applies to the other buildings.
    // 升级的**石/木定价参数**，以及每一级的工时（`upgrade_ticks <= 0` 当场
    // 完工，同 `build_ticks` 的先例）。等级本身存在 `World::b_level_`（不在
    // 这里，那是会变的状态，不是标定值）；上限由 `World::building_level_cap()`
    // 从 `Keep` 的等级推导，`Keep` 自己不受这个上限约束。
    //
    // ⚠️ **这两个数不是「一级的价钱」，除了 `Keep`。**（Stats/11 → Stats/12
    // 改的就是这件事，字段一个没动。）唯一的计算处是
    // `World::bld_upgrade_cost_stone()` / `..._wood()`：
    //
    //   * 非 `Keep` 十座——升级买的是血量与伤害，两者都 `∝ √B(L)`，
    //     所以这两个数是**累计曲线的标度**：
    //     `累计(L) = cost + up × (√B(L) − 1)`，一步的价钱是相邻两级之差。
    //     `up == cost` 时每石买到的火力与等级无关（正式表就取这个值），
    //     偏离它是留给标定的旋钮。**照旧当单价用会让累计造价线性、
    //     而战力开方 ⇒ 最优档恒为 1 级，这个输出整个是装饰品。**
    //   * `Keep`——升级买的是三个**线性**上限（人口 `8+2K`、建筑等级
    //     `ceil(K/2)`、兵种等级 `K`），线性输出配线性定价本来就同阶，
    //     所以它这两个数仍是**每一级的价钱**，一字未改。
    //
    // `upgrade_ticks` **刻意仍是每级一个常数**、不随定价一起开方：本仓库
    // 对「不希望被无脑刷」的东西偏好用时间与暴露而不是价格
    // （CLAUDE.md「波次进行中不禁止任何资源的使用」），而工时正是那个限制器
    // ——高等级的一步很便宜，但仍要占一名工匠整整一段工期，且那段时间里
    // 这座建筑修不了（升级与施工/维修互斥）。同 `train_ticks_permille_per_level`
    // 「训练耗时不参与 p−q=0 那组不变量」那条先例。
    std::int64_t upgrade_cost_stone = 0;
    std::int64_t upgrade_cost_wood = 0;
    std::int32_t upgrade_ticks = 0;
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
    // 等级缩放系数。**语义是「开方前」的线性系数**（`√(1 + k(L−1))`，见
    // `combat_math.hpp` 的 `level_permille`），不是「每级 +x‰」。
    //
    // These remain equal for combat units and ordinary buildings. Fortifications
    // use the separate linear/restarted-square-root HP parameters below.
    std::int32_t hp_permille_per_level = 0;
    std::int32_t dmg_permille_per_level = 0;
    // ——机制第二批：经济节律与维修——
    // 默认值仍守「一眼看出没标定」的纪律，但两个被除数除外：0 会除零，
    // 取 1 是「最小的合法值」而不是「看起来合理的值」。
    std::int32_t income_period_ticks = 1;      // 产出结算周期（tick）
    float mason_work_radius = 0.0f;            // 工匠有效施工/维修半径（格）
    std::int64_t repair_hp_per_work_tick = 1;  // 维修每工时恢复的血量
    std::int64_t repair_wood_per_1000hp = 0;   // 维修花费：每 1000 缺口血量的木材
    std::int32_t demolish_refund_permille = 0; // 拆除完工建筑的基础造价退款比例（千分比）
    // ——机制第三批：驻守与高度优势——
    // 三个 `high_ground_*` 是一组（前缀承载「墙血 >= 一半才生效」这一共同前提，
    // 见 `World::on_high_wall`）。哪些效果**存在**是结构（CLAUDE.md 抄的
    // Stronghold 三条），这里只有幅度。倍率默认 1000 = 恒等——它不守「默认 1/0」
    // 的字面，但守它的实质：恒等一眼就能看出没标定，而 0 会把伤害路径整个掐断，
    // 那是把「没标定」写成了一条结构禁令（同 income_period 取 1 的理由）。
    std::int32_t garrison_mount_ticks = 0;         // 上墙延迟（原地登上那个「短暂」）
    std::int32_t high_ground_miss_permille = 0;    // 低处打墙上单位：整发落空的概率
    std::int32_t high_ground_dmg_permille = 1000;  // 低处打墙上单位：命中后的伤害倍率
    float high_ground_range_bonus = 0.0f;          // 远程驻守的射程加成（格，加法）
    // ——机制第四批：冲锋——
    // 「冲锋伤害 ∝ 助跑距离」的两个参数（`rts/combat_math.hpp` 的 charge_permille）
    // 与枪阵克骑的满动量幅度（anti_charge_permille，关系本身由三轴推导，
    // 见 `rts/unit_behavior.hpp` 的 counters_charge）。封顶 0 = 冲锋系统关。
    // anti 的下界 1000 是结构（载入器拦）：低于恒等就把「克」写成了「被克」。
    std::int32_t charge_bonus_permille_per_cell = 0;  // 每格助跑的伤害加成（千分比）
    float charge_max_cells = 0.0f;                    // 助跑封顶（格）
    std::int32_t anti_charge_permille = 1000;         // 枪阵对满动量冲锋的克制幅度
    // ——建筑等级上限——
    // `建筑等级上限 = ceil(堡垒等级 / 这个数)`（`波次预算曲线与堡垒等级曲线.md`
    // §2）。默认 1 是诚实默认：公式退化成「上限 = 堡垒等级」，合法但显然
    // 不是标定值。**`StatsLoader` 拦 < 1**——0 会在除法里炸。
    std::int32_t building_level_cap_divisor = 1;
    // ——兵种等级上限（守方升级轴第三个输出）——
    // `兵种等级上限(K) = K`（`波次预算曲线与堡垒等级曲线.md` §2）——直接等于
    // 堡垒等级，**没有除数**：这是与上面 `building_level_cap_divisor` 刻意的
    // 不对称，公式来源就没有那一层，不是漏抄。见 `World::unit_level_cap()`。
    //
    // 造价 `cost_gold(L) = base × L` 是纯线性，不需要系数（同文档 §3）。
    // 训练/升级耗时不是纯线性，需要一个千分比系数（`train_ticks(L) =
    // base × (1 + k×(L-1))`，`k` 就是它，语义是线性、**不开方**——训练耗时
    // 不参与 TTK / 破坏速率那组要求 p−q=0 的不变量，没有理由跟着开方。
    // 默认 0 = 恒等（训练耗时不随等级变），一眼看出没标定。
    std::int32_t train_ticks_permille_per_level = 0;
    // Fortifications: linear base-HP increments, followed by a restarted square
    // root whose first increment equals one linear step. Prices grow more slowly
    // after the same breakpoint, then freeze at the price of reaching the cap.
    std::int32_t fortification_hp_permille_per_level = 0;
    std::int32_t fortification_linear_until_level = 20;
    std::int32_t fortification_price_step_permille = 50;
    std::int32_t fortification_price_cap_level = 30;
    // （这里曾有 `unit_upgrade_radius`——已有部队批量升级的「在场」判定半径。
    // 就地升级随编队系统移除一并删除（用户定版：升级只体现在招募时带级），
    // 字段失去唯一消费者，留着只会是「标定了却没生效」的陷阱——删字段并进格，
    // 见上方 Stats/9 → Stats/10。）
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

    std::int64_t building_max_hp(BldType type, std::int32_t level) const noexcept;
    std::int64_t fortification_upgrade_cost(std::int64_t scale,
                                           std::int32_t from_level) const noexcept;
};

}  // namespace rts

#endif  // RTS_STATS_HPP
