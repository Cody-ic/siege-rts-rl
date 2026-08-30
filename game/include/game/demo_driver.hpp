// Demo 对局驱动：一张地图 + 数值表 → 一场脚本化攻防。
//
// 它是**演示与联调工具**。守方一侧现在走真的执行层脚本
// （`game::DefenderScript`，README 第 6 项的最终形态）——弓手拉扯、枪卫
// 堵口不追、游骑避骑士摸攻城锤都由脚本给出；攻方一侧仍是占位脚本
// （打得着就打、否则按 flow field 向堡垒推进），它的正式形态是逐单位 RL。
// 本类只做四件事：建世界、摆两边的开局兵力、每个决策周期发一遍动作、
// 推进 tick。
//
// 放在 `game/`（不在 `render/`）：它不含像素，且要在默认构建里被测——
// 「demo 跑 N tick 后墙被打破」是一条真断言，GCC 侧也验。

#ifndef GAME_DEMO_DRIVER_HPP
#define GAME_DEMO_DRIVER_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "game/defender_script.hpp"
#include "game/map_data.hpp"
#include "rts/flow.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"

namespace game {

class DemoBattle {
public:
    // 建世界并摆守方开局兵力（城内一小队 + 箭楼与防空各一座）。
    // 攻方**不再开局就位**：波次循环生效后，每波在建造阶段结束时于集结点
    // 生成（编成是占位曲线，无平衡含义），打完进下一波。
    DemoBattle(const MapData& map, const rts::StatsTable& stats, std::uint64_t seed);

    // 推进 `ticks` 个 tick，途中每个决策周期（kDecisionPeriodMax）重发一遍动作。
    // 波次循环也在这里驱动：建造倒计时 → 生波 → 攻方清空 → 下一波。
    // **败局（Keep 被拆）后世界定格**——再 update 也不推进，好让人看清最后一帧。
    void update(int ticks);

    const rts::World& world() const noexcept { return w_; }
    bool defeated() const noexcept { return defeated_; }
    int build_ticks_left() const noexcept { return build_left_; }

private:
    void issue_actions();
    void spawn_wave();
    bool keep_alive() const;
    rts::UnitAction greedy_move(rts::UnitId id, rts::Vec2 target) const;
    // 攻方推进：按 flow field 取下一步（机制第六批的消费侧）。field 指向
    // 被墙占着的格是正常输出——移动机制把那一步变成自动破坏，「绕远走缺口
    // vs 就近砸墙」由代价模型自己比较。不可达退回贪心（演示不卡死）。
    rts::UnitAction flow_step(rts::UnitId id);

    rts::World w_;
    // 守方执行层（参数取占位默认；种子从对局种子派生，demo 因此仍是确定性的）。
    DefenderScript script_;
    int build_left_ = 0;      // 建造阶段剩余 tick（时长是占位常量，见 .cpp）
    bool defeated_ = false;   // Keep 被拆即败（丢失即败是设计，不是演示便宜）
    int since_decision_ = 0;
    std::vector<rts::UnitId> ids_;
    std::vector<rts::UnitAction> acts_;
    // field 缓存：兵种 × 等级档，每个决策拍作废重算（墙血变了破坏代价就变）。
    // demo 的攻方全是 1 级（低档），但按契约的形状存——这就是「档数烤进
    // 下游缓存下标」的那个下游。档界用占位默认值（rts/flow.hpp）。
    rts::FlowTiering tiering_{};
    std::array<std::optional<rts::FlowField>,
               static_cast<std::size_t>(rts::kUnitTypeCount) * rts::kFlowTierCount>
        flow_{};
};

}  // namespace game

#endif  // GAME_DEMO_DRIVER_HPP
