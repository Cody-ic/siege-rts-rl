// 机制第六批：flow field 寻路（1c 收尾）。
//
// 这批锁三类东西，每条对着设计原文写：
//
//   * **「墙段按高代价可通行」真的在比较**（CLAUDE.md 5.1）：同一张图、
//     同一兵种，等级档不同就给出不同路线——档数存在的全部理由就是这条，
//     数值取「几步能手算」的占位，无平衡含义
//   * **通行规则与移动机制同真值**：斜向穿角（对实体同样成立）、
//     城门对守方敞开对攻方是墙——field 指的路移动必须走得通，
//     两处漂移的症状是「单位对着一个它过不去的格子反复撞」
//   * **确定性**：平局的归属规范（先到者 = 弹出序 = (代价, 格下标) 全序），
//     以及穿门窗口的回放逐 tick 一致
//
// field 是派生数据（不进哈希、不进回放），所以这里没有哈希探针——
// 它的确定性由「同输入同输出」直接断言。

#include <cstddef>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rts/action.hpp"
#include "rts/flow.hpp"
#include "rts/replay.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"
#include "rts/world_view.hpp"

namespace {

rts::UnitStats& us(rts::StatsTable& t, rts::UnitType u) {
    return t.unit[static_cast<std::size_t>(u)];
}

// 数值只求可手算：Ghoul 4 tick/格、一发 10 tick（前摇 2 + 冷却 8）、
// 对结构全额；等级系数 5000 让档间差一眼可见（4 级 = 4 倍伤害）。
//
// **为什么是 5000 而不是 4 倍所需的 1000**：§1.4 之后等级缩放是
// `√(1 + k(L−1))`（`combat_math.hpp`），系数是**开方前**的。要 4 级拿到
// 4 倍伤害就得 `√(1 + 5000‰ × 3) = √16 = 4`，即 k = 5000。这条注释原来写
// 「1000 让 4 级 = 4 倍」，那是线性时代的算法——改成开方之后 1000 只给 2 倍，
// 于是「高档就近破墙」不再成立（破墙 20 tick、直穿 52 > 绕缺口 49.9，
// field 改选绕路），下面那条 42.0 的断言当场变红。**想要 N 倍，系数给 N² 量级。**
//
// 两个系数取同一个值不是巧合：`p − q = 0` 是结构约束（`StatsLoader` 对真表
// 强制相等）。这里是测试自己构造的表、不经载入器，但没有理由在测试里造一张
// 结构上非法的表。
rts::StatsTable flow_stats() {
    rts::StatsTable t;
    for (rts::UnitStats& u : t.unit) {
        u.max_hp = 30;
        u.vision = 4.0f;
        u.windup_ticks = 2;
        u.cooldown_ticks = 8;
    }
    us(t, rts::UnitType::Ghoul) = {30, 10, 1.2f, 0.25f, 4.0f, 2, 8, 1000, 0.0f, 0.0f};
    us(t, rts::UnitType::Ranger) = {18, 6, 1.2f, 0.50f, 5.0f, 2, 8, 1000, 0.0f, 0.0f};
    us(t, rts::UnitType::Wraith) = {14, 0, 0.0f, 0.50f, 6.0f, 0, 1, 0, 0.0f, 0.0f};
    // `Shade` 在测试表里伤害取 0：2026-09-03 起 `Wraith` 会飞，地面「破不了
    // 结构的兵种」这个测试载具由它接任（`can_break_structure()` 为真但
    // dmg = 0 ⇒ 进格代价照样是 kInf，与 flow.cpp 同一判据）。
    us(t, rts::UnitType::Shade) = {14, 0, 0.0f, 0.50f, 6.0f, 0, 1, 0, 0.0f, 0.0f};
    t.bld[static_cast<std::size_t>(rts::BldType::Keep)].max_hp = 200;
    t.bld[static_cast<std::size_t>(rts::BldType::Wall)].max_hp = 40;
    t.bld[static_cast<std::size_t>(rts::BldType::Gate)].max_hp = 20;
    t.global.hp_permille_per_level = 5000;
    t.global.dmg_permille_per_level = 5000;
    return t;
}

// 12×5 平地 + 角落一座 Keep（World 要求恰好一座）。
rts::WorldInit farena() {
    rts::WorldInit init;
    init.width = 12;
    init.height = 5;
    init.terrain.assign(60, rts::Terrain::Plain);
    init.keep = rts::GridPos{0, 0};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 200, 200});
    init.seed = 7;
    init.map_id = "flow-arena";
    init.nominal_level = 1;
    init.stats = flow_stats();
    return init;
}

}  // namespace

// ——纯函数：档的映射——

TEST_CASE("等级三档的映射与代表等级", "[flow]") {
    static_assert(rts::kFlowTierCount == 3, "档数是形状（#57 组内定夺），改它先回契约");
    const rts::FlowTiering t{};   // 占位默认：4 / 8
    REQUIRE(rts::flow_tier_of(1, t) == 0);
    REQUIRE(rts::flow_tier_of(3, t) == 0);
    REQUIRE(rts::flow_tier_of(4, t) == 1);
    REQUIRE(rts::flow_tier_of(7, t) == 1);
    REQUIRE(rts::flow_tier_of(8, t) == 2);
    REQUIRE(rts::flow_tier_of(100, t) == 2);   // 顶档开区间：等级无上限
    // 代表等级 = 档下界（保守端：只会多绕，绝不站杀伤区啃啃不穿的墙）。
    REQUIRE(rts::flow_rep_level(0, t) == 1);
    REQUIRE(rts::flow_rep_level(1, t) == 4);
    REQUIRE(rts::flow_rep_level(2, t) == 8);
    const rts::FlowTiering c{3, 10};
    REQUIRE(rts::flow_tier_of(3, c) == 1);
    REQUIRE(rts::flow_tier_of(9, c) == 1);
    REQUIRE(rts::flow_tier_of(10, c) == 2);
}

// ——空地：代价与方向可手算——

TEST_CASE("空地直线：代价 = 距离 ÷ 速度，方向朝目标", "[flow]") {
    rts::World w(farena());
    const rts::GridPos goal[1] = {rts::GridPos{10, 2}};
    const rts::FlowField f = rts::FlowField::compute(
        w.view(rts::Side::Attacker), rts::UnitType::Ghoul, 0, goal);

    // 正交 5 格 × 4 tick/格；MoveSE 是格东（+1, 0）——按名字猜方位会猜反，
    // 以 move_delta 为准（rts/action.hpp 文件头那张表）。
    REQUIRE(f.cost_at(rts::GridPos{5, 2}) == 20.0f);
    REQUIRE(f.step_of(rts::GridPos{5, 2}) == rts::UnitAction::MoveSE);
    // 斜向一步 = √2 ÷ 速度（与移动机制同一个 kSqrt2）。
    REQUIRE(f.cost_at(rts::GridPos{9, 1}) == 1.41421356f / 0.25f);
    // 目标格：代价 0、原地不动。
    REQUIRE(f.cost_at(rts::GridPos{10, 2}) == 0.0f);
    REQUIRE(f.step_of(rts::GridPos{10, 2}) == rts::UnitAction::Stop);
    // 越界查询给安全值，不给未定义行为。
    REQUIRE_FALSE(f.reachable(rts::GridPos{-1, 0}));
    REQUIRE(f.step_of(rts::GridPos{12, 0}) == rts::UnitAction::Stop);
}

// ——档数存在的理由：同图同兵种，档不同则路线不同——

TEST_CASE("低档绕缺口，高档就近破墙：破坏代价随档而变", "[flow]") {
    rts::World w(farena());
    // 墙线 i = 6、j = 1..4，缺口在 (6, 0)。
    for (int j = 1; j < 5; ++j) {
        w.place_bld(rts::BldType::Wall,
                    rts::GridPos{6, static_cast<std::int16_t>(j)}, 40, 40);
    }
    const rts::GridPos goal[1] = {rts::GridPos{10, 4}};
    const rts::GridPos probe{2, 4};
    const rts::FlowField f0 = rts::FlowField::compute(
        w.view(rts::Side::Attacker), rts::UnitType::Ghoul, 0, goal);
    const rts::FlowField f1 = rts::FlowField::compute(
        w.view(rts::Side::Attacker), rts::UnitType::Ghoul, 1, goal);

    // 高档（代表等级 4，一发 40 血）：破墙 1 发 = 10 tick，直穿 8 格 × 4 + 10 = 42。
    REQUIRE(f1.cost_at(probe) == 42.0f);
    REQUIRE(f1.step_of(probe) == rts::UnitAction::MoveSE);   // 朝墙直走
    // 低档（代表等级 1，一发 10 血）：破墙 4 发 = 40 tick，直穿 72 > 绕缺口 ≈ 49.9。
    REQUIRE(f0.step_of(probe) == rts::UnitAction::MoveE);    // 斜向缺口
    REQUIRE(f0.cost_at(probe) > 45.0f);
    REQUIRE(f0.cost_at(probe) < 52.0f);
    // 档越高路越便宜——破坏速率单调于等级，field 必须保序。
    REQUIRE(f0.cost_at(probe) > f1.cost_at(probe));
}

TEST_CASE("破不了结构的兵种：缺口是唯一的路，围死即不可达", "[flow]") {
    const rts::GridPos goal[1] = {rts::GridPos{10, 4}};
    const rts::GridPos probe{2, 4};

    SECTION("有缺口：Shade 从缺口绕") {
        rts::World w(farena());
        for (int j = 1; j < 5; ++j) {
            w.place_bld(rts::BldType::Wall,
                        rts::GridPos{6, static_cast<std::int16_t>(j)}, 40, 40);
        }
        const rts::FlowField f = rts::FlowField::compute(
            w.view(rts::Side::Attacker), rts::UnitType::Shade, 0, goal);
        REQUIRE(f.reachable(probe));
        REQUIRE(f.step_of(probe) == rts::UnitAction::MoveE);   // 只有绕这一条
    }
    SECTION("封死缺口：不可达，而不是给一条穿墙的假路") {
        rts::World w(farena());
        for (int j = 0; j < 5; ++j) {
            w.place_bld(rts::BldType::Wall,
                        rts::GridPos{6, static_cast<std::int16_t>(j)}, 40, 40);
        }
        const rts::FlowField f = rts::FlowField::compute(
            w.view(rts::Side::Attacker), rts::UnitType::Shade, 0, goal);
        REQUIRE_FALSE(f.reachable(probe));
        REQUIRE(f.step_of(probe) == rts::UnitAction::Stop);
    }
    SECTION("Wraith 会飞：同一条封死的墙线照穿（2026-09-03 起）") {
        // 上面两节是地面判据，这一节钉空军对照：墙线原样封死，`Wraith`
        // 既可达、且代价就是直线行军（8 格 × 2 tick），墙在它的 field 里
        // 不存在（`flow.cpp`：空军飞过一切，地形与实体都不挡）。
        rts::World w(farena());
        for (int j = 0; j < 5; ++j) {
            w.place_bld(rts::BldType::Wall,
                        rts::GridPos{6, static_cast<std::int16_t>(j)}, 40, 40);
        }
        const rts::FlowField f = rts::FlowField::compute(
            w.view(rts::Side::Attacker), rts::UnitType::Wraith, 0, goal);
        REQUIRE(f.reachable(probe));
        REQUIRE(f.cost_at(probe) == 16.0f);
        REQUIRE(f.step_of(probe) == rts::UnitAction::MoveSE);   // 格东直行
    }
}

// ——斜向穿角对实体成立 + 平局归属规范——

TEST_CASE("实体也挡斜角，等价两路的平局归格下标小的那条", "[flow]") {
    rts::World w(farena());
    w.place_bld(rts::BldType::Wall, rts::GridPos{6, 1}, 40, 40);
    const rts::GridPos goal[1] = {rts::GridPos{7, 1}};
    const rts::FlowField f = rts::FlowField::compute(
        w.view(rts::Side::Attacker), rts::UnitType::Shade, 0, goal);

    // (5,1) → (7,1) 被一格墙挡住：Shade 破不了（测试表里伤害为 0），斜擦墙角
    // （(5,1)→(6,0) 与 (6,0)→(7,1)）也被穿角规则拦下——上下两条正交绕路
    // 各 4 步 × 2 tick = 8.0。少了穿角规则这里会变成 2 步斜向 ≈ 5.66。
    REQUIRE(f.cost_at(rts::GridPos{5, 1}) == 8.0f);
    // 两条绕路等价：平局归先弹出的（(代价, 格下标) 全序 ⇒ j 小的那条），
    // 于是第一步是 MoveNE（格北，dj−1）而不是 MoveSW。改「严格更优才改写」
    // 或改弹出序都会翻这条——它就是「平局归属规范」的可断言形式。
    REQUIRE(f.step_of(rts::GridPos{5, 1}) == rts::UnitAction::MoveNE);
}

// ——城门：与移动机制同真值——

TEST_CASE("完工城门：守方按行军算，攻方按破门算，破不了的不可达", "[flow]") {
    // 墙线 i = 4 全高，(4, 2) 是门。World 不可拷贝，SECTION 各建各的。
    const auto wall_and_gate = [](rts::World& w, std::int32_t gate_work_left) {
        for (int j = 0; j < 5; ++j) {
            if (j == 2) continue;
            w.place_bld(rts::BldType::Wall,
                        rts::GridPos{4, static_cast<std::int16_t>(j)}, 40, 40);
        }
        w.place_bld(rts::BldType::Gate, rts::GridPos{4, 2}, 20, 20, gate_work_left);
    };
    const rts::GridPos goal[1] = {rts::GridPos{7, 2}};
    const rts::GridPos probe{2, 2};

    SECTION("同一扇门，三个兵种三种代价") {
        rts::World w(farena());
        wall_and_gate(w, 0);
        // 守方 Ranger：门是通的，5 格 × 2 tick，纯行军。
        const rts::FlowField fd = rts::FlowField::compute(
            w.view(rts::Side::Defender), rts::UnitType::Ranger, 0, goal);
        REQUIRE(fd.cost_at(probe) == 10.0f);
        REQUIRE(fd.step_of(probe) == rts::UnitAction::MoveSE);
        // 攻方 Ghoul：门是最薄的墙（20 血 2 发 = 20 tick），5 格 × 4 + 20。
        const rts::FlowField fa = rts::FlowField::compute(
            w.view(rts::Side::Attacker), rts::UnitType::Ghoul, 0, goal);
        REQUIRE(fa.cost_at(probe) == 40.0f);
        REQUIRE(fa.step_of(probe) == rts::UnitAction::MoveSE);
        // 攻方 Shade：测试表里伤害为 0 破不了门，整线不可达。
        const rts::FlowField fw = rts::FlowField::compute(
            w.view(rts::Side::Attacker), rts::UnitType::Shade, 0, goal);
        REQUIRE_FALSE(fw.reachable(probe));
    }
    SECTION("工地状态的门对守方也不通（还没有门洞）") {
        rts::World w(farena());
        wall_and_gate(w, 10);
        const rts::FlowField fd = rts::FlowField::compute(
            w.view(rts::Side::Defender), rts::UnitType::Ranger, 0, goal);
        REQUIRE_FALSE(fd.reachable(probe));
    }
}

// ——目标格一律敞开——

TEST_CASE("目标格上的建筑不挡可达性：斥候摸向 Keep 不因 Keep 不可破而全图不可达", "[flow]") {
    rts::World w(farena());
    const rts::GridPos goal[1] = {rts::GridPos{0, 0}};   // Keep 本体所在格
    const rts::FlowField f = rts::FlowField::compute(
        w.view(rts::Side::Attacker), rts::UnitType::Shade, 0, goal);
    // 进目标格不付破坏代价：它对所有路径是同一个常数，且「到得了旁边」的
    // 兵种不因目标格上的建筑而整场不可达（头文件「目标格一律视为敞开」）。
    const float diag = 1.41421356f / 0.50f;
    REQUIRE(f.reachable(rts::GridPos{3, 3}));
    REQUIRE(f.cost_at(rts::GridPos{3, 3}) == diag + diag + diag);
}

// ——确定性：同输入同输出 + 穿门窗口的回放——

TEST_CASE("同一世界两次 compute 逐格一致", "[flow]") {
    rts::World w(farena());
    for (int j = 1; j < 5; ++j) {
        w.place_bld(rts::BldType::Wall,
                    rts::GridPos{6, static_cast<std::int16_t>(j)}, 40, 40);
    }
    const rts::GridPos goal[1] = {rts::GridPos{10, 4}};
    const rts::FlowField a = rts::FlowField::compute(
        w.view(rts::Side::Attacker), rts::UnitType::Ghoul, 0, goal);
    const rts::FlowField b = rts::FlowField::compute(
        w.view(rts::Side::Attacker), rts::UnitType::Ghoul, 0, goal);
    for (int j = 0; j < a.height(); ++j) {
        for (int i = 0; i < a.width(); ++i) {
            const rts::GridPos p{static_cast<std::int16_t>(i),
                                 static_cast<std::int16_t>(j)};
            REQUIRE(a.cost_at(p) == b.cost_at(p));
            REQUIRE(a.step_of(p) == b.step_of(p));
        }
    }
}

TEST_CASE("回放跨越守方穿门的窗口逐 tick 一致", "[flow]") {
    // 穿门是移动机制的新分支（cell_open 的唯一实体例外）——把它压进
    // 回放窗口，任何顺序依赖或未初始化都会 Diverged。
    rts::WorldInit init = farena();
    init.buildings.push_back(rts::BldInit{rts::BldType::Gate, rts::GridPos{4, 2}, 20, 20});
    init.units.push_back(
        rts::UnitInit{rts::UnitType::Ranger, rts::Vec2{2.5f, 2.5f}, 1, 18, 18});
    rts::WorldInit init2 = init;

    rts::World w(std::move(init));
    rts::ReplayRecorder rec(w, 1);
    const std::vector<rts::UnitAction> d{rts::UnitAction::MoveSE};
    rec.submit_actions(rts::Side::Defender, d.data(), d.size());
    rec.advance(12);   // 0.5/tick × 12 = 6 格：从 2.5 穿过 (4,2) 走到 8.5
    const rts::Replay r = rec.finish();

    // 先确认「穿门」真的发生在窗口里——否则 Match 只是在验一段没走门的路。
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Defender, ids);
    REQUIRE(w.unit_pos(ids[0]).x > 6.0f);

    rts::World fresh(std::move(init2));
    const rts::ReplayResult res = rts::replay_verify(r, fresh);
    REQUIRE(res.verdict == rts::ReplayVerdict::Match);
}
