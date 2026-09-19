#ifndef RTS_VISIBILITY_HPP
#define RTS_VISIBILITY_HPP

#include "rts/terrain.hpp"
#include "rts/types.hpp"

namespace rts {

// 视线：两格之间的整数 Bresenham，检查**中间格**的 blocks_vision
// （两个端点不算——站在林子里的单位看得见自己与紧邻格）。
inline bool line_of_sight(const TerrainMasks& t, GridPos from, GridPos to) noexcept {
    int x0 = from.i, y0 = from.j;
    const int x1 = to.i, y1 = to.j;
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        if (!(x0 == from.i && y0 == from.j) && !(x0 == x1 && y0 == y1)) {
            if (t.blocks_vision(x0, y0)) return false;
        }
        if (x0 == x1 && y0 == y1) return true;
        const int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;   // 是 += dx 不是 += dy：写错的症状是**垂直**视线歪着走
                         // （纯竖直的射线在第二步开始横移），水平与对角反而全对，
                         // 于是只有「隔两格的正上/正下方」这种用例才抓得到
            y0 += sy;
        }
    }
}

}  // namespace rts

#endif
