#include "render/scene_renderer.hpp"

#include <set>
#include <string>

namespace render {

void SceneRenderer::place(const game::DrawItem& item) {
    const Sprite& s = atlas_->get(item.sprite, "idle", game::to_string(item.facing));
    const Vector2 c = proj_.grid_to_screen(item.pos);
    // 截断而不是四舍五入，与 `preview_map.py` 的 `int(x - ax)` 一致。
    // 锚点里确实有 .5（例如 Archer 的 227.5），两种取法差一个像素——
    // 差一个像素本身无所谓，但**两边不一致**会让「照抄那份 Python 校验渲染结果」
    // 这条验收手段失效。
    DrawTexture(s.texture, static_cast<int>(c.x - s.ground_anchor.x),
                static_cast<int>(c.y - s.ground_anchor.y), WHITE);
}

void SceneRenderer::draw(const game::DrawLists& lists) {
    // 第一遍：只铺地砖。地砖是平的、永远不遮挡任何东西，所以可以先整片铺完，
    // 不必进深度序列。
    for (const game::DrawItem& t : lists.tiles) place(t);

    // 第二遍：叠加物件与实体，`game/` 已经排好了。
    // **它们在同一个序列里**——站在岩壁前面的单位要遮住岩壁，站在后面的要被遮住。
    for (const game::DrawItem& o : lists.sorted) place(o);
}

void SceneRenderer::preload(const game::DrawLists& lists) {
    std::set<std::string> idents;
    for (const game::DrawItem& t : lists.tiles) idents.insert(std::string(t.sprite));
    for (const game::DrawItem& o : lists.sorted) idents.insert(std::string(o.sprite));
    atlas_->preload_idle(std::vector<std::string>(idents.begin(), idents.end()));
}

}  // namespace render
