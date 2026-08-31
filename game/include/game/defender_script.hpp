// 守方单兵执行层：一族参数化脚本（README 未认领工作第 6 项，就此认领落地）。
//
// ## 它是最终形态，不是「RL 做好之前的临时凑合」
//
// 判据与理由整段在 `守方AI与协同演化.md` 2.5：攻方需要见到的是**覆盖真人
// 水平区间的一个分布**，而 RL 只会收敛到一个点——脚本的微操强度是可调参数，
// RL 的不是。攻方在全部训练阶段面对的执行层都是它。
//
// ## 写到什么程度：克制二部图里已写死的战术行为进脚本，没写的留给 RL
//
// 逐条对照（同文 2.5 那张表）：
//
//   * `Archer` **拉扯**（克制表「Ghoul ──► Archer 拉扯」）：近战威胁贴近就
//     后撤拉开射程，威胁退出触发圈就回头放箭——「打一下退一步」由距离
//     震荡自然给出，不写状态机
//   * `Spear` **堵缺口不追击**（「Knight ──► Spear（开阔地）」）：打得着就打，
//     打不着**绝不朝敌人挪一步**——枪卫被风筝出阵位正是 Shade 克它的机制，
//     脚本不能亲手送
//   * `Ranger` **突袭而不对冲**（「Ram ──► Ranger 快速切入」+
//     「Ranger ──► Knight（骑兵对冲，遏制出城）」）：没有别的命令时主动
//     摸向敌方攻城锤；骑士进到保持距离内就脱离，不与之对冲
//
// 包夹、佯攻、择机——图里没写的一概不在此处，那是攻方 RL 的涌现空间。
//
// ## 它还是玩家命令的执行手：World 记账，脚本走路
//
// `MoveForce` / `Garrison` / `Clear` 在 `World` 里解算成**状态**
// （`force_target_` / `garrison_order_` / `o_clear_ordered_`），World 刻意
// 不代替单位走路（机制第二批的决定）。本类读这三张表把命令翻成逐单位动作：
// 编队开拔按 flow field 走（**穿城门出城靠它**，贪心会怼在自家墙上），
// 驻守的走到墙边等 World 自动登墙，清野的走过去撞上自动开始破坏。
// `Mason` 没有命令时自动找活干（工地 / 维修点半径内工时才会走）。
//
// ## 与仿真的关系：只读进、动作出
//
// 输入是 `WorldView`（不变量 2，编译期只读），输出是与
// `enumerate_units(Defender)` 同序的动作数组，由调用方 `submit_actions`。
// 脚本自身的状态（参数、RNG、威胁计时）**不是世界状态**：回放存的是脚本
// 选出的动作（契约 §4.6），所以换脚本、换参数都不触碰回放口径。
// 确定性：同种子 + 同输入序列 ⇒ 同输出序列（RNG 是 `rts::Rng`，掷点次序
// 跟着规范单位序走）。

#ifndef GAME_DEFENDER_SCRIPT_HPP
#define GAME_DEFENDER_SCRIPT_HPP

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "rts/action.hpp"
#include "rts/flow.hpp"
#include "rts/rng.hpp"
#include "rts/types.hpp"
#include "rts/world_view.hpp"

namespace game {

// 框选 + 右键下达的临时移动指令（#57 重开：框选只影响这次选中的单位，
// 不改变编队归属，见 CLAUDE.md「框选决定」）。**只有「开拔」这一种**——
// 驻守（登墙）与清野（破坏）的实际生效在 `rts_core` 里天生按编队/全局记账
// （`tick_garrison` 按 `u_force_` 挑人、`Clear` 标记是全局的），绕不开
// `World::submit`，所以那两种命令仍然走 `Command` 路径（见
// `game/player_input.hpp` 的 `distinct_forces()`），不需要这里的临时状态。
struct ManualOrder {
    bool active = false;
    rts::GridPos target{};
    // 配 `UnitId::generation()`：槎位被复用（旧单位死、新单位占了同一个
    // 下标）时旧指令必须视为失效，否则新单位会莫名其妙走向一个没人告诉过
    // 它的目标——`Handle` 的 tag/generation 机制原是防「拿地址/下标当 key」
    // 那类 bug，这里是它第一次被 `game/` 自己的每单位状态复用，同一条纪律。
    std::uint16_t generation = 0;
};

// 微操参数。**数值全部占位**（CLAUDE.md「关于数值」）；「一族」脚本 =
// 把它们在一个范围内随机化后各造一个实例（domain randomization，
// `守方AI与协同演化.md` 2.5 那张表的「可调 / 可控」列）。
struct ScriptParams {
    float kite_trigger_cells = 2.5f;    // 占位：近战威胁进此距离，弓手后撤
    std::int32_t kite_permille = 1000;  // 占位：每个决策拍真的后撤的概率
    std::int32_t reaction_decisions = 0;  // 占位：威胁持续几拍才响应（生疏度）
    float avoid_knight_cells = 3.0f;    // 占位：游骑对骑士的保持距离
    float arrive_cells = 1.5f;          // 占位：距编队目标多近算「到了」
    // 连续几拍「什么指令都没有」才真的挪去集结点，1 = 容忍 1 拍再动手。
    //
    // **这条不是可有可无的调优参数，是修一个真 bug 用的**：`World::submit`
    // 只入队，指令要到下一次 `advance()` 才落地（`rts_core 接口契约.md`
    // 的既有设计，不是 bug）。如果一个编队从「从没收到过指令」的状态
    // 刚被下了一条指令，中间恰好有一拍 `decide()` 先于 `advance()` 跑到、
    // 看到的还是「没有指令」——这一拍必须原地不动，不能真的挪向集结点，
    // 否则那一步会把单位带偏，而下一拍「指令生效」时的「最近选哪段墙/
    // 走哪条路」是按**那时的位置**算的，偏出去的那一步可能真的改变结果
    // （已被一条驻守测试的破坏性验证抓到过：两名弓手本该各奔一段墙，
    // 少了这条容忍就会一起挤向同一段）。
    std::int32_t muster_delay_decisions = 1;
};

class DefenderScript {
public:
    DefenderScript(ScriptParams p, std::uint64_t seed) noexcept
        : p_(p), rng_(seed) {}

    // 每个决策拍调一次。`ids` 必须是 `enumerate_units(Side::Defender, …)` 的
    // 产物（规范序）；`out` 与之同序，直接交给 `submit_actions`。
    void decide(const rts::WorldView& view, std::span<const rts::UnitId> ids,
                std::vector<rts::UnitAction>& out);

    // 给这批单位（必须是活着的守方单位）下一条临时开拔指令：只影响它们，
    // 不写 `force_target_`——同一编队里没被选中的单位不受影响，这是它与
    // `MoveForce` 的分界。优先级高于编队的持久指令（`follow_orders` 最前面
    // 查它）。到达目标格后自动清空，退回编队级默认值（或没有的话，退回
    // 集结点默认值）。
    void issue_move_order(std::span<const rts::UnitId> ids, rts::GridPos target);

private:
    rts::UnitAction decide_unit(const rts::WorldView& view, rts::UnitId id);
    // 返回 `nullopt` 表示这个单位**没有任何指令**（编队没有持久目标、
    // 也没有框选临时指令）——调用方据此决定要不要用类型自带的兜底
    // （`Ranger` 摸攻城锤 / `Mason` 找活干）或最终的集结点默认值。
    // 返回具体动作（哪怕是 `Stop`）表示「有指令，这就是它现在该做的事」
    // ——`Stop` 常见的含义是「已到达 / 正贴墙等着被拉上去」，这两种都不该
    // 被后面的兜底覆盖，所以不能靠「是不是 Stop」来判断有没有指令。
    std::optional<rts::UnitAction> follow_orders(const rts::WorldView& view,
                                                 rts::UnitId id, std::uint16_t mask);
    // 朝目标格走：flow field（按 (目标, 兵种, 档) 在一拍内缓存），
    // 不可达退回贪心。穿城门出城靠 field——贪心会怼在自家墙上。
    rts::UnitAction move_towards(const rts::WorldView& view, std::size_t slot,
                                 rts::GridPos goal, std::uint16_t mask);
    rts::UnitAction flee_from(const rts::WorldView& view, std::size_t slot,
                              rts::Vec2 threat, std::uint16_t mask);
    // 什么指令都没有时的最后一档：走向堡垒附近按编队错开的集结点，
    // 到了就停。**不要在还有类型自带兜底可试时调用它**（`Ranger`/`Mason`
    // 各自的兜底要排在它前面，见 `decide_unit`）。
    rts::UnitAction muster_fallback(const rts::WorldView& view, std::size_t slot,
                                    std::uint16_t mask);
    static rts::GridPos muster_point_for(std::uint8_t force, rts::GridPos keep) noexcept;

    ScriptParams p_;
    rts::Rng rng_;
    std::vector<std::int32_t> threat_streak_;   // 按槽位：威胁已持续的决策拍数
    std::vector<ManualOrder> manual_order_;      // 按槽位：框选下达的临时开拔指令
    // 按槽位：连续多少拍「没有任何指令」（`follow_orders` 返回 `nullopt`）。
    // 只用来给 `muster_fallback` 挡掉刚提交、还没落地那一拍的假阴性，见
    // `ScriptParams::muster_delay_decisions` 的注释。
    std::vector<std::int32_t> no_order_streak_;

    // 一拍内的缓存与预扫描（decide 开头重建）。
    struct CachedField {
        std::uint16_t goal;
        rts::UnitType type;
        int tier;
        rts::FlowField field;
    };
    std::vector<CachedField> fields_;
    std::vector<std::pair<std::uint16_t, std::uint8_t>> garrison_cells_;  // (格, 编队)
};

}  // namespace game

#endif  // GAME_DEFENDER_SCRIPT_HPP
