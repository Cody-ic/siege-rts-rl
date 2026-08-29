// 把 `game::DrawLists` 画出来。两遍绘制，这是深度排序的全部内容。
//
// **不变量 2 在类型层面的落地**：这个类拿到的只是一份绘制列表（格坐标 + 精灵标识符
// + 朝向 + 深度键），它**根本拿不到可变的仿真状态**，所以「渲染层不修改仿真状态」
// 不是靠约定守住的，是写不出来。等 `rts_core` 的只读视图出来，只需要加一个
// 「视图 → 绘制列表」的适配函数，这一层一行都不用改。

#ifndef RENDER_SCENE_RENDERER_HPP
#define RENDER_SCENE_RENDERER_HPP

#include "game/scene_model.hpp"
#include "game/iso_projection.hpp"
#include "render/sprite_atlas.hpp"

namespace render {

class SceneRenderer {
public:
    SceneRenderer(SpriteAtlas& atlas, game::IsoProjection projection) noexcept
        : atlas_(&atlas), proj_(projection) {}

    // 必须在 `BeginMode2D` 与 `EndMode2D` 之间调用——平移与缩放由相机负责，
    // 本类只管「哪张图贴在哪个世界坐标」。
    void draw(const game::DrawLists& lists);

    // 把绘制列表里出现的全部标识符预载一遍，缺素材立刻抛。
    // 理由见 SpriteAtlas::preload_idle：渲到一半才报错比一开始就报错难查得多。
    void preload(const game::DrawLists& lists);

private:
    // 关键一步：把**锚点**对齐到格心，而不是把图片左上角对齐到格心。
    // 各精灵画布尺寸不同（地砖 256×128、密林 532×758），按左上角贴会让高个子
    // 整体上浮，而那是一眼看得出、却不容易想到原因的错。
    void place(const game::DrawItem& item);

    SpriteAtlas* atlas_;
    game::IsoProjection proj_;
};

}  // namespace render

#endif  // RENDER_SCENE_RENDERER_HPP
