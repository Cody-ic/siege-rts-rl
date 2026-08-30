// flow field 寻路（机制第六批，1c 收尾）：从目标格集合反向多源 Dijkstra，
// 给每格一个「朝目标去的下一步」与代价（tick）。
//
// ## 它是派生数据，不是世界状态——这一条决定了它的一切形状
//
// field 是 `WorldView` 之上的**纯函数**：不进 `World`、不进 `state_hash`、
// 不进回放。回放只存外生输入（契约 §4.6），而脚本按 field 选出的**动作**
// 才是那个外生输入——field 本身重算即得，存它等于给同一件事两个真相来源。
// 所以改本文件的代价模型不触碰 `kWorldHashTag`，旧回放照样验（它们存的是
// 当初的动作，不是当初的 field）。
//
// 确定性仍然要守（训练与演示都要同输入同输出）：代价按固定顺序累加、
// 队列按（代价, 格下标）全序弹出、只有严格更优才改写——同平台逐位可复现，
// 与仿真同一档承诺（不跨平台）。
//
// ## 档数 = 3：组内定夺（#57，2026-08-30），不是本文件的自由
//
// CLAUDE.md：「field 是**兵种 × 等级档**而非兵种尺度的常数——等级要量化成
// 少数几档破坏速率，档数是接口契约里要定的形状」（`无尽模式与地形分层.md`
// §7.7.5）。备选 1 / 2 / 3 / 动态四案的得失摆在 #57 那条评论里；3 档
// （低 / 中 / 高）当选的两条主因：2 档的表达力在固定边界下会随波数塌缩
// （「本波的普通兵」整体上移，后期两档并成一档），动态档把契约形状从常数
// 变成变量、工期不值。**改档数改这一个常量**，field 份数与下游缓存随之而变。
//
// ## 档界不进 `StatsTable`，是有意的
//
// 「待标定数值只能从 stats 进仿真」那条纪律的对象是**仿真**——`advance()`
// 从头到尾不读 field。表指纹的职责是把「布局改了」与「跑歪了」分开（进
// `state_hash` 与回放头），而档界两者都不改：收进表里，改一个不影响推演的
// 旋钮就会让仍然有效的旧回放谎报 `StatsMismatch`。所以档界是本 API 的参数
// （`FlowTiering`），默认值是**占位**、按纪律标注；标定期由调用方从数据文件
// 传真值，不改这里。
//
// ## 代价模型：行军 + 破坏，「墙段按高代价可通行」的另一半
//
// 进一格的代价 = 步长几何 ÷ 速度（正交 1、斜向 √2，与 tick_movement 同一个
// kSqrt2）+ 格上实体的破坏时间。破坏时间 = ceil(血量 ÷ 单发结构伤害) ×
// (前摇 + 冷却)；单发结构伤害与机制走**同一对函数**（combat_math 的
// apply_permille × level_permille × vs_structure_permille），唯独不带冲锋
// 倍率——动量是瞬态，field 是稳态估计。**它是选择的启发量，不是结算承诺**：
// 失真只挪「绕缺口 vs 就近砸墙」的拐点，不影响任何正确性、确定性、回放。
//
// 墙血那一侧**逐格精确**（读当前 hp）；档只量化速率这一侧。档的代表等级取
// **档下界**：两种失真里，「低估自己、多绕一段」远比「高估自己、站在杀伤区
// 啃一面啃不穿的墙」便宜，取下界把后一种在结构上砍掉。
//
// ## 通行规则与 tick_movement 的 cell_open 逐条对应
//
//   * 地形不可通行 = 不可入；空中单位无视一切（地形与实体都不挡）
//   * 敌方实体 = 破坏代价（墙 / 门要 can_break_structure，其余建筑要 combat()
//     ——与 try_bump_attack 同一对谓词）；破不动 = 不可入
//   * 己方建筑 = 不可入，**唯一例外是完工的城门**（与移动机制同一条例外，
//     两处必须同真值——否则 field 指的路移动走不了）
//   * 斜向要求两个正交角**完全敞开**（kDiagonalNeedsBothOrthogonal 对实体
//     同样成立，机制如此）。机制里斜向撞击可以打斜角的墙，field 刻意不给
//     这种路线——破坏路线由正交边承担，保守但永不指一条走不通的路
//   * **目标格一律视为敞开**：进目标格不付破坏代价。它对所有路径是同一个
//     常数（每条路恰好进目标格一次），不改任何方向；且「到得了旁边」的兵种
//     （斥候摸向 Keep）不因目标格上的建筑而整场不可达

#ifndef RTS_FLOW_HPP
#define RTS_FLOW_HPP

#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "rts/action.hpp"
#include "rts/roster.hpp"
#include "rts/types.hpp"

namespace rts {

class WorldView;

// 等级档数。**形状，改它 = 改契约**（field 份数、下游缓存下标、train/ 侧的
// 沟通词汇都烤着它）；档界是数值，在下面的 FlowTiering 里，两者不是一个层。
inline constexpr int kFlowTierCount = 3;

inline constexpr std::uint8_t kFlowNoDir = 0xFF;

// 档界（占位值，待标定——按 CLAUDE.md「关于数值」的纪律标注）。
// 语义：等级 < mid_from 归低档，< high_from 归中档，其余归高档。
// 不进 StatsTable 的理由见文件头；标定期由调用方从数据文件传真值。
struct FlowTiering {
    std::int32_t mid_from = 4;    // 占位
    std::int32_t high_from = 8;   // 占位
};

constexpr int flow_tier_of(std::int32_t level, const FlowTiering& t) noexcept {
    return level >= t.high_from ? 2 : (level >= t.mid_from ? 1 : 0);
}

// 档的代表等级 = 档下界（保守端，理由见文件头「代价模型」）。低档恒为 1：
// 等级从 1 起（combat_math 的 level_permille 有同一条断言）。
constexpr std::int32_t flow_rep_level(int tier, const FlowTiering& t) noexcept {
    return tier <= 0 ? 1 : (tier == 1 ? t.mid_from : t.high_from);
}

class FlowField {
public:
    FlowField() = default;

    // 兵种 × 档 × 目标集合 → 一张 field。调用方按 (mover, tier) 缓存与失效
    // （墙血变了破坏代价就变了——重算节律归调用方，demo 取每个决策拍）。
    static FlowField compute(const WorldView& view, UnitType mover, int tier,
                             std::span<const GridPos> goals,
                             const FlowTiering& tiering = {});

    int width() const noexcept { return w_; }
    int height() const noexcept { return h_; }
    int tier() const noexcept { return tier_; }

    bool in_bounds(GridPos p) const noexcept {
        return p.i >= 0 && p.j >= 0 && p.i < w_ && p.j < h_;
    }
    bool reachable(GridPos p) const noexcept {
        return in_bounds(p) && cost_at(p) < std::numeric_limits<float>::infinity();
    }
    // 到目标的估计代价（tick）。不可达 / 越界 = +inf。
    float cost_at(GridPos p) const noexcept {
        return in_bounds(p) ? cost_[idx(p)] : std::numeric_limits<float>::infinity();
    }
    // 朝目标的下一步。目标格上 / 不可达 / 越界 = Stop（停住永远合法）。
    // 指向被实体占着的格是**正常输出**——移动机制会把那一步变成自动破坏。
    UnitAction step_of(GridPos p) const noexcept {
        if (!in_bounds(p)) return UnitAction::Stop;
        const std::uint8_t d = dir_[idx(p)];
        return d == kFlowNoDir ? UnitAction::Stop : move_of(static_cast<int>(d));
    }

private:
    std::size_t idx(GridPos p) const noexcept {
        return static_cast<std::size_t>(p.j) * static_cast<std::size_t>(w_) +
               static_cast<std::size_t>(p.i);
    }

    int w_ = 0;
    int h_ = 0;
    int tier_ = 0;
    std::vector<float> cost_;
    std::vector<std::uint8_t> dir_;   // 0..7（move_of 的下标），kFlowNoDir = 无
};

}  // namespace rts

#endif  // RTS_FLOW_HPP
