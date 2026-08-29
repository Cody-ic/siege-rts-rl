#include "render/scene_overlay.hpp"

#include "game/display_names.hpp"

namespace render {

void SceneOverlay::draw_cell_outline(rts::GridPos p, Color color,
                                     float thickness) const {
    const rts::Vec2 c = proj_.grid_to_screen(p);
    const float hw = static_cast<float>(proj_.tile_w()) * 0.5f;
    const float hh = static_cast<float>(proj_.tile_h()) * 0.5f;

    // 菱形的四个顶点。**格心在中间**——这与 `screen_to_grid` 用 round 而不是 floor
    // 是同一件事的两面：若格心在菱形的角上，高亮框就会和点选的判定错开半格，
    // 而那时你会看到「高亮的不是我点的那格」，却很难说清是哪一边错了。
    const Vector2 top{c.x, c.y - hh};
    const Vector2 right{c.x + hw, c.y};
    const Vector2 bottom{c.x, c.y + hh};
    const Vector2 left{c.x - hw, c.y};

    DrawLineEx(top, right, thickness, color);
    DrawLineEx(right, bottom, thickness, color);
    DrawLineEx(bottom, left, thickness, color);
    DrawLineEx(left, top, thickness, color);
}

void SceneOverlay::draw_cell_readout(const game::MapData& map, rts::GridPos p,
                                     rts::Vec2 at, float size) const {
    const std::string text = game::describe_cell(map, p);
    const rts::Vec2 dim = font_->measure(text, size);
    const float pad = 6.0f;

    // 贴边时翻到另一侧，别让信息条跑出画面。信息条跟着光标走，
    // 而光标一定会被移到右下角去——不夹住的话它在那儿就是不可见的。
    const float sw = static_cast<float>(GetScreenWidth());
    const float sh = static_cast<float>(GetScreenHeight());
    if (at.x + dim.x + pad > sw) at.x = sw - dim.x - pad;
    if (at.y + dim.y + pad > sh) at.y = sh - dim.y - pad;
    if (at.x < pad) at.x = pad;
    if (at.y < pad) at.y = pad;

    // 先铺一块半透明底再写字。等距地图配色偏中间调，白字压在草地或石头上
    // 都可能读不出来——而信息条读不出来等于没有。
    DrawRectangleRec(Rectangle{at.x - pad, at.y - pad, dim.x + pad * 2, dim.y + pad * 2},
                     Color{18, 18, 24, 205});
    font_->draw(text, at, size, Color{235, 235, 245, 255});
}

}  // namespace render
