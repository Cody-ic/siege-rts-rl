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
    // 动态场景版本（`game::BattleScene` 的产物：静态地砖 + 每帧重排的深度序列）。
    void draw(const std::vector<game::DrawItem>& tiles,
              const std::vector<game::DrawItem>& sorted);

    // 把绘制列表里出现的全部标识符预载一遍，缺素材立刻抛。
    // 理由见 SpriteAtlas::preload_idle：渲到一半才报错比一开始就报错难查得多。
    void preload(const game::DrawLists& lists);
    void preload(const std::vector<game::DrawItem>& tiles,
                 const std::vector<game::DrawItem>& sorted);

    // 每帧把「一个屏幕像素等于多少世界像素」告诉它（= 1 / 相机 zoom，与
    // `SceneOverlay` 那条描边宽度同一个做法）。血条用它对**屏幕**尺寸取下限。
    //
    // **注意语义变过一次，两个方向的失败都实测见过，改之前把两边都读完。**
    //
    // 原来是「血条纯按屏幕尺寸画、完全不随镜头缩放」，理由是：整张图入画时
    // zoom 很小，跟着缩放的血条会变成一根两像素高的短横线，「谁在挨打」读不
    // 出来。**那个理由没错，但它只看了一侧。** `px_per_tile = 256`，整张 72 格
    // 图入画时一格在屏幕上只有约 11 px，而恒定 42×7（含描边 45×10）的血条比
    // **整格宽四倍**——几条就能盖住它们标示的建筑，单位一多糊成一片（一次试玩
    // 反馈点出来的）；反过来 zoom≈1 时一格 256 px，同一条血条只占六分之一格。
    //
    // 现在是「世界尺寸为主 + 屏幕下限」（见 `scene_renderer.cpp` 血条那段）：
    // 相对大小恒定，而下限只在拉远那一侧生效，于是上面那根两像素横线仍被挡住。
    void set_screen_scale(float world_px_per_screen_px) noexcept {
        ui_scale_ = world_px_per_screen_px > 0.0f ? world_px_per_screen_px : 1.0f;
    }

    void set_presentation(bool enabled, float seconds) noexcept { presentation_=enabled; seconds_=seconds; }
private:
    bool presentation_ = true;
    float seconds_ = 0;
    void shadow(const game::DrawItem& item);
    // 关键一步：把**锚点**对齐到格心，而不是把图片左上角对齐到格心。
    // 各精灵画布尺寸不同（地砖 256×128、密林 532×758），按左上角贴会让高个子
    // 整体上浮，而那是一眼看得出、却不容易想到原因的错。
    void place(const game::DrawItem& item);

    SpriteAtlas* atlas_;
    game::IsoProjection proj_;
    float ui_scale_ = 1.0f;
};

}  // namespace render

#endif  // RENDER_SCENE_RENDERER_HPP
