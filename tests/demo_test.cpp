// 演示对局：`game::DemoBattle` + `game::BattleScene`。
//
// 这两个类是 demo 的**非渲染**那一半，所以它们的测试在默认构建里跑（GCC 侧也验）。
// 三件事值得钉：demo 是确定性的（两次构造推进同样的 tick 数 ⇒ 同一个哈希）、
// demo 里真的发生了攻防（不是两队人马站着对视）、场景装配把活的实体排进了
// 同一个深度序列。

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/battle_scene.hpp"
#include "game/demo_driver.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/world_view.hpp"

namespace {

game::MapData demo_map() {
    return game::MapLoader::from_file(std::string(GAME_DATA_DIR) +
                                      "/demo_skirmish.json");
}

rts::StatsTable demo_stats() {
    return game::StatsLoader::from_file(std::string(GAME_DATA_DIR) +
                                        "/stats_placeholder.json");
}

}  // namespace

TEST_CASE("demo 对局是确定性的，且攻防真的发生了", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();

    game::DemoBattle a(map, stats, 7);
    game::DemoBattle b(map, stats, 7);
    // 波次会**新增**攻方单位，总活数不再单调——战损的判据只能看守方
    //（守方没有补员）与建筑。拿总数判过一次，波 2 生兵把它抬回去了，假绿。
    const int initial_defenders = a.world().live_unit_count(rts::Side::Defender);
    const int initial_blds = a.world().live_bld_count();

    a.update(1200);   // 一分钟仿真时间
    b.update(1200);

    // 同一张图、同一份表、同一个种子 ⇒ 同一个世界。demo 若不确定，
    // 「答辩时演的和昨天录的不一样」就会成真。
    REQUIRE(a.world().now() == 1200);
    REQUIRE(a.world().state_hash() == b.world().state_hash());

    // 攻防发生过：守方有人阵亡，或有建筑被拆（两条都不成立说明脚本或机制哑了）。
    const bool casualties =
        a.world().live_unit_count(rts::Side::Defender) < initial_defenders;
    const bool demolition = a.world().live_bld_count() < initial_blds;
    REQUIRE((casualties || demolition));
}

TEST_CASE("波次循环：建造 → 生波 → 清波 → 下一波建造", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);

    // 建造阶段先行：倒计时没走完，一个攻方单位都不该出现。
    REQUIRE(a.world().phase() == rts::WavePhase::Build);
    REQUIRE(a.world().live_unit_count(rts::Side::Attacker) == 0);

    a.update(200);   // 占位建造时长 160，越过它
    REQUIRE(a.world().phase() == rts::WavePhase::Assault);
    REQUIRE(a.world().live_unit_count(rts::Side::Attacker) > 0);

    // 打到底。两条一起构成无尽模式的形状（CLAUDE.md「评估指标」）：
    //   * 循环真的在走——守方至少清掉第一波、活着见到第二波
    //   * **最终必败**——demo 守方没有补员，攻方编成随波数涨，这不是数值
    //     碰巧，是结构（「胜率在必败的模式里没有定义，看中位存活波数」）。
    // 占位表 + 种子 7 下实测：撑到波 4、Keep 于 t≈3700 陷落。断言取
    // 「≥ 2 且必败」而不钉具体波数——数值表会动，形状不会。
    a.update(6000);
    REQUIRE(a.world().wave() >= 2);
    REQUIRE(a.defeated());
}

TEST_CASE("提前召唤：建造阶段一条 Summon，倒计时直接作废开打", "[demo]") {
    // CLAUDE.md：「必须提供『提前召唤下一波』」。生波挂在「进攻阶段的第一拍」
    // 而不是「倒计时走完」上，Summon 与倒计时两条路在那里汇合——这条测的
    // 就是汇合真的成立（只走倒计时路径的话，Summon 会召出一个空波）。
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);
    REQUIRE(a.world().phase() == rts::WavePhase::Build);

    rts::Command c;
    c.kind = rts::CommandKind::Summon;
    c.side = rts::Side::Defender;
    a.submit_defender(&c, 1);
    a.update(2);   // 第 1 tick 排空命令转阶段，第 2 tick 生波

    REQUIRE(a.world().phase() == rts::WavePhase::Assault);
    REQUIRE(a.world().live_unit_count(rts::Side::Attacker) > 0);
}

TEST_CASE("败局定格：Keep 被拆后 update 不再推进", "[demo]") {
    const game::MapData map = demo_map();
    rts::StatsTable stats = demo_stats();
    // 把攻方步兵改成拆迁队（只为尽快打出败局，无任何平衡含义）：
    // 攻得快、打得疼、拆得动、打不死。
    rts::UnitStats& g = stats.unit[static_cast<std::size_t>(rts::UnitType::Ghoul)];
    g.max_hp = 4000;
    g.damage = 5000;
    g.speed = 0.6f;
    g.vs_structure_permille = 4000;

    game::DemoBattle d(map, stats, 7);
    d.update(4000);
    REQUIRE(d.defeated());

    // 定格：败局后世界停在最后一帧，好让人看清是怎么输的。
    const rts::Tick frozen = d.world().now();
    d.update(50);
    REQUIRE(d.world().now() == frozen);
}

TEST_CASE("BattleScene 把活的实体排进同一个深度序列", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle d(map, stats, 7);
    const rts::WorldView view = d.world().view(rts::Side::Defender);

    const std::vector<game::DrawItem> sorted =
        game::BattleScene::sorted(map, view, d.world().now());

    // 序列按连续深度非递减（画家算法的前提）。
    for (std::size_t k = 1; k < sorted.size(); ++k) {
        REQUIRE(sorted[k - 1].depth_f() <= sorted[k].depth_f());
    }
    // 单位、建筑、障碍都在场：demo 开局摆了三类。
    bool has_unit = false;
    bool has_wall = false;
    bool has_obstacle = false;
    for (const game::DrawItem& it : sorted) {
        if (it.continuous) has_unit = true;
        if (it.sprite == "Wall") has_wall = true;
        if (it.sprite == "Stump") has_obstacle = true;
    }
    REQUIRE(has_unit);
    REQUIRE(has_wall);
    REQUIRE(has_obstacle);
    // 单位带血条信息、按连续坐标定位。
    for (const game::DrawItem& it : sorted) {
        if (it.continuous) {
            REQUIRE(it.hp_frac >= 0.0f);
        }
    }
}
