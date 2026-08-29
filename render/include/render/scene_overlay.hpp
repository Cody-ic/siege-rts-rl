// 场景内 UI：世界空间的格高亮 + 光标旁的信息条。
//
// **为什么在 raylib 这边而不是留给 ImGui**：CLAUDE.md 的分工是「Dear ImGui 负责
// 面板与 HUD，raylib 负责世界渲染与场景内 UI（选择框、血条、建造虚影）」。
// 格高亮与贴着光标的信息条属于后者——它们要跟着世界坐标走、要被相机缩放，
// 交给一个屏幕空间的 UI 库反而别扭。所以这一层不是 ImGui 落地前的临时物，
// ImGui 来了它照样在。
//
// 里面**没有一个假数据**：显示的每一项都从 `MapData` 读。这是刻意的——
// 排期表原先写的是「面板先接假数据」，但硬编码的数字在界面上和真数据长得一模一样，
// 于是「这个面板到底接没接上」变成一个要读代码才能回答的问题。宁可少显示几项。

#ifndef RENDER_SCENE_OVERLAY_HPP
#define RENDER_SCENE_OVERLAY_HPP

#include <string>

#include "game/iso_projection.hpp"
#include "game/map_data.hpp"
#include "raylib.h"
#include "render/text.hpp"
#include "rts/types.hpp"

namespace render {

class SceneOverlay {
public:
    SceneOverlay(const FontSet& font, game::IsoProjection proj) noexcept
        : font_(&font), proj_(proj) {}

    // 一格菱形的描边。**必须在 `BeginMode2D` / `EndMode2D` 之间调用**
    // ——它给的是世界坐标，缩放平移交给相机。
    void draw_cell_outline(rts::GridPos p, Color color, float thickness) const;

    // 光标旁的信息条：地形 + 这一格上有什么。**必须在 `BeginMode2D` 之外调用**
    // ——它贴着屏幕上的光标，不该随相机缩放变大变小。
    //
    // 文本由 `game::describe_cell()` 组装。**那一步刻意不在这里**：它是纯逻辑、
    // 无图形依赖，放进 `game/` 才进默认构建、才能被测——而它要测的东西很具体
    // （越界、有墙段、资源点、禁建各自出什么字），且它产出的每个字符都必须已登记，
    // 那条也做成了测试。见 `game/display_names.hpp`。
    void draw_cell_readout(const game::MapData& map, rts::GridPos p, rts::Vec2 at,
                           float size) const;

private:
    const FontSet* font_;
    game::IsoProjection proj_;
};

}  // namespace render

#endif  // RENDER_SCENE_OVERLAY_HPP
