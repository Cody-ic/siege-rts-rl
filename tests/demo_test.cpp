// 演示对局：`game::DemoBattle` + `game::BattleScene`。
//
// 这两个类是 demo 的**非渲染**那一半，所以它们的测试在默认构建里跑（GCC 侧也验）。
// 三件事值得钉：demo 是确定性的（两次构造推进同样的 tick 数 ⇒ 同一个哈希）、
// demo 里真的发生了攻防（不是两队人马站着对视）、场景装配把活的实体排进了
// 同一个深度序列。

#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/battle_scene.hpp"
#include "game/demo_driver.hpp"
#include "game/map_loader.hpp"
#include "game/player_input.hpp"
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

// 玩家真的能把经济与补员跑起来吗？
//
// 这条是从一次试玩里长出来的：交互层此前只让玩家造**墙 / 门 / 木栅 / 箭楼 /
// 弩楼**五种，而三种资源的**流量只来自采集建筑**（`Keep` 那点金币是地板、
// 不是收入）。于是石与木永远停在开局那个数，金币每 5 秒 +2——一名弓手 60 金，
// 要等 150 秒。玩家的原话是「只能新建造建筑，不能招兵，兵被打死了就没了」。
//
// **所以这条测的不是某个函数，而是那条链路通不通**：建采集建筑 → 收入涨 →
// 买得起兵 → 兵真的出来、且进了指定编队。占位数值下它成立；数值一改，
// 这条会红——那正是该有人回来看一眼的时刻。
TEST_CASE("经济与补员的闭环：建金矿 → 攒够金 → 征兵 → 新兵进编队", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle d(map, stats, 7);
    const int w = map.width();

    // 演示地图城内有一个金矿点 (2,6)。`Mine` 只能盖在金矿点上（结构规则）。
    const rts::Command build =
        game::build_command(rts::BldType::Mine, rts::GridPos{2, 6}, w);
    REQUIRE(game::can_place_hint(d.world().view(rts::Side::Defender), rts::BldType::Mine,
                                 rts::GridPos{2, 6}));
    d.submit_defender(&build, 1);
    d.update(1);
    REQUIRE(d.world().live_bld_count() > 0);

    // 工地要工匠盖（开局那名 3 号编队的工匠），所以先把它叫过去，再等它盖完。
    const rts::Command send = game::command_for_click(
        d.world().view(rts::Side::Defender), /*force=*/3, rts::GridPos{3, 6});
    REQUIRE(send.kind == rts::CommandKind::MoveForce);
    d.submit_defender(&send, 1);

    const std::int64_t gold_before = d.world().stock(rts::Resource::Gold);
    d.update(1500);
    const std::int64_t gold_after = d.world().stock(rts::Resource::Gold);

    // 金矿盖起来之后，收入曲线要明显快过「只有堡垒那条地板」。
    // 占位表：Keep 每 100 tick +2，Mine 每 100 tick +10 ⇒ 1500 tick 至少多 100。
    REQUIRE(gold_after - gold_before > 100);

    // 征兵：堡垒也能出兵（兵营可退化为从堡垒出兵，机制侧同一条）。
    const rts::GridPos keep = map.keep();
    REQUIRE(game::can_train_hint(d.world().view(rts::Side::Defender), keep));

    // **认「新面孔」，不数总数**：这一千多 tick 里波次一直在打，0 号编队的
    // 弓手本来就可能在训练完成前后死掉——`before + 1` 这种纯计数断言会被
    // 同一时间窗口内的战损抵消掉（死一个、补一个，总数不变，但训练那条
    // 链路其实是通的）。真正要证明的是「训练确实产出了一个原来不存在的
    // 单位，且它进了指定编队」，与其余弓手的战损无关——按 `UnitId` 认，
    // 一个在 `before` 里没见过的 id 出现在 0 号编队里，就是训练生效的
    // 直接证据。
    const auto force0_archer_ids = [](const rts::World& world) {
        std::vector<rts::UnitId> ids;
        world.enumerate_units(rts::Side::Defender, ids);
        const rts::WorldView v = world.view(rts::Side::Defender);
        std::vector<rts::UnitId> out;
        for (const rts::UnitId id : ids) {
            const std::size_t k = id.index();
            if (v.unit_type()[k] == rts::UnitType::Archer && v.unit_force()[k] == 0) {
                out.push_back(id);
            }
        }
        return out;
    };
    const std::vector<rts::UnitId> before_ids = force0_archer_ids(d.world());

    const rts::Command train =
        game::train_command(rts::UnitType::Archer, /*force=*/0, keep, w);
    d.submit_defender(&train, 1);
    d.update(1);
    // 钱扣了 ⇒ 命令没有被静默拒绝（买不起时解算是 break，什么都不说）。
    REQUIRE(d.world().stock(rts::Resource::Gold) < gold_after);
    // 兵营占用了 ⇒ 这一格立刻不能再点（一次一名）。
    REQUIRE_FALSE(game::can_train_hint(d.world().view(rts::Side::Defender), keep));

    // 训练要时间（占位 100 tick）。等它出来。
    //
    // 新兵进的是**指定的编队**——命令枚举里没有「把单位编入编队」（#57 定的
    // 12 种不变），所以「往打薄的那支里补兵」只有征兵这一条路，
    // 而这条断言就是那句话的可执行形式。
    d.update(200);
    const std::vector<rts::UnitId> after_ids = force0_archer_ids(d.world());
    const bool found_new = std::any_of(
        after_ids.begin(), after_ids.end(), [&](rts::UnitId id) {
            return std::find(before_ids.begin(), before_ids.end(), id) ==
                  before_ids.end();
        });
    REQUIRE(found_new);
}
