// 等距投影：格坐标 ↔ 像素。**两个方向的数学各两行，但反方向有个坑。**
//
// 为什么在 `game/` 而不是 `render/`：这是 #40 立的那条切分准则的直接后果——
// **按「能不能在默认构建里被测」切，而不是按「渲染 / 非渲染」切**。
// `render/` 默认不配置（`RTS_BUILD_RENDER=OFF`），挂在它下面的测试在默认构建里
// 根本不存在、ctest 照样全绿；而下面的 `screen_to_grid` 正是「一行代码但错了
// 才发现」的那种（用 floor 代替 round，点选会稳定偏半格，见 §「那个坑」）。
// 它上一版放在 `render/` 是我放错了位置，这一版改回来。
//
// **这不违反 §7「像素几何只有一个来源」**：本类不含任何像素数字，`px_per_tile`
// 由构造参数注入，来源仍然是 `_sprite_meta.json`（经 `render::SpriteAtlas`）。
// 它只做换算，不持有几何常量。
//
// 反方向属于「玩家输入」这一档（点选、建造放置都要它把鼠标位置变成格），
// 而 CLAUDE.md 的架构分层把玩家输入划给 `game/`，所以正反两向在一起也是对的。

#ifndef GAME_ISO_PROJECTION_HPP
#define GAME_ISO_PROJECTION_HPP

#include <cmath>
#include <cstdint>

#include "rts/types.hpp"

namespace game {

// 世界像素空间里的一个矩形。刻意不用 raylib 的 `Rectangle`——`game/` 不链接图形库。
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

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
    // 这里不带 ox/oy——平移交给相机，那样缩放与平移是同一套机制。
    //
    // 一处跨语言的细节：Python 的 `//` 是**向下**取整，C++ 的 `/` 是**向零**取整。
    // 两者仅在被除数为负且为奇数时不同，而 `(gi - gj) * TW` 在 `TW` 为偶数时恒为偶数
    // （§7.1 要求 tile = [px_per_tile, px_per_tile / 2]，px_per_tile = 256）。
    // 所以两边现在逐像素相同；**若哪天 px_per_tile 变成奇数，这条对应关系会在
    // gi < gj 的那一半地图上断掉**，而症状是「照抄 Python 校验渲染结果」这条验收
    // 手段悄悄失效。写在这里以免它成为一个无人知道的前提。
    rts::Vec2 grid_to_screen(rts::GridPos p) const noexcept {
        const int i = p.i;
        const int j = p.j;
        return rts::Vec2{static_cast<float>((i - j) * tile_w_ / 2),
                         static_cast<float>((i + j) * tile_h_ / 2)};
    }

    // 世界像素 → 格坐标。`grid_to_screen` 的逆。
    //
    // 推导（令 tw、th 为格宽格高）：
    //     sx = (i - j) * tw / 2      sy = (i + j) * th / 2
    //  ⇒  i = sx / tw + sy / th      j = sy / th - sx / tw
    //
    // **那个坑：这里必须是 round，不能是 floor / 截断。**
    // `grid_to_screen` 给的是**格心**，所以格 (i, j) 的菱形是以那个点为中心的；
    // 连续解 i 落在 [i-0.5, i+0.5) 内都应当归到格 i。用 floor 就等于把归属区间
    // 平移了半格，后果是**点选稳定偏向左下一格**——不是随机出错，是每次都错同样的量，
    // 因此很容易被当成「美术锚点没对齐」而去改精灵，改到哪儿都不对。
    //
    // 截断（`static_cast<int>`）比 floor 更坏：它在正负号两侧偏向不同方向，
    // 于是地图左半边和右半边的错法还不一样。
    rts::GridPos screen_to_grid(rts::Vec2 world) const noexcept {
        const float tw = static_cast<float>(tile_w_);
        const float th = static_cast<float>(tile_h_);
        const float fi = world.x / tw + world.y / th;
        const float fj = world.y / th - world.x / tw;
        return rts::GridPos{round_to_i16(fi), round_to_i16(fj)};
    }

    // 整张 w×h 地图在世界像素里的包围盒。相机的「让整张图入画」要用它。
    //
    // 只看四个角就够：等距投影是线性的，所以格坐标矩形的像素像也是个（旋转 45° 的）
    // 矩形，极值必在角上。注意**这只是格心的包围盒**，不含精灵本身的高度——
    // 堡垒能高到 4 格边长，所以调用方要另留上边距。
    Rect grid_bounds(int w, int h) const noexcept {
        const rts::Vec2 a = grid_to_screen({0, 0});
        const rts::Vec2 b = grid_to_screen({static_cast<std::int16_t>(w - 1), 0});
        const rts::Vec2 c = grid_to_screen({0, static_cast<std::int16_t>(h - 1)});
        const rts::Vec2 d = grid_to_screen({static_cast<std::int16_t>(w - 1),
                                            static_cast<std::int16_t>(h - 1)});
        const float min_x = (b.x < c.x) ? b.x : c.x;   // 左极点在 (0, h-1) 或 (w-1, 0)
        const float max_x = (b.x > c.x) ? b.x : c.x;
        return Rect{min_x, a.y, max_x - min_x, d.y - a.y};
    }

private:
    // 四舍五入到 `GridPos` 的 int16。**越界时夹住而不是回绕**——回绕会把
    // 「鼠标在地图外很远处」变成「鼠标在地图另一角的合法格子上」，
    // 那是一个看起来像点选逻辑错的错。调用方仍要自己判在不在图内。
    static std::int16_t round_to_i16(float v) noexcept {
        const float r = std::round(v);
        if (r < -32768.0f) return -32768;
        if (r > 32767.0f) return 32767;
        return static_cast<std::int16_t>(r);
    }

    int tile_w_;
    int tile_h_;
};

}  // namespace game

#endif  // GAME_ISO_PROJECTION_HPP
