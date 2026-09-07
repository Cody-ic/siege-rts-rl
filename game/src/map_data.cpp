#include "game/map_data.hpp"

#include <cassert>

namespace game {
bool MapData::gate_approach(rts::GridPos p) const noexcept {
    for(const auto& gate:walls_) {
        if(gate.kind!=WallKind::Gate) continue;
        const int dx=keep_.i-gate.pos.i,dy=keep_.j-gate.pos.j;
        const int ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
        const int sx=ax>=ay?(dx>0?1:-1):0,sy=ax>=ay?0:(dy>0?1:-1);
        const int px=p.i-gate.pos.i,py=p.j-gate.pos.j;
        const int inward=px*sx+py*sy,lateral=px*sy-py*sx;
        if(inward>=1 && inward<=3 && lateral>=-1 && lateral<=1) return true;
    }
    return false;
}


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
