#include "game/world_builder.hpp"

#include <cmath>

namespace game {
namespace {

// 残血比例 → 绝对血量。
//
// 四舍五入而不是向下截断，理由与战斗解算那条（决定 ⑫ 第三子条）同源：
// 向下截断的偏置是单向的，全局稳定偏向**攻方**（初始城墙一律比设计值薄一点）。
//
// 钳到 [1, max]：`hp == 0` 在 `rts::World` 里是非法的（`0 < hp <= max_hp`），
// 而地图里「这一段是缺口」的表达方式是**不写这一段**，不是写一段 0 血的墙。
// 所以 `hp_frac` 极小的一段应当被当成"还剩一点点"，而不是被静默变成缺口。
std::int64_t scale_hp(float frac, std::int64_t max_hp) {
    const double v = std::floor(static_cast<double>(frac) * static_cast<double>(max_hp) +
                               0.5);
    if (v < 1.0) return 1;
    if (v > static_cast<double>(max_hp)) return max_hp;
    return static_cast<std::int64_t>(v);
}

}  // namespace

rts::WorldInit make_world_init(const MapData& map, const rts::StatsTable& stats,
                              std::uint64_t seed, std::int32_t nominal_level,
                              const std::array<unsigned char, 32>& content_hash) {
    rts::WorldInit init;
    init.width = map.width();
    init.height = map.height();
    init.seed = seed;
    init.nominal_level = nominal_level;
    init.map_id = map.map_id();
    init.map_content_hash = content_hash;
    init.keep = map.keep();
    init.stats = stats;

    // 地形与 no_build 都摊平成行主序，与 `MapData` 内部同序（`y * width + x`）。
    // **这一步没有转置的机会**：两侧用同一个下标式子，而那个式子在
    // `MapData` 那边由非方形夹具钉住（`tests/map_loader_test.cpp`）。
    const std::size_t cells =
        static_cast<std::size_t>(map.width()) * static_cast<std::size_t>(map.height());
    init.terrain.resize(cells);
    init.no_build.resize(cells);
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const std::size_t k = static_cast<std::size_t>(y) *
                                      static_cast<std::size_t>(map.width()) +
                                  static_cast<std::size_t>(x);
            init.terrain[k] = map.terrain_at(x, y);
            init.no_build[k] = map.no_build_at(x, y) ? std::uint8_t{1} : std::uint8_t{0};
        }
    }

    init.spawns.reserve(map.spawns().size());
    for (const SpawnPoint& s : map.spawns()) {
        // **走廊种类刻意不带过去**，理由见 `rts::SpawnSite` 的注释：
        // 「每个集结点对应一条性质不同的走廊」是对**地图**的结构约束，
        // 仿真不消费它；带过去会诱使有人按走廊种类写分支。
        init.spawns.push_back(rts::SpawnSite{s.pos});
    }

    init.resources.reserve(map.resources().size());
    for (const ResourceNode& r : map.resources()) {
        // `ResourceTier`（城内 / 墙外）同样不带：它是**描述**——哪些资源点在墙内
        // 取决于玩家把墙建在哪，而墙会变。仿真要判「这个点在墙内吗」得看当前墙况，
        // 不能看地图作者当初的标注。
        init.resources.push_back(rts::ResourceSite{r.pos, r.type});
    }

    // 领主堡垒。**地图文件里没有它**（`walls` 只有 `Wall` / `Gate`），
    // 而 `rts::World` 要求恰好一座——「丢失即败」这个败北条件否则无从表达。
    // 合成它正是这个适配层的活。
    const std::int64_t keep_hp = stats.of(rts::BldType::Keep).max_hp;
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, map.keep(),
                                          keep_hp, keep_hp});

    init.buildings.reserve(init.buildings.size() + map.walls().size());
    for (const WallSegment& w : map.walls()) {
        const bool is_gate = w.kind == WallKind::Gate;
        const rts::BldType type = is_gate ? rts::BldType::Gate : rts::BldType::Wall;
        const std::int64_t max_hp = stats.of(type).max_hp;
        init.buildings.push_back(
            rts::BldInit{type, w.pos, scale_hp(w.hp_frac, max_hp), max_hp});
    }

    // 可破坏障碍。**满血进场**——`ObstacleNode` 不带残血比例，理由见它的注释
    // （城墙的残血来自 2.3 的设计要求，障碍没有对应的要求）。
    init.obstacles.reserve(map.obstacles().size());
    for (const ObstacleNode& o : map.obstacles()) {
        const std::int64_t max_hp = stats.of(o.type).max_hp;
        init.obstacles.push_back(rts::ObstacleInit{o.type, o.pos, max_hp, max_hp});
    }

    return init;
}

}  // namespace game
