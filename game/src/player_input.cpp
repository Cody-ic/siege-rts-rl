#include "game/player_input.hpp"

#include <cassert>
#include <cstddef>

#include "rts/world.hpp"

namespace game {

ClickTarget classify_click(const rts::WorldView& view, rts::GridPos cell) {
    assert(cell.i >= 0 && cell.j >= 0 && cell.i < view.width() &&
           cell.j < view.height());
    // 己方完工的墙/门。「墙段」的判据是 Wall‖Gate，与机制第三批同一条。
    const auto b_type = view.bld_type();
    const auto b_pos = view.bld_pos();
    const auto b_built = view.bld_built();
    const auto b_alive = view.bld_alive();
    for (std::size_t k = 0; k < b_pos.size(); ++k) {
        if (!b_alive[k] || !(b_pos[k] == cell)) continue;
        if ((b_type[k] == rts::BldType::Wall || b_type[k] == rts::BldType::Gate) &&
            b_built[k] != 0) {
            return ClickTarget::Wall;
        }
        return ClickTarget::Ground;   // 点在别的建筑上：走到它旁边
    }
    const auto o_pos = view.obstacle_pos();
    const auto o_alive = view.obstacle_alive();
    for (std::size_t k = 0; k < o_pos.size(); ++k) {
        if (o_alive[k] && o_pos[k] == cell) return ClickTarget::Obstacle;
    }
    return ClickTarget::Ground;
}

std::vector<rts::UnitId> units_in_rect(const rts::WorldView& view,
                                       std::span<const rts::UnitId> ids,
                                       const IsoProjection& proj, Rect rect) {
    // 矩形的两个角谁大谁小不作要求——鼠标可能往任何方向拖。
    const float x0 = rect.width >= 0.0f ? rect.x : rect.x + rect.width;
    const float x1 = rect.width >= 0.0f ? rect.x + rect.width : rect.x;
    const float y0 = rect.height >= 0.0f ? rect.y : rect.y + rect.height;
    const float y1 = rect.height >= 0.0f ? rect.y + rect.height : rect.y;
    const auto pos = view.unit_pos();
    std::vector<rts::UnitId> out;
    for (const rts::UnitId id : ids) {
        const rts::Vec2 screen = proj.world_to_screen(pos[id.index()]);
        if (screen.x >= x0 && screen.x <= x1 && screen.y >= y0 && screen.y <= y1) {
            out.push_back(id);
        }
    }
    return out;
}

namespace {

bool in_map(const rts::WorldView& view, rts::GridPos cell) noexcept {
    return cell.i >= 0 && cell.j >= 0 && cell.i < view.width() &&
           cell.j < view.height();
}

// 这一格上活着的建筑在第几个槽位；没有则 -1。三个 hint 都要它。
int bld_slot_at(const rts::WorldView& view, rts::GridPos cell) noexcept {
    const auto b_pos = view.bld_pos();
    const auto b_alive = view.bld_alive();
    for (std::size_t k = 0; k < b_pos.size(); ++k) {
        if (b_alive[k] && b_pos[k] == cell) return static_cast<int>(k);
    }
    return -1;
}

rts::Command make(rts::CommandKind kind, rts::GridPos cell, int map_width) {
    rts::Command c;
    c.kind = kind;
    c.side = rts::Side::Defender;
    c.slot = rts::slot_of(cell, map_width);
    return c;
}

}  // namespace

const std::vector<rts::BldType>& buildable_types() {
    // 顺序是**玩起来的顺序**：先墙线（墙 / 门 / 木栅），再火力（箭楼 / 弩楼），
    // 再经济（采石 / 伐木 / 金矿），最后是产兵与视野（兵营 / 瞭望）。
    // TAB 一路按下去，正好是一局里操心它们的先后。
    //
    // `Keep` 不在列：不可再建（结构，`World` 的 Build 解算第一行）。
    static const std::vector<rts::BldType> kTypes = {
        rts::BldType::Wall,   rts::BldType::Gate,   rts::BldType::Fence,
        rts::BldType::Tower,  rts::BldType::Flak,   rts::BldType::Quarry,
        rts::BldType::Lumber, rts::BldType::Mine,   rts::BldType::Barrack,
        rts::BldType::Watch,
    };
    return kTypes;
}

bool can_place_hint(const rts::WorldView& view, rts::BldType bt, rts::GridPos cell) {
    if (!in_map(view, cell)) return false;
    // `buildable` 已经与 `no_build` 合成过（rts/terrain.hpp），别再 AND 一遍。
    if (!view.terrain().buildable(cell.i, cell.j)) return false;
    if (bld_slot_at(view, cell) >= 0) return false;
    const auto o_pos = view.obstacle_pos();
    const auto o_alive = view.obstacle_alive();
    for (std::size_t k = 0; k < o_pos.size(); ++k) {
        if (o_alive[k] && o_pos[k] == cell) return false;
    }
    // 资源点归属。**两个方向都要查**：采集建筑必须在对应种类的点上，
    // 其余建筑不得占点（把资源点糊死是不可逆的浪费，`World` 按结构封死）。
    const rts::ResourceSite* site = nullptr;
    for (const rts::ResourceSite& r : view.resources()) {
        if (r.pos == cell) {
            site = &r;
            break;
        }
    }
    if (rts::is_gatherer(bt)) {
        return site != nullptr && site->kind == rts::resource_of(bt);
    }
    return site == nullptr;
}

rts::Command build_command(rts::BldType bt, rts::GridPos cell, int map_width) {
    rts::Command c = make(rts::CommandKind::Build, cell, map_width);
    c.what = static_cast<std::uint8_t>(bt);
    return c;
}

bool can_afford_build(const rts::WorldView& view, rts::BldType bt) {
    const rts::BldStats& s = view.stats().of(bt);
    const auto stock = view.stock();
    return stock[static_cast<std::size_t>(rts::Resource::Stone)] >= s.cost_stone &&
          stock[static_cast<std::size_t>(rts::Resource::Wood)] >= s.cost_wood;
}

const std::vector<rts::UnitType>& trainable_types() {
    // 守方五种，顺序同「操作说明」里编队那一行的口径（弓手 / 枪卫 / 游骑
    // 在前，斥候与工匠在后）——两处说的是同一批兵，顺序不同会读得很别扭。
    static const std::vector<rts::UnitType> kTypes = {
        rts::UnitType::Archer, rts::UnitType::Spear, rts::UnitType::Ranger,
        rts::UnitType::Scout,  rts::UnitType::Mason,
    };
    return kTypes;
}

bool can_train_hint(const rts::WorldView& view, rts::GridPos cell) {
    if (!in_map(view, cell)) return false;
    const int k = bld_slot_at(view, cell);
    if (k < 0) return false;
    const auto idx = static_cast<std::size_t>(k);
    const rts::BldType bt = view.bld_type()[idx];
    if (bt != rts::BldType::Barrack && bt != rts::BldType::Keep) return false;
    if (view.bld_built()[idx] == 0) return false;
    // 一次一名（机制侧同一条）：正在练的兵营点了没反应，那就该是红的。
    return view.bld_train_type()[idx] == rts::kNoTrain;
}

bool can_afford_train(const rts::WorldView& view, rts::UnitType ut, std::int32_t level) {
    if (level < rts::kMinUnitLevel || level > view.unit_level_cap()) return false;
    return view.stock()[static_cast<std::size_t>(rts::Resource::Gold)] >=
           view.train_cost_gold(ut, level);
}

rts::Command train_command(rts::UnitType u, std::int32_t level,
                           rts::GridPos cell, int map_width) {
    rts::Command c = make(rts::CommandKind::Train, cell, map_width);
    c.what = static_cast<std::uint8_t>(u);
    c.level = static_cast<std::uint8_t>(level);
    return c;
}

bool can_repair_hint(const rts::WorldView& view, rts::GridPos cell) {
    if (!in_map(view, cell)) return false;
    const int k = bld_slot_at(view, cell);
    if (k < 0) return false;
    const auto idx = static_cast<std::size_t>(k);
    if (view.bld_built()[idx] == 0) return false;   // 工地不修
    if (view.bld_work_left()[idx] > 0) return false;   // 已经在修了
    return view.bld_hp()[idx] < view.bld_max_hp()[idx];
}

std::int64_t repair_wood_cost(const rts::WorldView& view, rts::GridPos cell) {
    const int k = bld_slot_at(view, cell);
    if (k < 0) return 0;
    const auto idx = static_cast<std::size_t>(k);
    const std::int64_t missing = view.bld_max_hp()[idx] - view.bld_hp()[idx];
    if (missing <= 0) return 0;   // 没有缺口，谈不上花不花钱
    // 与 `World` 的 `Repair` 解算逐字相同的公式（`rts_core/src/world.cpp`），
    // 抄一份而不是各自推导——两处算法分叉正是「绿框骗人」这类 bug 的成因。
    return (missing * view.stats().global.repair_wood_per_1000hp + 999) / 1000;
}

bool can_afford_repair(const rts::WorldView& view, rts::GridPos cell) {
    // 这一格没有建筑：`repair_wood_cost` 会返回 0，但那是「没有缺口」的
    // 返回值，不能借这条路把「格子本身无效」悄悄判成「买得起」。
    if (bld_slot_at(view, cell) < 0) return false;
    const std::int64_t wood = repair_wood_cost(view, cell);
    if (wood <= 0) return true;   // 没有缺口，谈不上花不花钱
    return view.stock()[static_cast<std::size_t>(rts::Resource::Wood)] >= wood;
}

rts::Command repair_command(rts::GridPos cell, int map_width) {
    return make(rts::CommandKind::Repair, cell, map_width);
}

rts::Command clear_command(rts::GridPos cell, int map_width) {
    return make(rts::CommandKind::Clear, cell, map_width);
}

UpgradeBlock upgrade_block(const rts::WorldView& view, rts::GridPos cell) {
    if (!in_map(view, cell)) return UpgradeBlock::NoBuilding;
    const int k = bld_slot_at(view, cell);
    if (k < 0) return UpgradeBlock::NoBuilding;
    const auto idx = static_cast<std::size_t>(k);
    if (view.bld_built()[idx] == 0) return UpgradeBlock::Unbuilt;
    // 在建/在修 与 已经在升 都归 `Busy`：对玩家是同一句话（「等这件工程
    // 完了再来」），而两者的区别（`b_work_` 还是 `b_upgrade_left_`）是
    // 实现细节，分成两档只会让文案多一条却不多给一点信息。
    if (view.bld_work_left()[idx] > 0) return UpgradeBlock::Busy;
    if (view.bld_upgrade_left()[idx] > 0) return UpgradeBlock::Busy;
    // `Keep` 不受等级上限约束，其余建筑受 `building_level_cap()` 约束——
    // 判据只在这里查一遍 `view.building_level_cap()`，不重新推
    // `rts_core/src/world.cpp` 那条公式（同 `repair_wood_cost` 的纪律）。
    if (view.bld_type()[idx] == rts::BldType::Keep) return UpgradeBlock::None;
    if (view.bld_level()[idx] >= view.building_level_cap()) {
        return UpgradeBlock::LevelCap;
    }
    return UpgradeBlock::None;
}

std::int32_t bld_level_at(const rts::WorldView& view, rts::GridPos cell) {
    if (!in_map(view, cell)) return 0;
    const int k = bld_slot_at(view, cell);
    if (k < 0) return 0;
    return view.bld_level()[static_cast<std::size_t>(k)];
}

bool can_upgrade_hint(const rts::WorldView& view, rts::GridPos cell) {
    return upgrade_block(view, cell) == UpgradeBlock::None;
}

std::int64_t upgrade_cost_stone(const rts::WorldView& view, rts::GridPos cell) {
    const int k = bld_slot_at(view, cell);
    if (k < 0) return 0;
    return view.stats().of(view.bld_type()[static_cast<std::size_t>(k)]).upgrade_cost_stone;
}

std::int64_t upgrade_cost_wood(const rts::WorldView& view, rts::GridPos cell) {
    const int k = bld_slot_at(view, cell);
    if (k < 0) return 0;
    return view.stats().of(view.bld_type()[static_cast<std::size_t>(k)]).upgrade_cost_wood;
}

bool can_afford_upgrade(const rts::WorldView& view, rts::GridPos cell) {
    if (bld_slot_at(view, cell) < 0) return false;
    return view.stock()[static_cast<std::size_t>(rts::Resource::Stone)] >=
               upgrade_cost_stone(view, cell) &&
           view.stock()[static_cast<std::size_t>(rts::Resource::Wood)] >=
               upgrade_cost_wood(view, cell);
}

rts::Command upgrade_command(rts::GridPos cell, int map_width) {
    return make(rts::CommandKind::Upgrade, cell, map_width);
}

}  // namespace game
