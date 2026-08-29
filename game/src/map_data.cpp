#include "game/map_data.hpp"

#include <cassert>

namespace game {

Terrain MapData::terrain_at(int x, int y) const noexcept {
    assert(in_bounds(x, y));
    return terrain_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                    static_cast<std::size_t>(x)];
}

bool MapData::no_build_at(int x, int y) const noexcept {
    assert(in_bounds(x, y));
    return no_build_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                     static_cast<std::size_t>(x)];
}

const WallSegment* MapData::wall_at(int x, int y) const noexcept {
    for (const WallSegment& w : walls_) {
        if (w.pos.i == x && w.pos.j == y) return &w;
    }
    return nullptr;
}

}  // namespace game
