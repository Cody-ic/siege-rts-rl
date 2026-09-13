#include "game/battle_scene.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>

#include "rts/fog.hpp"
#include "rts/roster.hpp"

namespace game {
namespace {

// 世界坐标增量 → 四方位。主导轴定胜负，平手让给 x 轴（等距下左右比上下醒目）。
Facing facing_from_delta(float dx, float dy) noexcept {
    const float ax = dx >= 0 ? dx : -dx;
    const float ay = dy >= 0 ? dy : -dy;
    if (ax >= ay) return dx >= 0 ? Facing::SE : Facing::NW;
    return dy >= 0 ? Facing::SW : Facing::NE;
}

float hp_frac_of(std::int64_t hp, std::int64_t max_hp) noexcept {
    if (max_hp <= 0) return 0.0f;
    const float f = static_cast<float>(hp) / static_cast<float>(max_hp);
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

// 驻守槽位 `slot` 那一格上，人踩着的是哪座建筑的精灵。
//
// 走一遍建筑数组而不是查一张格→建筑的表：`WorldView` 没有暴露那张表
// （`bld_at_` 是 `World` 的私有派生缓存，刻意不进哈希），而驻守单位是个位数、
// 建筑是几百，逐帧的这点开销远不值得为它开一个新的公开接口。
//
// **查不到就返回空**（人画在地面上）而不是猜一个 `Wall`：墙被拆掉的那一拍，
// `destroy_bld` 会清掉驻守指令、把人放到缺口里，画在地面才是对的。
std::string_view garrison_stand_sprite(const rts::WorldView& view,
                                       std::uint16_t slot) noexcept {
    const auto b_alive = view.bld_alive();
    const auto b_type = view.bld_type();
    const auto b_pos = view.bld_pos();
    const auto b_built = view.bld_built();
    for (std::size_t k = 0; k < b_alive.size(); ++k) {
        if (b_alive[k] == 0 || b_built[k] == 0) continue;
        if (b_type[k] != rts::BldType::Wall && b_type[k] != rts::BldType::Gate) continue;
        if (rts::slot_of(b_pos[k], view.width()) != slot) continue;
        return rts::ident_of(b_type[k]);
    }
    return {};
}

}  // namespace

Facing BattleScene::facing_of(rts::UnitAction a, rts::Vec2 pos, rts::Vec2 aim,
                              bool winding) noexcept {
    if (winding) return facing_from_delta(aim.x - pos.x, aim.y - pos.y);
    if (rts::is_move(a)) {
        const rts::GridDelta d = rts::move_delta(a);
        return facing_from_delta(static_cast<float>(d.di), static_cast<float>(d.dj));
    }
    return Facing::SE;
}

std::vector<DrawItem> BattleScene::tiles(const MapData& map) {
    // 地砖那一半与静态装配完全相同，直接复用（同一份规则只实现一次）。
    return SceneModel::build(map).tiles;
}

std::vector<DrawItem> BattleScene::sorted(const MapData& map,
                                          const rts::WorldView& view, rts::Tick now) {
    std::vector<DrawItem> out;

    // ——迷雾：敌方的东西只在**当前可见**的格上出现——
    //
    // 2026-09-01。此前本函数把 god 视角（`WorldView` 本来就是，那是不变量 2）里的
    // 一切原样画出，于是「编成构成」与「精确进攻方向」——CLAUDE.md 的情报划分里
    // 明列为**需要侦查**的那两项——对玩家全部免费。后果不是「少了个效果」，是
    // **整个侦查系统形同虚设**：`Scout`（25 金）与 `Watch` 没有任何理由被造出来，
    // 「等 `Wraith` 走了再造防空」这类玩家欺骗手段也无从发生。
    //
    // 判据与来源逐字照 `rts_core/src/obs_pack.cpp` 那一段（硬要求 1 的落点，
    // 也是全仓唯一的先例）：**只经 `view.fog()` 这一个来源读可见性**，
    // 多一个来源就多一条绕过它的路；越界回落 `Unseen`。
    //
    // **只过滤敌方的动态实体。** 地形、地形叠加物、中立障碍、资源标记、己方一切
    // 一律照画——地形不过迷雾（同 `obs_pack` 的 `Passable` 通道那条注释：地图长什么样
    // 是免费信息），而把「从未探索」压黑是另一档设计（当前不做：城区半径 12–15 而
    // 建筑视野零散，压黑会让自家城内出现环状暗带）。
    //
    // **单位不进记忆图**（`FogLayer` 只记建筑），所以离开视野的敌人是**消失**而不是
    // 留一个残影——这正是三态设计的意思，不要在这里写 `!= Unseen` 把它压回两态。
    const rts::FogLayer& fog = view.fog();
    const rts::Side me = view.side();
    const auto visible_at = [&fog](rts::GridPos g) {
        return fog.in_bounds(g.i, g.j) && fog.at(g.i, g.j) == rts::Vis::Visible;
    };

    // 地形叠加物（岩 / 林 / 桥）。**不含地图里的墙与障碍**——那两类活在仿真里，
    // 会被拆掉，从 `MapData` 画等于画一份不死的幽灵墙。
    for (int y = 0; y < map.height(); ++y) {
        for (int x = 0; x < map.width(); ++x) {
            const Terrain t = map.terrain_at(x, y);
            const TerrainSprites ts = SceneModel::expand(t);
            if (ts.overlay.empty()) continue;
            DrawItem it;
            it.pos = rts::GridPos{static_cast<std::int16_t>(x),
                                  static_cast<std::int16_t>(y)};
            it.sprite = ts.overlay;
            if (t == Terrain::Bridge) {
                it.facing = SceneModel::run_direction(map, it.pos,
                                                      SceneModel::RunKind::Bridge);
            }
            out.push_back(it);
        }
    }

    // 资源点的地表标记。**插在建筑循环之前**：等深时 stable_sort 保插入序，
    // 于是采集建筑盖住自己脚下那个标记（与 `SceneModel::build` 里
    // 「标记与叠加物同层」是同一条规则在对局侧的形状）。
    for (const rts::ResourceSite& r : view.resources()) {
        DrawItem it;
        it.pos = r.pos;
        it.sprite = SceneModel::resource_marker(r.kind);
        out.push_back(it);
    }

    // 建筑（从仿真读，含血条）。墙的走向读取活墙/门，工地也算邻居。
    const auto b_alive = view.bld_alive();
    const auto b_type = view.bld_type();
    const auto b_pos = view.bld_pos();
    const auto b_hp = view.bld_hp();
    const auto b_max = view.bld_max_hp();
    for (std::size_t k = 0; k < b_alive.size(); ++k) {
        if (b_alive[k] == 0) continue;
        // 建筑**全属守方**（`World::tick_vision` 那条判据的同一个事实），所以守方
        // 视角下它们恒可见。这一句因此在今天不会挡掉任何东西——**但它不是死代码**：
        // 它是「本函数尊重迷雾」与「本函数碰巧安全，因为只有一侧有建筑」的差别，
        // 而 `AI vs AI` / 双视图那一项一旦拿攻方视角调用本函数，少了它就是一次
        // 泄漏。攻方视角下记忆格里的建筑当前**不重建绘制**（`remembered_bld` 有数据，
        // 但守方视角用不上，今天写了就是死代码），那是已知缺口、不是这一句的问题。
        if (me != rts::Side::Defender && !visible_at(b_pos[k])) continue;
        DrawItem it;
        it.pos = b_pos[k];
        it.sprite = rts::ident_of(b_type[k]);
        if (b_type[k] == rts::BldType::Wall || b_type[k] == rts::BldType::Gate ||
            b_type[k] == rts::BldType::Fence) {
            it.facing = SceneModel::run_direction(view, it.pos, {}, b_type[k]);
        }
        if(b_type[k]==rts::BldType::Tower || b_type[k]==rts::BldType::Flak) {
            const auto& stats=view.stats().of(b_type[k]);
            const int left=view.bld_windup()[k],cd=view.bld_cooldown()[k];
            const int elapsed=stats.cooldown_ticks-cd;
            it.facing=facing_from_delta(view.bld_aim()[k].x-rts::center_of(it.pos).x,view.bld_aim()[k].y-rts::center_of(it.pos).y);
            if(left>0 || (cd>0 && elapsed<stats.windup_ticks+8)) {
                // 箭楼旧攻击图烘焙了一支位置/方向不符的箭；用稳定楼体与出口闪光。
                // 弩楼使用现有四帧机械攻击动画。
                it.state=b_type[k]==rts::BldType::Flak?"attack":"idle";
                it.aim=view.bld_aim()[k];
                it.attack_progress=left>0?1.0f-static_cast<float>(left)/static_cast<float>(std::max(1,stats.windup_ticks))
                    :1.0f+static_cast<float>(std::max(0,elapsed-stats.windup_ticks))/8.0f;
            }
        }
        it.hp_frac = hp_frac_of(b_hp[k], b_max[k]);
        it.level = view.bld_level()[k];
        out.push_back(it);
        // 拐角格补竖板（同 `SceneModel::build` 那条纪律）：城圈四角横竖两条边
        // 相交，`run_direction` 只给横板（NE），竖边缺一格、角在画面上是开的。
        // 拐角同样读取活墙，拆除相邻墙后不再留下补板。补板不重复画血条。
        if ((b_type[k] == rts::BldType::Wall || b_type[k] == rts::BldType::Fence) &&
            SceneModel::is_wall_corner(view, it.pos, {}, b_type[k])) {
            DrawItem corner = it;
            corner.facing = Facing::SE;
            corner.hp_frac = -1.0f;
            out.push_back(corner);
        }
    }

    // 中立障碍。
    const auto o_alive = view.obstacle_alive();
    const auto o_type = view.obstacle_type();
    const auto o_pos = view.obstacle_pos();
    const auto o_hp = view.obstacle_hp();
    const auto o_max = view.obstacle_max_hp();
    for (std::size_t k = 0; k < o_alive.size(); ++k) {
        if (o_alive[k] == 0) continue;
        DrawItem it;
        it.pos = o_pos[k];
        it.sprite = rts::ident_of(o_type[k]);
        it.hp_frac = hp_frac_of(o_hp[k], o_max[k]);
        out.push_back(it);
    }

    // 单位：连续坐标、按动作挑状态与朝向。
    const auto u_alive = view.unit_alive();
    const auto u_type = view.unit_type();
    const auto u_pos = view.unit_pos();
    const auto u_hp = view.unit_hp();
    const auto u_max = view.unit_max_hp();
    const auto u_action = view.unit_action();
    const auto u_windup = view.unit_windup();
    const auto u_tgt = view.unit_target_kind();
    const auto u_aim = view.unit_aim();
    const auto u_garrison = view.unit_garrison();
    const auto u_mount = view.unit_mount();
    for (std::size_t k = 0; k < u_alive.size(); ++k) {
        if (u_alive[k] == 0) continue;
        // 己方恒画、敌方要当前可见。**己方不过迷雾**是刻意的（同 `obs_pack`）：
        // 自己的兵在哪不需要侦查，而按迷雾过滤己方会在视野边缘把自己的单位抹掉。
        if (rts::side_of(u_type[k]) != me && !visible_at(rts::grid_of(u_pos[k]))) {
            continue;
        }
        DrawItem it;
        it.continuous = true;
        it.world = u_pos[k];
        it.pos = rts::grid_of(u_pos[k]);
        it.sprite = rts::ident_of(u_type[k]);
        // 驻守登顶：画成站在墙头（仿真里位置就是墙格中心；在爬的还在地面，不抬）。
        // **抬多少不在这里定**——只说踩着哪座建筑，像素归渲染侧（见 `stand_on`）。
        if (u_garrison[k] != rts::kNoSlot && u_mount[k] == 0) {
            it.stand_on = garrison_stand_sprite(view, u_garrison[k]);
            it.stand_facing=SceneModel::run_direction(view,rts::pos_of_slot(u_garrison[k],view.width()));
        }
        const bool winding = u_windup[k] > 0 && u_tgt[k] != rts::TgtKind::None;
        it.facing = facing_of(u_action[k], u_pos[k], u_aim[k], winding);
        it.state = winding ? "attack" : (rts::is_move(u_action[k]) ? "move" : "idle");
        it.anim = static_cast<int>(now / 4);   // 5 帧/秒的相位；帧数由渲染侧取模
        it.hp_frac = hp_frac_of(u_hp[k], u_max[k]);
        out.push_back(it);
    }

    // 在途弹丸（第四组实体，机制第五批）。单张 FREE 图，方向由渲染侧按
    // 飞行角旋转；归属按 tools/sprite_gen/README.md §8.2 / §8.4 的表：
    //
    //   `Flak` → 弩矢 `Bolt`    `Tower` → 箭 `Arrow`
    //   守方单位 → 箭 `Arrow`（`Archer`）   攻方单位 → 魔法弹 `Magic`（`Shade`）
    //
    // **按阵营分单位那两档不是巧合，是 `launches_projectile()` 的性质**
    // （Ranged × 非空中）：它在每一侧恰好命中一个兵种（守 `Archer` / 攻 `Shade`），
    // 所以「单位射的」这一档按阵营一分就够，不需要知道具体兵种。
    //
    // **这条路刻意不新增 `p_src_unit_`**：`p_side_` 本来就在 `World` 里，
    // 只是没暴露；加数组要动布局、动 `state_hash` 的喂入清单、动
    // `kWorldHashTag`（旧回放重录），而加一个只读访问器这三样一个都不动。
    //
    // **但它是花名册的性质、不是结构不变量。** 某一侧哪天多出第二个远程地面
    // 兵种，按侧挑就会让两者共用同一张图——而那种错**画面照样出、只是画错了**。
    // 因此由 `tests/scene_model_test.cpp` 的「弹丸精灵按阵营分档的前提」钉住
    // （**不能用 `static_assert`**：`launches_projectile()` 是虚函数、
    // `behavior_of()` 不是 constexpr，编译期到不了——这正是 `CLAUDE.md` 说的
    // 「虚函数丢掉的那条完备性保证，由测试互相印证补回来」）。
    const auto p_pos = view.proj_pos();
    const auto p_aim = view.proj_aim();
    const auto p_src = view.proj_src_bld();
    const auto p_side = view.proj_side();
    for (std::size_t k = 0; k < p_pos.size(); ++k) {
        // 同单位那条。**弹丸按它自己当前所在的格判**，不按射手也不按落点：
        // 一支从迷雾里飞进视野的箭，进了视野就该看得见（否则它会在半空凭空出现，
        // 反而更怪），而飞出视野就该消失。
        if (p_side[k] != me && !visible_at(rts::grid_of(p_pos[k]))) continue;
        DrawItem it;
        it.continuous = true;
        it.world = p_pos[k];
        it.pos = rts::grid_of(p_pos[k]);
        if (p_src[k] != rts::kProjFromUnit) {
            it.sprite = (p_src[k] == static_cast<std::uint8_t>(rts::BldType::Flak))
                            ? "Bolt" : "Arrow";
        } else {
            it.sprite = (p_side[k] == rts::Side::Attacker) ? "Magic" : "Arrow";
        }
        it.aim = p_aim[k];
        it.lift = 0.4f;
        if(p_src[k]!=rts::kProjFromUnit) {
            it.projectile_source=p_src[k]==static_cast<std::uint8_t>(rts::BldType::Flak)?"Flak":"Tower";
            const auto origin=view.proj_origin()[k];
            const float dx=it.aim.x-origin.x,dy=it.aim.y-origin.y,d2=dx*dx+dy*dy;
            it.facing=facing_from_delta(dx,dy);
            it.flight_progress=d2>0?std::clamp(((it.world.x-origin.x)*dx+(it.world.y-origin.y)*dy)/d2,0.0f,1.0f):1.0f;
            if(it.projectile_source=="Flak") it.lift=1.2f;
        }
        out.push_back(it);
    }

    // 稳定排序：深度并列时保持上面的插入顺序（叠加物 → 建筑 → 障碍 → 单位
    // → 弹丸），顺序是确定的，画面不会逐帧跳变。
    std::stable_sort(out.begin(), out.end(), [](const DrawItem& a, const DrawItem& b) {
        return a.depth_f() < b.depth_f();
    });
    return out;
}

}  // namespace game
