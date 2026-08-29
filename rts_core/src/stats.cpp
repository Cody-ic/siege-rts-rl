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
    }
    for (const BldStats& s : bld) {
        h.feed_pod(s.max_hp);
        h.feed_pod(s.damage);
        h.feed_f32(s.range);
        h.feed_f32(s.vision);
        h.feed_pod(s.windup_ticks);
        h.feed_pod(s.cooldown_ticks);
    }
    for (const ObstacleStats& s : obstacle) {
        h.feed_pod(s.max_hp);
        h.feed_pod(s.yield_amount);
    }
    h.feed_pod(global.hp_permille_per_level);
    h.feed_pod(global.dmg_permille_per_level);

    return h.value();
}

}  // namespace rts
