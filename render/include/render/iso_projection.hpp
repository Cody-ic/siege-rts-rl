// 格坐标 → 屏幕像素。**等距投影的全部数学就两行。**
//
// 这是整个程序里唯一知道 `px_per_tile` 的地方（值本身来自 `SpriteAtlas`，
// 而它来自 `_sprite_meta.json`——见 §7「像素几何只有一个来源」）。
// `game/` 那一侧的绘制列表刻意只有格坐标，就是为了让这条约束成立。

#ifndef RENDER_ISO_PROJECTION_HPP
#define RENDER_ISO_PROJECTION_HPP

#include <cstdint>

#include "raylib.h"
#include "rts/types.hpp"

namespace render {

class IsoProjection {
public:
    // `px_per_tile` 是一格菱形的像素**宽**。高固定为宽的一半（2:1 等距）。
    explicit IsoProjection(int px_per_tile) noexcept
        : tile_w_(px_per_tile), tile_h_(px_per_tile / 2) {}

    int tile_w() const noexcept { return tile_w_; }
    int tile_h() const noexcept { return tile_h_; }

    // 格心的世界像素坐标。与 `preview_map.py` 的 `grid_to_screen` 逐字对应：
    //     ox + (gi - gj) * TW // 2,  oy + (gi + gj) * TH // 2
    //
    // 这里不带 ox/oy——平移交给 raylib 的 Camera2D，那样缩放与平移是同一套机制。
    Vector2 grid_to_screen(rts::GridPos p) const noexcept {
        const int i = p.i;
        const int j = p.j;
        return Vector2{static_cast<float>((i - j) * tile_w_ / 2),
                       static_cast<float>((i + j) * tile_h_ / 2)};
    }

    // 整张 w×h 地图在世界像素里的包围盒。相机的「让整张图入画」要用它。
    //
    // 只看四个角就够：等距投影是线性的，所以格坐标矩形的像素像也是个（旋转 45° 的）
    // 矩形，极值必在角上。注意**这只是格心的包围盒**，不含精灵本身的高度——
    // 堡垒能高到 4 格边长，所以调用方要另留上边距。
    Rectangle grid_bounds(int w, int h) const noexcept {
        const Vector2 a = grid_to_screen({0, 0});
        const Vector2 b = grid_to_screen({static_cast<std::int16_t>(w - 1), 0});
        const Vector2 c = grid_to_screen({0, static_cast<std::int16_t>(h - 1)});
        const Vector2 d = grid_to_screen({static_cast<std::int16_t>(w - 1),
                                          static_cast<std::int16_t>(h - 1)});
        const float min_x = (b.x < c.x) ? b.x : c.x;   // 左极点在 (0, h-1) 或 (w-1, 0)
        const float max_x = (b.x > c.x) ? b.x : c.x;
        return Rectangle{min_x, a.y, max_x - min_x, d.y - a.y};
    }

private:
    int tile_w_;
    int tile_h_;
};

}  // namespace render

#endif  // RENDER_ISO_PROJECTION_HPP
