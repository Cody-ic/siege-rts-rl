#include "game/player_input.hpp"

#include <cassert>
#include <cstddef>

#include "rts/world.hpp"

namespace game {

rts::Command command_for_click(const rts::WorldView& view, std::uint8_t force,
                               rts::GridPos cell) {
    assert(cell.i >= 0 && cell.j >= 0 && cell.i < view.width() &&
           cell.j < view.height());
    rts::Command c;
    c.side = rts::Side::Defender;
    c.force = force;
    c.slot = rts::slot_of(cell, view.width());

    const auto b_type = view.bld_type();
    const auto b_pos = view.bld_pos();
    const auto b_built = view.bld_built();
    const auto b_alive = view.bld_alive();
    for (std::size_t k = 0; k < b_pos.size(); ++k) {
        if (!b_alive[k] || !(b_pos[k] == cell)) continue;
        const bool is_wall = b_type[k] == rts::BldType::Wall ||
                             b_type[k] == rts::BldType::Gate;
        if (is_wall && b_built[k] != 0) {
            c.kind = rts::CommandKind::Garrison;
            return c;
        }
        break;   // 点在别的建筑上：不驻守，落到 MoveForce（走到它旁边）
    }

    const auto o_pos = view.obstacle_pos();
    const auto o_alive = view.obstacle_alive();
    for (std::size_t k = 0; k < o_pos.size(); ++k) {
        if (o_alive[k] && o_pos[k] == cell) {
            c.kind = rts::CommandKind::Clear;
            return c;
        }
    }

    c.kind = rts::CommandKind::MoveForce;
    return c;
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

std::vector<std::uint8_t> distinct_forces(const rts::WorldView& view,
                                          std::span<const rts::UnitId> ids) {
    const auto force = view.unit_force();
    std::vector<std::uint8_t> out;
    for (const rts::UnitId id : ids) {
        const std::uint8_t f = force[id.index()];
        if (f == rts::kNoForce) continue;
        bool seen = false;
        for (const std::uint8_t x : out) {
            if (x == f) { seen = true; break; }
        }
        if (!seen) out.push_back(f);
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

bool can_afford_train(const rts::WorldView& view, rts::UnitType ut) {
    const rts::UnitStats& s = view.stats().of(ut);
    return view.stock()[static_cast<std::size_t>(rts::Resource::Gold)] >= s.cost_gold;
}

rts::Command train_command(rts::UnitType u, std::uint8_t force, rts::GridPos cell,
                           int map_width) {
    rts::Command c = make(rts::CommandKind::Train, cell, map_width);
    c.what = static_cast<std::uint8_t>(u);
    c.force = force;
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

bool can_afford_repair(const rts::WorldView& view, rts::GridPos cell) {
    const int k = bld_slot_at(view, cell);
    if (k < 0) return false;
    const auto idx = static_cast<std::size_t>(k);
    const std::int64_t missing = view.bld_max_hp()[idx] - view.bld_hp()[idx];
    if (missing <= 0) return true;   // 没有缺口，谈不上花不花钱
    // 与 `World` 的 `Repair` 解算逐字相同的公式（`rts_core/src/world.cpp`），
    // 抄一份而不是各自推导——两处算法分叉正是「绿框骗人」这类 bug 的成因。
    const std::int64_t wood =
        (missing * view.stats().global.repair_wood_per_1000hp + 999) / 1000;
    return view.stock()[static_cast<std::size_t>(rts::Resource::Wood)] >= wood;
}

rts::Command repair_command(rts::GridPos cell, int map_width) {
    return make(rts::CommandKind::Repair, cell, map_width);
}

}  // namespace game
