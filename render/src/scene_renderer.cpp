#include "render/scene_renderer.hpp"

#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace render {

void SceneRenderer::place(const game::DrawItem& item) {
    // 状态回落：不是每个实体都有每种状态（`_sprite_meta.json` 的 note：
    // 未声明状态的实体只有 idle）。回落是**这里**的知识——`game/` 不该知道
    // 素材有哪些状态，问了就是把素材侧知识泄进逻辑层（§7 的同族问题）。
    std::string_view state = item.state;
    if (state != "idle" && !atlas_->has_state(item.sprite, state)) state = "idle";

    // 动画帧：`anim` 是相位，帧数是素材侧知识，这里取模。
    int frame = 0;
    const std::vector<int>& frames = atlas_->frames_of(item.sprite, state);
    if (!frames.empty()) {
        frame = frames[static_cast<std::size_t>(item.anim) % frames.size()];
    }

    // 弹丸走「按飞行角旋转」的绘制路径，不按朝向选图：它只有一张 FREE 图
    // （横躺、箭头朝屏幕右 = 0°，§8.3）。角度从**投影后的**方向算——
    // 等距投影不保角，拿世界角直接用的症状是「箭大致朝目标飞、但总歪一个
    // 固定角度」，看着像弹道算错、不像投影用错。
    const bool projectile = atlas_->is_projectile(item.sprite);
    const Sprite& s =
        projectile ? atlas_->get(item.sprite, state, "FREE", frame)
                   : atlas_->get(item.sprite, state, game::to_string(item.facing), frame);
    // 单位在格间连续移动，用连续投影；静态项照旧按格心。
    rts::Vec2 c = item.continuous ? proj_.world_to_screen(item.world)
                                  : proj_.grid_to_screen(item.pos);
    if (projectile) {
        // 角度在抬升之前算（起点与目的地同高，抬升是纯视觉的整体平移）。
        const rts::Vec2 to = proj_.world_to_screen(item.aim);
        const float dx = to.x - c.x;
        const float dy = to.y - c.y;
        const float deg = (dx == 0.0f && dy == 0.0f)
                              ? 0.0f
                              : std::atan2(dy, dx) * (180.0f / 3.14159265f);
        c.y -= item.lift * static_cast<float>(proj_.tile_h());
        // 旋转中心必须是 `pivot`，不是 `ground_anchor`——后者是世界原点的
        // 投影、离箭的视觉中心实测差 19–26 px，拿它旋转的症状只在**转起来**
        // 之后可见：箭绕一个看不见的点公转（sprite_atlas.hpp 那段）。
        const Vector2 pivot = atlas_->pivot_of(item.sprite, state);
        const float w = static_cast<float>(s.texture.width);
        const float h = static_cast<float>(s.texture.height);
        DrawTexturePro(s.texture, Rectangle{0.0f, 0.0f, w, h},
                       Rectangle{c.x, c.y, w, h}, pivot, deg, WHITE);
        return;   // 弹丸不画血条
    }
    // 竖直提升（驻守单位站上墙顶）。`lift` 的单位是格高，像素换算在这一侧
    // （§7：像素几何只有一个来源）。血条跟着 c 一起抬，不用另算。
    c.y -= item.lift * static_cast<float>(proj_.tile_h());
    // 截断而不是四舍五入，与 `preview_map.py` 的 `int(x - ax)` 一致。
    // 锚点里确实有 .5（例如 Archer 的 227.5），两种取法差一个像素——
    // 差一个像素本身无所谓，但**两边不一致**会让「照抄那份 Python 校验渲染结果」
    // 这条验收手段失效。
    DrawTexture(s.texture, static_cast<int>(c.x - s.ground_anchor.x),
                static_cast<int>(c.y - s.ground_anchor.y), WHITE);

    // 血条：满血不画（画面干净，且「谁在挨打」一眼可见）。
    //
    // **尺寸按屏幕算，不随镜头缩放**（`set_screen_scale` 的注释讲了为什么）：
    // 下面几个数字是**屏幕像素**，乘 `ui_scale_` 换成世界像素。
    if (item.hp_frac >= 0.0f && item.hp_frac < 1.0f) {
        const float w = 42.0f * ui_scale_;
        const float h = 7.0f * ui_scale_;
        const float border = 1.5f * ui_scale_;
        const float gap = 5.0f * ui_scale_;
        const float x = c.x - w * 0.5f;
        // 竖直位置仍按**精灵**算（贴在它头顶），所以这一项不缩放——
        // 缩放它会让血条在拉近时飘到天上去。
        const float y = c.y - s.ground_anchor.y - h - gap;
        // 深色描边 + 空槽底。只画一个深色边框的话，掉了一半血的那一半是透明的，
        // 压在草地上读不出「还剩多少」——空槽必须是实心的。
        DrawRectangleRec(
            Rectangle{x - border, y - border, w + border * 2.0f, h + border * 2.0f},
            Color{12, 12, 16, 235});
        DrawRectangleRec(Rectangle{x, y, w, h}, Color{62, 46, 46, 235});
        const unsigned char r =
            static_cast<unsigned char>(230.0f * (1.0f - item.hp_frac) + 25.0f);
        const unsigned char g =
            static_cast<unsigned char>(200.0f * item.hp_frac + 30.0f);
        DrawRectangleRec(Rectangle{x, y, w * item.hp_frac, h}, Color{r, g, 60, 255});
    }
}

void SceneRenderer::draw(const game::DrawLists& lists) {
    // 第一遍：只铺地砖。地砖是平的、永远不遮挡任何东西，所以可以先整片铺完，
    // 不必进深度序列。
    for (const game::DrawItem& t : lists.tiles) place(t);

    // 第二遍：叠加物件与实体，`game/` 已经排好了。
    // **它们在同一个序列里**——站在岩壁前面的单位要遮住岩壁，站在后面的要被遮住。
    for (const game::DrawItem& o : lists.sorted) place(o);
}

void SceneRenderer::draw(const std::vector<game::DrawItem>& tiles,
                         const std::vector<game::DrawItem>& sorted) {
    for (const game::DrawItem& t : tiles) place(t);
    for (const game::DrawItem& o : sorted) place(o);
}

void SceneRenderer::preload(const game::DrawLists& lists) {
    std::set<std::string> idents;
    for (const game::DrawItem& t : lists.tiles) idents.insert(std::string(t.sprite));
    for (const game::DrawItem& o : lists.sorted) idents.insert(std::string(o.sprite));
    atlas_->preload_idle(std::vector<std::string>(idents.begin(), idents.end()));
}

void SceneRenderer::preload(const std::vector<game::DrawItem>& tiles,
                            const std::vector<game::DrawItem>& sorted) {
    std::set<std::string> idents;
    for (const game::DrawItem& t : tiles) idents.insert(std::string(t.sprite));
    for (const game::DrawItem& o : sorted) idents.insert(std::string(o.sprite));
    atlas_->preload_idle(std::vector<std::string>(idents.begin(), idents.end()));
}

}  // namespace render
