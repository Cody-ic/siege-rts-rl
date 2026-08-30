// Demo 对局驱动：一张地图 + 数值表 → 一场脚本化攻防。
//
// 它是**演示与联调工具，不是守方脚本执行层**（README 未认领工作第 6 项是后者，
// 判据在 `守方AI与协同演化.md` 2.5——参数化、可随机化、承载克制表里写死的战术）。
// 本类只做四件事：建世界、摆两边的开局兵力、每个决策周期给双方发一遍
// 最傻的动作（守方有敌就打、攻方向堡垒推进）、推进 tick。
// 它的价值是让「机制第一批」有一个**能看**的整体形态——攻方过桥、
// 啃墙、破口、进城，守方箭塔与防空开火，全部由机制自己涌现，
// 脚本一行战术都没写。
//
// 放在 `game/`（不在 `render/`）：它不含像素，且要在默认构建里被测——
// 「demo 跑 N tick 后墙被打破」是一条真断言，GCC 侧也验。

#ifndef GAME_DEMO_DRIVER_HPP
#define GAME_DEMO_DRIVER_HPP

#include <cstdint>
#include <vector>

#include "game/map_data.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"

namespace game {

class DemoBattle {
public:
    // 建世界并摆开局兵力（守方城内一小队 + 箭楼与防空各一座，
    // 攻方在集结点一波混编）。兵力构成是演示用的定数，血量按表 × 等级算。
    DemoBattle(const MapData& map, const rts::StatsTable& stats, std::uint64_t seed);

    // 推进 `ticks` 个 tick，途中每个决策周期（kDecisionPeriodMax）重发一遍动作。
    void update(int ticks);

    const rts::World& world() const noexcept { return w_; }

private:
    void issue_actions();
    rts::UnitAction greedy_move(rts::UnitId id, rts::Vec2 target) const;

    rts::World w_;
    int since_decision_ = 0;
    std::vector<rts::UnitId> ids_;
    std::vector<rts::UnitAction> acts_;
};

}  // namespace game

#endif  // GAME_DEMO_DRIVER_HPP
