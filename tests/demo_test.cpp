// 演示对局：`game::DemoBattle` + `game::BattleScene`。
//
// 这两个类是 demo 的**非渲染**那一半，所以它们的测试在默认构建里跑（GCC 侧也验）。
// 三件事值得钉：demo 是确定性的（两次构造推进同样的 tick 数 ⇒ 同一个哈希）、
// demo 里真的发生了攻防（不是两队人马站着对视）、场景装配把活的实体排进了
// 同一个深度序列。

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/battle_scene.hpp"
#include "game/demo_driver.hpp"
#include "game/display_names.hpp"
#include "game/map_loader.hpp"
#include "game/player_input.hpp"
#include "game/stats_loader.hpp"
#include "rts/roster.hpp"
#include "rts/utf8_path.hpp"
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

    // 2026-08-31 血量翻倍 + 墙 +50% + 首波延后 10s 之后，1200 tick（一分钟）
    // 里可能一次伤亡/拆除都还没发生（首波 t=360 才生波，双方又都更抗打）——
    // 那会把这条测试的意图（demo 真的在打，不是两队人马对视）测成假阳性的
    // 「通过」。3000 tick（2.5 分钟）实测足够跨过第 2 波，留出真实攻防的窗口。
    a.update(3000);
    b.update(3000);

    // 同一张图、同一份表、同一个种子 ⇒ 同一个世界。demo 若不确定，
    // 「答辩时演的和昨天录的不一样」就会成真。
    REQUIRE(a.world().now() == 3000);
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

    a.update(400);   // 占位建造时长 360（首波），越过它
    REQUIRE(a.world().phase() == rts::WavePhase::Assault);
    REQUIRE(a.world().live_unit_count(rts::Side::Attacker) > 0);

    // 打到底。两条一起构成无尽模式的形状（CLAUDE.md「评估指标」）：
    //   * 循环真的在走——守方至少清掉第一波、活着见到第二波
    //   * **最终必败**——demo 守方没有补员，攻方编成随波数涨，这不是数值
    //     碰巧，是结构（「胜率在必败的模式里没有定义，看中位存活波数」）。
    // 断言取「≥ 2 且必败」而不钉具体波数与具体 tick 预算——数值表与波次
    // 节奏都会动，形状不会。2026-08-31 守方血量翻倍 + 墙 +50% + 波次间隔
    // 拉长后，实测撑到必败所需的 tick 数明显变长，预算相应放宽。
    a.update(20000);
    REQUIRE(a.world().wave() >= 2);
    REQUIRE(a.defeated());
}

TEST_CASE("波次强度由易到难：第 1 波没有 Ram/Shade", "[demo]") {
    // 2026-08-31 试玩反馈：「不同波次刷新的攻方精灵应该由易到难，不能刚开局
    // 就刷 Ram 这样的高强度精灵」——原曲线里 `Shade`（中程压制）与 `Ram`
    // （攻城，本作单件威胁最高）都从第 1 波（`wave/2`、`wave/3` 在 wave=1 时
    // 已经不是 0）就出场，玩家开局就要同时应付压制与破墙。`spawn_wave()`
    // 改成先把它们各自的出场波数往后推，这条测试把「第 1 波只有 Ghoul」钉住。
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);

    a.update(400);   // 占位建造时长 360（首波），越过它触发第 1 波生波
    REQUIRE(a.world().wave() == 1);
    REQUIRE(a.world().live_unit_count(rts::Side::Attacker) > 0);

    const rts::WorldView v = a.world().view(rts::Side::Attacker);
    int ghouls = 0, shades = 0, rams = 0, knights = 0, phoenixes = 0;
    for (std::size_t k = 0; k < v.unit_type().size(); ++k) {
        if (!v.unit_alive()[k]) continue;
        switch (v.unit_type()[k]) {
            case rts::UnitType::Ghoul: ++ghouls; break;
            case rts::UnitType::Shade: ++shades; break;
            case rts::UnitType::Ram: ++rams; break;
            case rts::UnitType::Knight: ++knights; break;
            case rts::UnitType::Phoenix: ++phoenixes; break;
            default: break;
        }
    }
    REQUIRE(ghouls > 0);
    REQUIRE(shades == 0);
    REQUIRE(rams == 0);
    REQUIRE(knights == 0);
    REQUIRE(phoenixes == 0);
}

// ——侦查系统：迷雾真的挡住了玩家看敌军的眼睛——
//
// 这是整件事的判据本身。在它之前 `BattleScene::sorted` 把 god 视角的一切原样
// 画出，于是「编成构成」与「精确进攻方向」——CLAUDE.md 情报划分里明列为
// **需要侦查**的那两项——对玩家全部免费，`Scout` 与 `Watch` 没有任何理由被造。
//
// 夹具选 `demo_skirmish`（20×12，keep 在 (4,6)、集结点在 x=18）：集结点离堡垒
// 14 格，而守方视野最远的是 `Keep` 的 8 与弓手的 7 ⇒ 刚生出来的那一波必然在
// 迷雾里。**这不是挑一张好过的图**，是挑一张判据不退化的图（同 #110 那条
// 「换成实战尺度的图之后三组破坏才各红在该红处」的教训）。
TEST_CASE("迷雾：集结点上的攻方不进绘制列表，己方与地形照进", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);

    a.update(370);   // 越过首波建造时长 360，刚生波、还没走几步
    REQUIRE(a.world().wave() == 1);
    // 前提：他们**确实存在于世界里**。这一条不能省——少了它，下面那条
    // 「画不出来」在「压根没生出来」的情况下也会通过，是个假绿。
    REQUIRE(a.world().live_unit_count(rts::Side::Attacker) > 0);

    const rts::WorldView dv = a.world().view(rts::Side::Defender);
    const std::vector<game::DrawItem> items =
        game::BattleScene::sorted(map, dv, a.world().now());

    int enemy_drawn = 0, ally_drawn = 0;
    for (const game::DrawItem& it : items) {
        if (!it.continuous) continue;                 // 只看单位/弹丸
        if (it.sprite == rts::ident_of(rts::UnitType::Ghoul)) ++enemy_drawn;
        if (it.sprite == rts::ident_of(rts::UnitType::Archer)) ++ally_drawn;
    }
    REQUIRE(enemy_drawn == 0);   // 敌军在迷雾里 ⇒ 画不出来
    REQUIRE(ally_drawn > 0);     // 己方不过迷雾 ⇒ 照画

    // 地形与建筑不受影响（迷雾只藏敌方的动态实体，见 `BattleScene::sorted`）。
    bool saw_wall = false;
    for (const game::DrawItem& it : items) {
        if (it.sprite == rts::ident_of(rts::BldType::Wall)) saw_wall = true;
    }
    REQUIRE(saw_wall);
}

// 迷雾的**反面**：走到眼皮底下的敌人必须画得出来。
//
// 只有上面那条的话，「`sorted` 永远不画敌方单位」也会通过——那是把侦查系统
// 从「白给」改成「全瞎」，同样是坏的。
TEST_CASE("迷雾：推进到守军视野内的攻方重新出现在绘制列表里", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);

    bool ever_drawn = false;
    for (int t = 0; t < 4000 && !ever_drawn; t += 20) {
        a.update(20);
        const rts::WorldView dv = a.world().view(rts::Side::Defender);
        for (const game::DrawItem& it : game::BattleScene::sorted(map, dv,
                                                                  a.world().now())) {
            if (it.continuous && it.sprite == rts::ident_of(rts::UnitType::Ghoul)) {
                ever_drawn = true;
                break;
            }
        }
    }
    REQUIRE(ever_drawn);
}

// ——侦查系统：攻方那一半——
//
// 在这条之前 `Wraith` 在 `game/` 里只出现在中文展示名表里：**攻方从来没有
// 侦查过**，于是「双向欺骗」只有守方那一向。
TEST_CASE("幽影窥使：第 1 波没有，第 2 波起恒一只", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);

    const auto count_wraiths = [&] {
        const rts::WorldView v = a.world().view(rts::Side::Attacker);
        int n = 0;
        for (std::size_t k = 0; k < v.unit_type().size(); ++k) {
            if (v.unit_alive()[k] && v.unit_type()[k] == rts::UnitType::Wraith) ++n;
        }
        return n;
    };

    a.update(370);
    REQUIRE(a.world().wave() == 1);
    REQUIRE(count_wraiths() == 0);   // 由易到难：第 1 波只有 Ghoul

    // 推进到第 2 波刚生出来的那一拍。**取「生波后尽快数」**——`Wraith` 会被
    // 守军杀掉，等久了数出来的 0 分不清「没生」与「死了」。
    bool saw_wave2 = false;
    for (int t = 0; t < 20000 && !saw_wave2; t += 10) {
        a.update(10);
        if (a.world().wave() == 2 &&
            a.world().live_unit_count(rts::Side::Attacker) > 0) {
            saw_wave2 = true;
        }
    }
    REQUIRE(saw_wave2);
    REQUIRE(count_wraiths() == 1);   // 恒 1 是结构（同 Phoenix 那条数量上限）
}

// `Wraith` 无战力，所以它**不能**照步兵那套「打得着就打、否则奔堡垒」走——
// 那会让它一路走到墙下被射死，一次侦查都不成功。这条钉的是「看到了就撤」：
// 侦查到手之后它离堡垒更远，而不是更近。
TEST_CASE("幽影窥使：侦查到手后掉头，不再往堡垒里钻", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);

    const auto wraith_dist = [&]() -> float {
        const rts::WorldView v = a.world().view(rts::Side::Attacker);
        const rts::Vec2 keep = rts::center_of(a.world().keep_pos());
        for (std::size_t k = 0; k < v.unit_type().size(); ++k) {
            if (!v.unit_alive()[k]) continue;
            if (v.unit_type()[k] != rts::UnitType::Wraith) continue;
            const float dx = v.unit_pos()[k].x - keep.x;
            const float dy = v.unit_pos()[k].y - keep.y;
            return dx * dx + dy * dy;
        }
        return -1.0f;
    };

    // 等到第 2 波的 `Wraith` 出场。
    bool found = false;
    for (int t = 0; t < 20000 && !found; t += 10) {
        a.update(10);
        found = wraith_dist() >= 0.0f;
    }
    REQUIRE(found);

    // 记下它最接近堡垒的那一刻，再往后看：它必须离开过那个最近点。
    // **不断言「一直后退」**——落单时它会改为前压（那是防波次循环卡死的分支，
    // 见 `issue_actions`），而那条分支本身是对的，不该被这条测试禁掉。
    float closest = wraith_dist();
    bool retreated = false;
    for (int t = 0; t < 2000 && !retreated; t += 10) {
        a.update(10);
        const float d = wraith_dist();
        if (d < 0.0f) break;                 // 被打死了，不算失败（见下）
        if (d < closest) closest = d;
        if (d > closest * 1.20f) retreated = true;   // 明显退开了
    }
    // 它可能在退开之前就被守军射死——那同样是设计里的一环（玩家猎杀
    // `Wraith` 让 AI 带错情报开打）。所以判据是「要么退过，要么死了」，
    // 唯独不能是「活着而且一路钻到堡垒脚下」。
    const float end = wraith_dist();
    REQUIRE((retreated || end < 0.0f));
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

    // 资源点的地表标记也在序列里（2026-09-01 试玩反馈：资源点长期不可见，
    // 「金矿不刷新」其实是「金矿不画」）。插在建筑**之前**：等深 stable_sort
    // 保插入序，采集建筑落成后盖住脚下那枚标记。demo_skirmish 的五处资源点
    // 石 [3,4]、木 [3,8]、金 [2,6]、木 [16,2]、石 [16,8]。
    const auto marker_at = [&](int x, int y, std::string_view sprite) {
        for (const game::DrawItem& it : sorted) {
            if (it.pos.i == x && it.pos.j == y && it.sprite == sprite) return true;
        }
        return false;
    };
    REQUIRE(marker_at(3, 4, "StonePt"));
    REQUIRE(marker_at(3, 8, "WoodPt"));
    REQUIRE(marker_at(2, 6, "GoldPt"));
    REQUIRE(marker_at(16, 2, "WoodPt"));
    REQUIRE(marker_at(16, 8, "StonePt"));
}

// 玩家真的能把经济与补员跑起来吗？
//
// 这条是从一次试玩里长出来的：交互层此前只让玩家造**墙 / 门 / 木栅 / 箭楼 /
// 弩楼**五种，而三种资源的**流量只来自采集建筑**（`Keep` 那点金币是地板、
// 不是收入）。于是石与木永远停在开局那个数，金币每 5 秒 +2——一名弓手 60 金，
// 要等 150 秒。玩家的原话是「只能新建造建筑，不能招兵，兵被打死了就没了」。
//
// **所以这条测的不是某个函数，而是那条链路通不通**：建采集建筑 → 收入涨 →
// 买得起兵 → 兵真的出来。占位数值下它成立；数值一改，
// 这条会红——那正是该有人回来看一眼的时刻。
TEST_CASE("经济与补员的闭环：建金矿 → 攒够金 → 征兵 → 新兵真的出现", "[demo]") {
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

    // 工地要工匠盖（开局那名工匠），所以先把它叫过去，再等它盖完。
    // 编队移除后没有「派哪支部队」，只有框选式的临时开拔指令——按类型认出
    // 工匠，给它下一道（到达即失效，随后它回归自主找活，正好接着盖）。
    std::vector<rts::UnitId> defenders;
    d.world().enumerate_units(rts::Side::Defender, defenders);
    rts::UnitId mason{};
    {
        const rts::WorldView v = d.world().view(rts::Side::Defender);
        for (const rts::UnitId id : defenders) {
            if (v.unit_type()[id.index()] == rts::UnitType::Mason) mason = id;
        }
    }
    REQUIRE(mason.valid());
    REQUIRE(game::classify_click(d.world().view(rts::Side::Defender),
                                 rts::GridPos{3, 6}) == game::ClickTarget::Ground);
    const rts::UnitId masons[1] = {mason};
    d.issue_move_order(masons, rts::GridPos{3, 6});

    const std::int64_t gold_before = d.world().stock(rts::Resource::Gold);
    d.update(1500);
    const std::int64_t gold_after = d.world().stock(rts::Resource::Gold);

    // 金矿盖起来之后，收入曲线要明显快过「只有堡垒那条地板」。
    // 占位表：Keep 每 100 tick +2，Mine 每 100 tick +10 ⇒ 1500 tick 至少多 100。
    REQUIRE(gold_after - gold_before > 100);

    // 征兵：堡垒也能出兵（兵营可退化为从堡垒出兵，机制侧同一条）。
    const rts::GridPos keep = map.keep();
    REQUIRE(game::can_train_hint(d.world().view(rts::Side::Defender), keep));

    // **认「新面孔」，不数总数**：这一千多 tick 里波次一直在打，
    // 弓手本来就可能在训练完成前后死掉——`before + 1` 这种纯计数断言会被
    // 同一时间窗口内的战损抵消掉（死一个、补一个，总数不变，但训练那条
    // 链路其实是通的）。真正要证明的是「训练确实产出了一个原来不存在的
    // 单位」，与其余弓手的战损无关——按 `UnitId` 认，
    // 一个在 `before` 里没见过的弓手 id 出现了，就是训练生效的直接证据。
    const auto archer_ids = [](const rts::World& world) {
        std::vector<rts::UnitId> ids;
        world.enumerate_units(rts::Side::Defender, ids);
        const rts::WorldView v = world.view(rts::Side::Defender);
        std::vector<rts::UnitId> out;
        for (const rts::UnitId id : ids) {
            const std::size_t k = id.index();
            if (v.unit_type()[k] == rts::UnitType::Archer) {
                out.push_back(id);
            }
        }
        return out;
    };
    const std::vector<rts::UnitId> before_ids = archer_ids(d.world());

    const rts::Command train =
        game::train_command(rts::UnitType::Archer, /*level=*/1, keep, w);
    d.submit_defender(&train, 1);
    d.update(1);
    // 钱扣了 ⇒ 命令没有被静默拒绝（买不起时解算是 break，什么都不说）。
    REQUIRE(d.world().stock(rts::Resource::Gold) < gold_after);
    // 兵营占用了 ⇒ 这一格立刻不能再点（一次一名）。
    REQUIRE_FALSE(game::can_train_hint(d.world().view(rts::Side::Defender), keep));

    // 训练要时间（占位 100 tick）。等它出来。编队移除后新兵不带任何归属，
    // 出兵即自主行动（弓手会自己找墙登）——所以这里只认「新面孔出现了」，
    // 不再有「进哪支编队」可言。
    d.update(200);
    const std::vector<rts::UnitId> after_ids = archer_ids(d.world());
    const bool found_new = std::any_of(
        after_ids.begin(), after_ids.end(), [&](rts::UnitId id) {
            return std::find(before_ids.begin(), before_ids.end(), id) ==
                  before_ids.end();
        });
    REQUIRE(found_new);
}

// 开局那三名弓手**真的站上了墙**，并且场景装配把「站在墙头」这件事说了出来。
//
// **这条测试补的是一处结构性空缺，不是某个函数的行为。** demo 的驻守目标原先写成
// 「堡垒正东三格」这样的固定偏移，而实战地图换成随机池（`city_radius` 也随机）
// 之后城墙离堡垒有远有近：指令落在空地上被 `apply_one` 直接丢掉，弓手也摆在
// 空地上。症状是**演示里从来没有人站上墙**，而当时一条测试都不会红——
// 「没人登墙」与「三个人都登上了」在原有断言下都说得通。
//
// 三段各自封一种回归：
//   1. 指令落在真墙上（沿墙的走向取三格，不是照堡垒的偏移方向）
//   2. 人真的登顶（`mount` 归零、位置落到墙格中心）
//   3. `DrawItem::stand_on` 填的是**脚下那座建筑**的标识符——渲染侧据它算抬升，
//      填错了画面上就是「人站在墙外」，而那正是这批改动要修的那个 bug
TEST_CASE("驻守：开局的三名弓手登上真实的墙，并带上 stand_on", "[demo]") {
    // **刻意不用 `demo_map()`（`demo_skirmish.json`）跑这一条。**
    // 那张夹具图 `keep=(4,6)`、墙在 `i=7`，于是被换掉的那个固定偏移
    // 「堡垒正东三格」**恰好命中真墙**——照它跑，这条测试对本次要修的 bug
    // 永远是绿的（写完先破坏性验证了一次，第一版正是这样绿着过的）。
    //
    // 换成实战尺度的参考图（`keep=(28,28)`、最近的墙段在 `j=20`）：固定偏移在这里
    // 落在空地上，于是「指令没落在墙上」这件事才会真的红。
    // **这不是挑一张更难的图，是挑一张判据不退化的图。**
    //（坐标是这张图的现况，改 `build_reference_map.py` 重生成后要顺手核对。）
    const game::MapData map = game::MapLoader::from_file(
        std::string(GAME_DATA_DIR) + "/maps/reference_border_keep_01.json");
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle d(map, stats, 7);

    // 登墙延迟从数值表来（占位 30 tick），所以推进量按它算、不写死一个 tick 数。
    // 编队移除后，登墙不再是 t=0 就下好的指令：弓手要等第一个决策拍才决定上墙，
    // 还要自己**走到**墙边（这张图城内到城墙有约 8 格）才开始爬——所以除了延迟
    // 还要留足行军余量。但仍要停在首波生波（占位 360 tick）之前：攻方出现后
    // 弓手的第一优先级是拉扯放箭，不再是找墙。
    const int mount = stats.global.garrison_mount_ticks;
    d.update(mount + 240);

    const rts::WorldView view = d.world().view(rts::Side::Defender);
    const auto u_alive = view.unit_alive();
    const auto u_type = view.unit_type();
    const auto u_pos = view.unit_pos();
    const auto u_garrison = view.unit_garrison();
    const auto u_mount = view.unit_mount();

    int on_wall = 0;
    for (std::size_t k = 0; k < u_alive.size(); ++k) {
        if (u_alive[k] == 0 || u_type[k] != rts::UnitType::Archer) continue;
        if (u_garrison[k] == rts::kNoSlot) continue;
        REQUIRE(u_mount[k] == 0);   // 延迟已走完
        // 登顶即落位墙心（`tick_garrison`）。位置对不上就说明抬升的基准点是错的。
        const rts::GridPos cell = rts::pos_of_slot(u_garrison[k], view.width());
        const rts::Vec2 center = rts::center_of(cell);
        REQUIRE(u_pos[k].x == center.x);
        REQUIRE(u_pos[k].y == center.y);
        // 那一格上真的有一段完工的墙或门（否则指令本该被丢掉，人不该在上面）。
        bool wall_here = false;
        for (std::size_t b = 0; b < view.bld_alive().size(); ++b) {
            if (view.bld_alive()[b] == 0 || view.bld_built()[b] == 0) continue;
            if (view.bld_pos()[b].i != cell.i || view.bld_pos()[b].j != cell.j) continue;
            wall_here = view.bld_type()[b] == rts::BldType::Wall ||
                        view.bld_type()[b] == rts::BldType::Gate;
        }
        REQUIRE(wall_here);
        ++on_wall;
    }
    // **三个都要上去。** 只断言 ">= 1" 的话，「三格里只有一格是墙」那个 bug
    // 照样绿——它当时的表现正是「只有中间那个人登上了」。
    REQUIRE(on_wall == 3);

    // 渲染契约那一半：登顶的单位带 `stand_on`，且它等于脚下那座建筑的标识符。
    const std::vector<game::DrawItem> sorted =
        game::BattleScene::sorted(map, view, d.world().now());
    int with_stand = 0;
    for (const game::DrawItem& it : sorted) {
        if (!it.continuous || it.stand_on.empty()) continue;
        ++with_stand;
        REQUIRE((it.stand_on == "Wall" || it.stand_on == "Gate"));
        // 与同一格上那座建筑的图对得上（demo 选中的那段含门楼，所以两种都会出现）。
        bool matched = false;
        for (const game::DrawItem& b : sorted) {
            if (b.continuous || b.sprite != it.stand_on) continue;
            if (b.pos.i == it.pos.i && b.pos.j == it.pos.j) matched = true;
        }
        REQUIRE(matched);
    }
    REQUIRE(with_stand == 3);
    // 没登墙的单位不许带 `stand_on`——带了就会被抬到半空。
    for (const game::DrawItem& it : sorted) {
        if (it.continuous && it.stand_on.empty()) continue;
        if (!it.continuous) REQUIRE(it.stand_on.empty());
    }
}

// ——免费方向提示——
//
// 迷雾一开，敌军在画面上消失；CLAUDE.md 的情报划分要求「大致方向」仍是
// **免费**的（要花钱买的是编成构成与精确分兵），否则玩家就是全盲乱找。
TEST_CASE("免费方向提示：没生波时不报，生波后指向兵力最多的集结点", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);

    // 建造阶段一个攻方单位都没有 ⇒ 不报方向（而不是报一个假的）。
    REQUIRE(game::strongest_spawn(a.world().view(rts::Side::Defender)) < 0);

    a.update(370);
    const rts::WorldView v = a.world().view(rts::Side::Defender);
    const int lead = game::strongest_spawn(v);
    REQUIRE(lead >= 0);
    REQUIRE(lead < static_cast<int>(v.spawns().size()));

    // **独立重算一遍**（不复用被测函数的写法）：把每个攻方单位算给离它最近的
    // 集结点，被测函数报的那个必须是人数最多的。
    std::vector<int> tally(v.spawns().size(), 0);
    for (std::size_t k = 0; k < v.unit_alive().size(); ++k) {
        if (!v.unit_alive()[k]) continue;
        if (rts::side_of(v.unit_type()[k]) != rts::Side::Attacker) continue;
        std::size_t best = 0;
        float best_d2 = -1.0f;
        for (std::size_t s = 0; s < v.spawns().size(); ++s) {
            const rts::Vec2 c = rts::center_of(v.spawns()[s].pos);
            const float dx = c.x - v.unit_pos()[k].x;
            const float dy = c.y - v.unit_pos()[k].y;
            const float d2 = dx * dx + dy * dy;
            if (best_d2 < 0.0f || d2 < best_d2) { best_d2 = d2; best = s; }
        }
        ++tally[best];
    }
    for (int n : tally) REQUIRE(tally[static_cast<std::size_t>(lead)] >= n);

    // 提示**不过迷雾**：那一波此刻正好全在迷雾里（上面「集结点上的攻方不进
    // 绘制列表」钉的就是这件事），而方向照样报得出来——这正是「免费」的含义。
    const std::vector<game::DrawItem> items =
        game::BattleScene::sorted(map, v, a.world().now());
    int drawn = 0;
    for (const game::DrawItem& it : items) {
        if (it.continuous && it.sprite == rts::ident_of(rts::UnitType::Ghoul)) ++drawn;
    }
    REQUIRE(drawn == 0);
}

// 方位换算：`gj` 增大是屏幕下方（南），`gi` 增大是屏幕右方（东）。
// 弄反的话方向提示会把玩家指到反方向去，而画面上一切正常。
TEST_CASE("方位换算：南北东西不许弄反，接近正方向时不报斜向", "[demo]") {
    const rts::GridPos o{10, 10};
    const auto at = [](int x, int y) {
        return rts::GridPos{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
    };
    REQUIRE(game::compass_of(o, at(10, 0)) == game::Compass::N);
    REQUIRE(game::compass_of(o, at(10, 20)) == game::Compass::S);
    REQUIRE(game::compass_of(o, at(20, 10)) == game::Compass::E);
    REQUIRE(game::compass_of(o, at(0, 10)) == game::Compass::W);
    REQUIRE(game::compass_of(o, at(20, 0)) == game::Compass::NE);
    REQUIRE(game::compass_of(o, at(0, 20)) == game::Compass::SW);
    // 几乎正北（东向分量只有北向的 1/5）不该报成东北——否则这行提示没法用。
    REQUIRE(game::compass_of(o, at(12, 0)) == game::Compass::N);
}

// 开局防御建筑必须**够得着墙线**，而不是贴着堡垒。
//
// 2026-09-01 试玩反馈「tower 的位置很奇怪」：`demo_init` 此前用固定偏移摆
// `Tower`(keep+(1,−2)) 与 `Flak`(keep+(1,2))，是单张固定地图时期的写法（同
// #110 那个「驻守目标写成堡垒正东三格」）。换成随机地图池后 `city_radius`
// 是 12–15 ⇒ 两座离墙线 10–13 格，而 `Tower.range = 7`、`Flak.range = 5`
// ⇒ **永远打不到墙线**，敌人进城五格才开火。
//
// 判据取「到最近墙段的切比雪夫距离 ≤ 自己的射程」——那正是「它够不够得着
// 墙线」这句话本身，不依赖具体地图尺寸，也不依赖任何固定偏移。
//
// **夹具必须用池图，不能用 `demo_skirmish`。** 那张手写图只有 20×12、
// 城墙离堡垒 3 格，**任何位置都在射程内** ⇒ 判据在它身上恒真。这不是挑一张
// 更难的图，是挑一张**判据不退化**的图——实测过：把摆位改回固定偏移，用
// `demo_skirmish` 跑照样全绿（假绿），换池图才红。同 #110 的教训。
//
// 池图**按目录取字典序第一张**而不是写死文件名：池图会被重新生成、名字会变
// （合 #122 那次就有一张 `gen_01007001` 改叫 `gen_01007000`）。
//
// **目录遍历必须走 `rts::path_from_utf8`。** `GAME_DATA_DIR` 是 CMake 烤进来的
// **UTF-8 绝对路径**，而本仓库的路径含中文；`std::filesystem::path(窄串)` 在
// MSVC 上按 ANSI 代码页解释 ⇒ `is_directory()` 直接为假。第一版就是这么写的，
// 后果不是报错而是**整条用例静默跳过、零断言、照样绿**——正是
// `tests/CMakeLists.txt` 那段注释点名要防的东西。所以这里也**没有**
// 「找不到就 return」那条退路：找不到就让它红。
TEST_CASE("开局的箭楼与防空都够得着墙线（实战尺度的池图）", "[demo]") {
    const std::filesystem::path pool =
        rts::path_from_utf8(std::string(GAME_DATA_DIR) + "/maps/pool");
    REQUIRE(std::filesystem::is_directory(pool));
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(pool)) {
        if (e.path().extension() == ".json") files.push_back(e.path());
    }
    REQUIRE_FALSE(files.empty());
    std::sort(files.begin(), files.end());

    const game::MapData map =
        game::MapLoader::from_file(rts::utf8_from_path(files.front()));
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);
    const rts::WorldView v = a.world().view(rts::Side::Defender);

    std::vector<rts::GridPos> walls;
    for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
        if (!v.bld_alive()[k]) continue;
        const rts::BldType t = v.bld_type()[k];
        if (t == rts::BldType::Wall || t == rts::BldType::Gate) {
            walls.push_back(v.bld_pos()[k]);
        }
    }
    // 城区够大，判据才不退化（20×12 那张手写图上它恒真，见上面那段）。
    REQUIRE(walls.size() > 40);

    int checked = 0;
    for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
        if (!v.bld_alive()[k]) continue;
        const rts::BldType t = v.bld_type()[k];
        if (t != rts::BldType::Tower && t != rts::BldType::Flak) continue;
        ++checked;
        const rts::GridPos p = v.bld_pos()[k];
        int best = 1 << 20;
        for (const rts::GridPos& w : walls) {
            const int dx = w.i - p.i;
            const int dy = w.j - p.j;
            const int d = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
            if (d < best) best = d;
        }
        const auto range = static_cast<int>(v.stats().of(t).range);
        CAPTURE(rts::ident_of(t), p.i, p.j, best, range);
        REQUIRE(best <= range);
    }
    // 防空必须在场（它只可能来自 `demo_init`——池图不预置 `Flak`）。
    // 少了这一句，「一座防御建筑都没生成」会平凡通过。
    bool has_flak = false;
    for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
        if (v.bld_alive()[k] && v.bld_type()[k] == rts::BldType::Flak) has_flak = true;
    }
    REQUIRE(has_flak);
    REQUIRE(checked >= 2);   // 池图预置 2–3 座箭楼 + demo_init 的一座防空
}

// 侦查面板的编成读数必须**与画面一致**：面板报出的，正是绘制列表里画出来的。
//
// 这是整块面板的正确性判据。若面板走了一条不过迷雾的路（例如直接数
// `unit_alive`），它就把「编成构成」从 CLAUDE.md 情报划分的**需侦查**那一列
// 偷偷挪回**免费**那一列——而画面上一切正常，没有任何别的东西会红。
TEST_CASE("侦查面板：编成读数与绘制列表逐类对齐（都过同一份迷雾）", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);

    const auto compare_at = [&](const char* when) {
        const rts::WorldView v = a.world().view(rts::Side::Defender);
        // 面板侧
        int panel[rts::kUnitTypeCount] = {};
        for (const game::SightedType& s : game::sighted_composition(v)) {
            panel[static_cast<std::size_t>(s.type)] = s.count;
            REQUIRE(s.count > 0);                                   // 不报 0
            REQUIRE(rts::side_of(s.type) == rts::Side::Attacker);   // 只报敌方
        }
        // 画面侧
        int drawn[rts::kUnitTypeCount] = {};
        for (const game::DrawItem& it : game::BattleScene::sorted(map, v,
                                                                  a.world().now())) {
            if (!it.continuous) continue;
            for (int i = 0; i < rts::kUnitTypeCount; ++i) {
                const rts::UnitType t = rts::unit_at(i);
                if (rts::side_of(t) != rts::Side::Attacker) continue;
                if (it.sprite == rts::ident_of(t)) ++drawn[static_cast<std::size_t>(t)];
            }
        }
        for (int i = 0; i < rts::kUnitTypeCount; ++i) {
            const rts::UnitType t = rts::unit_at(i);
            if (rts::side_of(t) != rts::Side::Attacker) continue;
            CAPTURE(when, rts::ident_of(t));
            REQUIRE(panel[static_cast<std::size_t>(t)] ==
                    drawn[static_cast<std::size_t>(t)]);
        }
    };

    a.update(370);   // 刚生波，全在迷雾里
    REQUIRE(a.world().live_unit_count(rts::Side::Attacker) > 0);
    REQUIRE(game::sighted_composition(a.world().view(rts::Side::Defender)).empty());
    compare_at("刚生波（全在迷雾里）");

    // 推进到真的看见了敌人，再比一次——只测「看不见」那一头会让
    // 「面板永远返回空」也通过。
    bool ever_seen = false;
    for (int t = 0; t < 4000 && !ever_seen; t += 20) {
        a.update(20);
        ever_seen = !game::sighted_composition(
                         a.world().view(rts::Side::Defender)).empty();
    }
    REQUIRE(ever_seen);
    compare_at("已经看见敌人");
}

// 兵力必须**集中**，不能四面平摊。
//
// 2026-09-02 试玩反馈：「敌人从四面八方来，每个方位就一点点人，一点压迫感都
// 没有」。根因是 `spawn_wave` 曾按 `spawns[n % spawns.size()]` **轮转**派点
// ⇒ 各集结点分到的兵力恒等。实测第 5 波总共 10 人、每路 2.5 人。
//
// 这条同时钉住另外两件本来就该成立的事：**分兵佯攻此前结构上不可能**
// （各路恒等），以及**「免费方向提示」此前由整数取余决定**——有主攻之后
// 那行 HUD 才真的指向主攻。
TEST_CASE("兵力集中：一路主攻拿大头，不是四面平摊", "[demo]") {
    const game::MapData map = demo_map();
    const rts::StatsTable stats = demo_stats();
    game::DemoBattle a(map, stats, 7);
    a.update(370);
    const rts::WorldView v = a.world().view(rts::Side::Attacker);
    const auto& spawns = v.spawns();
    REQUIRE(spawns.size() >= 2);   // 只有一个集结点时这条测不出东西

    // 每个攻方单位算给离它最近的集结点。
    std::vector<int> tally(spawns.size(), 0);
    int total = 0;
    for (std::size_t k = 0; k < v.unit_alive().size(); ++k) {
        if (!v.unit_alive()[k]) continue;
        if (rts::side_of(v.unit_type()[k]) != rts::Side::Attacker) continue;
        std::size_t best = 0;
        float best_d2 = -1.0f;
        for (std::size_t s = 0; s < spawns.size(); ++s) {
            const rts::Vec2 c = rts::center_of(spawns[s].pos);
            const float dx = c.x - v.unit_pos()[k].x;
            const float dy = c.y - v.unit_pos()[k].y;
            const float d2 = dx * dx + dy * dy;
            if (best_d2 < 0.0f || d2 < best_d2) { best_d2 = d2; best = s; }
        }
        ++tally[best];
        ++total;
    }
    REQUIRE(total > 0);

    const int top = *std::max_element(tally.begin(), tally.end());
    CAPTURE(total, top, spawns.size());
    // **判据取「主攻占过半」而不是「恰好七成」**：比例是占位旋钮，会调；
    // 「有没有一路是主攻」是结构，不会变。轮转派点下这条必然失败
    //（各路恒等 ⇒ 最大一路只有 1/n ≤ 1/2）。
    REQUIRE(top * 2 > total);

    // 主攻方向必须**随波数轮换**：否则玩家永远守同一面就能蒙混过关。
    const auto lead_of = [&](int wave_no) {
        game::DemoBattle d(map, stats, 7);
        d.update(370);
        for (int i = 1; i < wave_no; ++i) {
            for (int t = 0; t < 20000 &&
                            !(d.world().wave() == i + 1 &&
                              d.world().live_unit_count(rts::Side::Attacker) > 0);
                 t += 10) {
                d.update(10);
            }
        }
        return game::strongest_spawn(d.world().view(rts::Side::Defender));
    };
    const int w1 = lead_of(1);
    const int w2 = lead_of(2);
    CAPTURE(w1, w2);
    REQUIRE(w1 >= 0);
    REQUIRE(w2 >= 0);
    REQUIRE(w1 != w2);   // 相邻两波的主攻方向不同
}
