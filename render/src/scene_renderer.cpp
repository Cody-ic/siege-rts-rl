#include "render/scene_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>
#include "rts/roster.hpp"

namespace render {
namespace {
bool hostile(std::string_view id) {
    for(int i=rts::kDefenderUnitCount;i<rts::kUnitTypeCount;++i)
        if(rts::ident_of(static_cast<rts::UnitType>(i))==id) return true;
    return false;
}
}
void SceneRenderer::shadow(const game::DrawItem& item) {
    if(!presentation_ || atlas_->is_projectile(item.sprite) || !item.stand_on.empty()) return;
    const auto c=item.continuous?proj_.world_to_screen(item.world):proj_.grid_to_screen(item.pos);
    const float w=static_cast<float>(proj_.tile_w());
    if(item.sprite=="Keep") {
        const float r=w*1.35f;
        for(int ring=0;ring<2;++ring) {
            const float rx=r*(ring==0?1.0f:0.86f),ry=r*(ring==0?0.48f:0.41f);
            for(int j=0;j<64;++j) {
                const float a=static_cast<float>(j)*0.09817477f,b=a+0.09817477f;
                DrawLineEx({c.x+rx*std::cos(a),c.y+ry*std::sin(a)},
                           {c.x+rx*std::cos(b),c.y+ry*std::sin(b)},ui_scale_,Color{190,173,115,120});
            }
        }
        for(int i=0;i<8;++i) {
            const float angle=static_cast<float>(i)*0.785398f;
            const Vector2 p{c.x+std::cos(angle)*r,c.y+std::sin(angle)*r*0.48f};
            DrawLineEx({p.x-w*0.04f,p.y},{p.x+w*0.04f,p.y},w*0.012f,Color{218,194,127,165});
            DrawLineEx({p.x,p.y-w*0.035f},{p.x,p.y+w*0.035f},w*0.012f,Color{218,194,127,165});
        }
    }
    const bool unit=item.continuous;
    const float rx=w*(unit?0.16f:0.50f), ry=w*(unit?0.06f:0.14f);
    DrawEllipse(static_cast<int>(c.x+rx*0.4f),static_cast<int>(c.y+ry*0.3f),rx,ry,Color{19,28,36,48});
    if(unit && hostile(item.sprite)) {
        DrawEllipseLines(static_cast<int>(c.x),static_cast<int>(c.y),rx,ry,Color{148,198,200,145});
    }
}


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
        c.y -= item.lift * proj_.tile_z();
        // 旋转中心必须是 `pivot`，不是 `ground_anchor`——后者是世界原点的
        // 投影、离箭的视觉中心实测差 19–26 px，拿它旋转的症状只在**转起来**
        // 之后可见：箭绕一个看不见的点公转（sprite_atlas.hpp 那段）。
        const Vector2 pivot = atlas_->pivot_of(item.sprite, state);
        const float w = static_cast<float>(s.texture.width);
        const float h = static_cast<float>(s.texture.height);
        if(presentation_) {
            const float length=std::sqrt(dx*dx+dy*dy);
            if(length>0.01f) DrawLineEx({c.x-dx/length*proj_.tile_w()*0.24f,c.y-dy/length*proj_.tile_w()*0.24f},
                                      {c.x,c.y},std::max(2.0f,2.0f*ui_scale_),Color{231,224,189,150});
        }
        DrawTexturePro(s.texture, Rectangle{0.0f, 0.0f, w, h},
                       Rectangle{c.x, c.y, w, h}, pivot, deg, WHITE);
        return;   // 弹丸不画血条
    }
    // 竖直提升。像素换算在这一侧（§7：像素几何只有一个来源），血条跟着 c 一起抬。
    //
    // 两条路，量纲不同、来源也不同：
    //   * `stand_on`（驻守登顶）——抬多少由**那座建筑的精灵**说，见 `stand_lift_px`。
    //     城墙与门楼一高一矮，所以这一档不能是一个固定数。
    //   * `lift`（弹丸的飞行高度）——世界量，单位是竖直格边长，用 `tile_z()` 换算。
    //     **不是 `tile_h()`**：那是菱形半高、属于地面两轴，混用会让抬升偏小 22%。
    c.y -= item.lift * proj_.tile_z();
    if (!item.stand_on.empty()) c.y -= atlas_->stand_lift_px(item.stand_on);
    // 截断而不是四舍五入，与 `preview_map.py` 的 `int(x - ax)` 一致。
    // 锚点里确实有 .5（例如 Archer 的 227.5），两种取法差一个像素——
    // 差一个像素本身无所谓，但**两边不一致**会让「照抄那份 Python 校验渲染结果」
    // 这条验收手段失效。
    Color tint=WHITE;
    if(presentation_) {
        const bool terrain=item.sprite.substr(0,5)=="Plain" || item.sprite=="Forest";
        tint=terrain?Color{221,205,170,255}:Color{255,230,196,255};
        if(item.sprite=="Water") tint=Color{168,191,213,255};
        if(hostile(item.sprite)) tint=Color{183,206,226,255};
        if(item.sprite=="Phoenix") tint=Color{247,250,255,255};
    }
    DrawTexture(s.texture, static_cast<int>(c.x - s.ground_anchor.x),
                static_cast<int>(c.y - s.ground_anchor.y), tint);
    if(presentation_ && (item.sprite=="Keep" || item.sprite=="Gate")) {
        const float w=static_cast<float>(proj_.tile_w());
        const Vector2 pole{c.x+w*0.15f,c.y-s.ground_anchor.y*0.81f};
        const float flutter=std::sin(seconds_*2.4f+c.x)*w*0.018f;
        DrawLineEx(pole,{pole.x,pole.y-w*0.28f},w*0.012f,Color{171,141,86,255});
        DrawTriangle({pole.x,pole.y-w*0.28f},{pole.x,pole.y-w*0.10f},
                     {pole.x+w*0.22f,pole.y-w*0.17f+flutter},Color{43,69,77,255});
        DrawLineEx({pole.x+w*0.025f,pole.y-w*0.24f},
                   {pole.x+w*0.14f,pole.y-w*0.19f+flutter},w*0.014f,Color{216,191,133,255});
    }

    // 血条：满血不画（画面干净，且「谁在挨打」一眼可见）。
    //
    // ## 尺寸：以**世界**像素为主，但屏幕上不小于一个下限
    //
    // 这里原先是「纯屏幕尺寸、完全不随镜头缩放」（42×7 屏幕像素）。那个写法修的
    // 是一个真问题——拉远到整张图入画时 zoom 很小，跟着缩放的血条会变成一根两
    // 像素高的短横线，「谁在挨打」读不出来。**但它两头都不对**，一次试玩反馈点
    // 出来的：`px_per_tile = 256`，整张 72 格图入画时一格在屏幕上只有约 11 px，
    // 而血条含描边 45 px 宽——**比整格宽四倍**，几条血条就能盖住它们标示的建筑，
    // 单位一多糊成一片；反过来拉近到 zoom≈1 时一格 256 px，同一条血条只占六分
    // 之一格，细得像根线。
    //
    // 现在的形状按世界像素给（于是血条与单位的**相对**大小恒定，拉近拉远都像是
    // 场景里的东西），再对屏幕尺寸取下限。**下限只作用在拉远那一侧**，所以原来
    // 那个「两像素高读不出」的担心仍然被挡住——`kMinScreenW` 比它点名的 14 px
    // 宽出一半有余。
    //
    // **统一乘一个 `k` 而不是对宽高各自 clamp**：各自 clamp 会让宽高比在过渡区
    // 变形，血条会先变扁再变宽，比"稍微小一点"难看得多。
    if (item.hp_frac >= 0.0f && item.hp_frac < 1.0f) {
        const float kW = 112.0f;        // 世界像素，≈ 0.44 格
        const float kH = 16.0f;
        const float kMinScreenW = 22.0f;   // 屏幕像素下限
        const float k = std::max(kW, kMinScreenW * ui_scale_) / kW;
        const float w = kW * k;
        const float h = kH * k;
        const float border = 2.5f * k;
        const float gap = 12.0f * k;
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
    for (const auto& o : lists.sorted) shadow(o);
    for (const game::DrawItem& o : lists.sorted) place(o);
}

void SceneRenderer::draw(const std::vector<game::DrawItem>& tiles,
                         const std::vector<game::DrawItem>& sorted) {
    for (const game::DrawItem& t : tiles) place(t);
    for (const auto& o : sorted) shadow(o);
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
