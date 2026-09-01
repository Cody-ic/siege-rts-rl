#include "game/battle_scene.hpp"

#include <algorithm>
#include <cstdint>

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

    // 建筑（从仿真读，含血条）。墙的走向本该看相邻墙段（4.2.1.1），
    // 这里先按地图初始墙况推——拆墙不改剩余墙段的走向读法，缺口两侧
    // 仍读作「同一条墙断了」，这正是想要的画面。
    const auto b_alive = view.bld_alive();
    const auto b_type = view.bld_type();
    const auto b_pos = view.bld_pos();
    const auto b_hp = view.bld_hp();
    const auto b_max = view.bld_max_hp();
    for (std::size_t k = 0; k < b_alive.size(); ++k) {
        if (b_alive[k] == 0) continue;
        DrawItem it;
        it.pos = b_pos[k];
        it.sprite = rts::ident_of(b_type[k]);
        if (b_type[k] == rts::BldType::Wall || b_type[k] == rts::BldType::Gate) {
            it.facing = SceneModel::run_direction(map, it.pos, SceneModel::RunKind::Wall);
        }
        it.hp_frac = hp_frac_of(b_hp[k], b_max[k]);
        out.push_back(it);
        // 拐角格补竖板（同 `SceneModel::build` 那条纪律）：城圈四角横竖两条边
        // 相交，`run_direction` 只给横板（SW），竖边缺一格、角在画面上是开的。
        // 走向看**地图初始墙况**（同上面 run_direction 的那份注释），不是活墙——
        // 拆墙不改剩余墙段的走向读法。补板不带血条（血条画在主板上，否则两板
        // 重叠画两条）。
        if (b_type[k] == rts::BldType::Wall &&
            SceneModel::is_wall_corner(map, it.pos)) {
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
        DrawItem it;
        it.continuous = true;
        it.world = u_pos[k];
        it.pos = rts::grid_of(u_pos[k]);
        it.sprite = rts::ident_of(u_type[k]);
        // 驻守登顶：画成站在墙头（仿真里位置就是墙格中心；在爬的还在地面，不抬）。
        // **抬多少不在这里定**——只说踩着哪座建筑，像素归渲染侧（见 `stand_on`）。
        if (u_garrison[k] != rts::kNoSlot && u_mount[k] == 0) {
            it.stand_on = garrison_stand_sprite(view, u_garrison[k]);
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
        it.lift = 0.4f;   // 飞行高度的视觉占位——箭不贴地滑
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
