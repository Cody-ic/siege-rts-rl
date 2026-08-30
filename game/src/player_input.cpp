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

bool can_place_hint(const rts::WorldView& view, rts::GridPos cell) {
    if (cell.i < 0 || cell.j < 0 || cell.i >= view.width() ||
        cell.j >= view.height()) {
        return false;
    }
    // `buildable` 已经与 `no_build` 合成过（rts/terrain.hpp），别再 AND 一遍。
    if (!view.terrain().buildable(cell.i, cell.j)) return false;
    const auto b_pos = view.bld_pos();
    const auto b_alive = view.bld_alive();
    for (std::size_t k = 0; k < b_pos.size(); ++k) {
        if (b_alive[k] && b_pos[k] == cell) return false;
    }
    const auto o_pos = view.obstacle_pos();
    const auto o_alive = view.obstacle_alive();
    for (std::size_t k = 0; k < o_pos.size(); ++k) {
        if (o_alive[k] && o_pos[k] == cell) return false;
    }
    return true;
}

}  // namespace game
