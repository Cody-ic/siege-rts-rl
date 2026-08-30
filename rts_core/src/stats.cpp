#include "rts/stats.hpp"

#include "rts/hash.hpp"

namespace rts {

// 喂入顺序：形状标签 → 单位（枚举序，逐字段）→ 建筑 → 障碍 → 全局。
// **改字段、改类型、改顺序都要把 `kStatsShapeTag` 进一格**（头文件那条纪律）。
std::uint64_t StatsTable::fingerprint() const noexcept {
    StateHash h;
    h.feed_text(kStatsShapeTag);

    for (const UnitStats& s : unit) {
        h.feed_pod(s.max_hp);
        h.feed_pod(s.damage);
        h.feed_f32(s.range);
        h.feed_f32(s.speed);
        h.feed_f32(s.vision);
        h.feed_pod(s.windup_ticks);
        h.feed_pod(s.cooldown_ticks);
        h.feed_pod(s.vs_structure_permille);
        h.feed_f32(s.aoe_radius);
        h.feed_f32(s.proj_speed);
        h.feed_pod(s.cost_gold);
        h.feed_pod(s.train_ticks);
    }
    for (const BldStats& s : bld) {
        h.feed_pod(s.max_hp);
        h.feed_pod(s.damage);
        h.feed_f32(s.range);
        h.feed_f32(s.vision);
        h.feed_pod(s.windup_ticks);
        h.feed_pod(s.cooldown_ticks);
        h.feed_pod(s.cost_stone);
        h.feed_pod(s.cost_wood);
        h.feed_pod(s.build_ticks);
        h.feed_pod(s.income_amount);
        h.feed_f32(s.aoe_radius);
        h.feed_f32(s.proj_speed);
    }
    for (const ObstacleStats& s : obstacle) {
        h.feed_pod(s.max_hp);
        h.feed_pod(s.yield_amount);
    }
    h.feed_pod(global.hp_permille_per_level);
    h.feed_pod(global.dmg_permille_per_level);
    h.feed_pod(global.income_period_ticks);
    h.feed_f32(global.mason_work_radius);
    h.feed_pod(global.repair_hp_per_work_tick);
    h.feed_pod(global.repair_wood_per_1000hp);
    h.feed_pod(global.cancel_refund_permille);
    h.feed_pod(global.garrison_mount_ticks);
    h.feed_pod(global.high_ground_miss_permille);
    h.feed_pod(global.high_ground_dmg_permille);
    h.feed_f32(global.high_ground_range_bonus);
    h.feed_pod(global.charge_bonus_permille_per_cell);
    h.feed_f32(global.charge_max_cells);
    h.feed_pod(global.anti_charge_permille);

    return h.value();
}

}  // namespace rts
