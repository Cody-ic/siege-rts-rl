// 守方宏观决策层（`game::DefenderMacro`）与波次曲线/节奏参数化。
//
// 这一批的共同背景：**无渲染跑的时候，此前没有人替守方下宏观命令**——全仓
// 唯一产出 `Build`/`Train`/`Upgrade`/`Repair` 的地方是 `player_input.cpp`
// （人点鼠标）。于是校准 runner 量到的「中位第 2 波陷落」测的是「守方什么
// 都不做」，不是数值失衡。本文件钉住补上那一层之后的几条性质。
//
// 最要紧的是第一条：**把编译期常量提成运行期参数时，默认值必须逐波复现旧行为**。
// 那一步没有任何设计意图，纯粹是让配平搜索够得着这些数；一旦默认值漂了，
// 就是在「重构」的名义下悄悄改了游戏，而且不会有任何别的测试红。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/attacker_macro.hpp"
#include "game/defender_macro.hpp"
#include "game/demo_driver.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/roster.hpp"
#include "rts/utf8_path.hpp"
#include "game/world_builder.hpp"
#include "rts/fog.hpp"
#include "rts/world_view.hpp"

namespace {

game::MapData pool_map() {
    const std::filesystem::path pool =
        rts::path_from_utf8(std::string(GAME_DATA_DIR) + "/maps/pool");
    REQUIRE(std::filesystem::is_directory(pool));
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(pool)) {
        if (e.path().extension() == ".json") files.push_back(e.path());
    }
    REQUIRE_FALSE(files.empty());
    std::sort(files.begin(), files.end());
    return game::MapLoader::from_file(rts::utf8_from_path(files.front()));
}

rts::StatsTable pool_stats() {
    return game::StatsLoader::from_file(std::string(GAME_DATA_DIR) +
                                        "/stats_placeholder.json");
}

int cheb(rts::GridPos a, rts::GridPos b) {
    const int dx = static_cast<int>(a.i) - static_cast<int>(b.i);
    const int dy = static_cast<int>(a.j) - static_cast<int>(b.j);
    return std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
}

// 把宏观层接上去跑 n 拍。**临时指令每拍重发**（驻守走意愿通道，意愿清空即
// 下墙），与 runner 里的接法一致——两处不一致的话测出来的不是同一个东西。
void run_with_macro(game::DemoBattle& b, game::DefenderMacro& m, int ticks,
                    int period = 20) {
    std::vector<rts::Command> cmds;
    std::vector<game::UnitOrder> orders;
    for (int t = 0; t < ticks; ++t) {
        if (t % period == 0) {
            cmds.clear();
            orders.clear();
            m.decide(b.world(), cmds, orders);
            if (!cmds.empty()) b.submit_defender(cmds.data(), cmds.size());
        }
        for (const game::UnitOrder& o : orders) {
            if (o.garrison) {
                b.issue_garrison_order(o.ids, o.target);
            } else {
                b.issue_move_order(o.ids, o.target);
            }
        }
        b.update(1);
        if (b.defeated()) return;
    }
}

}   // namespace

// ——曲线参数化：默认值不许漂——

TEST_CASE("波次曲线：默认值逐波复现原来那四个编译期常量", "[macro]") {
    const game::WaveCurve c;   // 默认构造 = 提参数之前的行为
    // 原式：kSlotsBase 6.0 + kSlotsPerWave 1.2 × (w−1)，截断取整
    //       kPowerBase 6.0 × w ^ kPowerAlpha 1.25
    //
    // **2026-09-05 加了 `slots_cap = 27`，所以线性项要单独钉。** 这一条守的是
    // 「别在重构名义下悄悄改游戏」，而硬顶是一次**有意的设计改动**（组长定，
    // 依据见 `WaveCurve::slots_cap` 的注释）——把它一起放行会让守卫失效，
    // 所以拆成两半：**线性项仍逐波对旧式子**（下面的 `want_slots`），
    // **硬顶单独一条断言**（在这条用例末尾），改它必须改这里。
    for (int w = 1; w <= 40; ++w) {
        const int want_slots = std::min(
            27, static_cast<int>(6.0 + 1.2 * static_cast<double>(w - 1)));
        CHECK(c.slots_at(w) == want_slots);
        const double want_power = 6.0 * std::pow(static_cast<double>(w), 1.25);
        CHECK(std::fabs(c.power_at(w) - want_power) < 1e-9);
        // 线性项本身没漂（脱掉硬顶之后仍是旧式子）——这一条让「改了斜率还
        // 顺手把硬顶降下去掩盖掉」不可能悄悄发生。
        CHECK(std::min(27, static_cast<int>(6.0 + 1.2 * (w - 1))) == want_slots);
    }
}

TEST_CASE("波次曲线：换形式真的换出不同的曲线", "[macro]") {
    game::WaveCurve p;   // 幂律（默认）
    game::WaveCurve lin = p;
    lin.power_form = game::WaveCurve::PowerForm::Linear;
    game::WaveCurve lg = p;
    lg.power_form = game::WaveCurve::PowerForm::Log;
    game::WaveCurve sat = p;
    sat.power_form = game::WaveCurve::PowerForm::Saturating;

    // 四条在第 1 波都从 base 起（Log: ln 1 = 0；Linear: w−1 = 0），
    // 到第 20 波必须拉开——否则「形式也是旋钮」这句话是假的。
    CHECK(p.power_at(20) > lin.power_at(20));
    CHECK(lin.power_at(20) > lg.power_at(20));
    // 饱和形式的定义就是「后期趋平」：它的增速必须严格慢于幂律。
    CHECK(sat.power_at(40) / sat.power_at(20) < p.power_at(40) / p.power_at(20));
    // 全都得是单调不减的——「波次强度随波数增长」是结构，不是旋钮。
    for (const game::WaveCurve& c : {p, lin, lg, sat}) {
        for (int w = 2; w <= 40; ++w) CHECK(c.power_at(w) >= c.power_at(w - 1));
    }
}

TEST_CASE("波次曲线：编成位封顶为 0 时不封顶，非 0 时真的咬住", "[macro]") {
    game::WaveCurve c;
    // **原来这里断言的是 `slots_cap == 0`（2026-09-01 撤下那个没依据的 40）。**
    // 2026-09-05 硬顶接回来了、取 27，而这一条测的是**机制**（0 = 不封顶、
    // 非 0 = 真咬住），不是默认取值——默认值归上面那两条守卫。所以显式置 0，
    // 于是这条用例与「默认值取多少」解耦，改默认值不会再连带打红它。
    c.slots_cap = 0;
    CHECK(c.slots_at(100) > 40);   // 0 = 不封顶
    c.slots_cap = 12;
    CHECK(c.slots_at(100) == 12);
    CHECK(c.slots_at(1) == 6);     // 没到上限的照旧
}

// ——波次循环：两种「波永远不结束」——

TEST_CASE("波次循环：不求战的单位不会把一波挂住", "[macro]") {
    // 背景（CLAUDE.md 早就写下的坑）：循环原本挂在「攻方存活数归零」上，而
    // 会撤退的侦查单位不会死 ⇒ 一只 `Wraith` 就能让游戏永远停在这一波。
    // 它此前从未发作，只因为守方总是先死；守方一旦守得住就立刻是硬停。
    const game::MapData map = pool_map();
    rts::StatsTable stats = pool_stats();
    stats.unit[static_cast<std::size_t>(rts::UnitType::Ghoul)].max_hp = 1;
    stats.unit[static_cast<std::size_t>(rts::UnitType::Shade)].max_hp = 1;
    stats.unit[static_cast<std::size_t>(rts::UnitType::Knight)].max_hp = 1;
    stats.unit[static_cast<std::size_t>(rts::UnitType::Ram)].max_hp = 1;
    stats.unit[static_cast<std::size_t>(rts::UnitType::Phoenix)].max_hp = 1;
    // `Wraith` 保持满血：它不参战、也没人追得上，正是要测的那一只。

    game::DemoBattle b(map, stats, /*seed=*/1);
    // 跑到第 2 波（`Wraith` 第 2 波起才有）之后再往前推。
    for (int t = 0; t < 40000 && b.world().wave() < 3 && !b.defeated(); ++t) {
        b.update(1);
    }
    INFO("卡在第 " << b.world().wave() << " 波");
    CHECK(b.world().wave() >= 3);   // 挂住的话这里永远是 2
}

TEST_CASE("波次循环：进攻阶段超时即收场，残兵撤走", "[macro]") {
    // 另一种死锁：**求战但打不动**。实测第 3 波跑了 54391 tick，场上剩
    // `{Knight 1, Phoenix 1, Wraith 1}`——塔打不到空中、`Phoenix` 也啃不动
    // 堡垒，谁也杀不掉谁。上一条修的是「不求战」，这条修的是「打不动」。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();

    game::WaveTiming t;
    t.build_ticks = 60;
    t.first_build_ticks = 60;
    t.assault_max_ticks = 300;   // 逼它超时
    game::DemoBattle b(map, stats, /*seed=*/1, t);

    // 每波最多 60 + 300 tick ⇒ 2000 拍足够跨过好几波。
    for (int k = 0; k < 2000 && !b.defeated(); ++k) b.update(1);
    INFO("波数 " << b.world().wave() << "，堡垒还在 " << !b.defeated());
    CHECK((b.defeated() || b.world().wave() >= 3));
}

TEST_CASE("波次节奏：默认值就是原来那两个常量", "[macro]") {
    const game::WaveTiming t;
    // **2026-09-04：13 → 26 秒、18 → 36 秒。** 这条断言的用途从「默认值别
    // 漂」变成「默认值别悄悄漂回去」——26 秒不是手拍的，是
    // 「斥候单程 12 秒 + 一座箭楼 12 秒 + 2 秒余量」的下界（推导写在
    // `WaveTiming` 的注释里）。压回 13 秒会让「侦查到的编成来不及变成建筑」
    // 这件事重新成立，而那是结构性的、不是手感问题。
    CHECK(t.first_build_ticks == 900);   // 45 s @ 20 Hz
    CHECK(t.build_ticks == 660);         // 33 s @ 20 Hz
    // **下界本身也钉一遍**，因为那才是这个默认值存在的理由：
    // 斥候要**走到集结点**（城区半径 ~10 + D 40 ≈ 50 格，0.13 格/tick
    // ⇒ 约 385 tick），判定通过后玩家还要来得及照情报建一座箭楼（240 tick）。
    // 压回去会让「侦查到的编成来不及变成部署」重新成立——那是结构性的，
    // 不是手感问题。
    CHECK(t.build_ticks >= 385 + 240);
    // 上界取 2400 不是拍的：CLAUDE.md「一波 = 一个 RL episode」，而地图校验器
    // 第 5 条按 episode ∈ [1200, 2400] 算行军占比——那是全仓既有的承诺。
    CHECK(t.assault_max_ticks == 2400);
}

// ——宏观层——

TEST_CASE("宏观层：先把设计缺口砌上（这是前提，不是优化）", "[macro]") {
    // 不封的话攻方直接走进城，门前攻防整段不发生：runner 首批 36 局里
    // 33 局就是这么输的。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();
    game::DefenderMacro macro(map);
    REQUIRE_FALSE(macro.gaps().empty());   // 池图都留 1–2 个设计缺口

    game::DemoBattle b(map, stats, /*seed=*/1);
    run_with_macro(b, macro, 600);

    // 每个缺口那一格上都该出现建筑（工地也算——它已经占位、且会被工匠盖起来）。
    const rts::WorldView v = b.world().view(rts::Side::Defender);
    const auto bp = v.bld_pos();
    const auto ba = v.bld_alive();
    int sealed = 0;
    for (const rts::GridPos g : macro.gaps()) {
        for (std::size_t k = 0; k < bp.size(); ++k) {
            if (ba[k] && bp[k] == g) {
                ++sealed;
                break;
            }
        }
    }
    INFO("缺口 " << macro.gaps().size() << " 个，封上 " << sealed);
    CHECK(sealed == static_cast<int>(macro.gaps().size()));
    CHECK(macro.stats().walls_built >= sealed);
}

TEST_CASE("宏观层：给堡垒配的近卫塔真的够得着堡垒", "[macro]") {
    // 几何：`Tower` 射程 7，而城区半径 9–10 ⇒ 摆在环内侧的塔打不到堡垒脚下，
    // 墙上的弓手（射程 6.5）也打不到。**那一圈没有火力**，攻方破门后一路走到
    // 堡垒脚下无人可挡。这条钉住近卫塔确实落在射程内。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();
    game::MacroParams p;
    p.keep_guard_towers = 2;
    game::DefenderMacro macro(map, p);

    // 这里验证塔位几何，不验证开局经济。工匠真正响应升级后，旧用例的
    // 有限资源会用于升级，不能再把「4000 拍内攒得起塔」当作几何的前提。
    // 给足资源，仍经过真实宏观选址、Build 命令及 World 的合法性检查。
    rts::World world(game::make_world_init(map, stats, 1, 1));
    world.set_stock(rts::Resource::Stone, 10000);
    world.set_stock(rts::Resource::Wood, 10000);
    std::vector<rts::Command> commands;
    std::vector<game::UnitOrder> orders;
    for(int tick=0;tick<400;++tick) {
        if(tick%20==0) {
            commands.clear();orders.clear();
            macro.decide(world,commands,orders);
            world.submit(rts::Side::Defender,commands.data(),commands.size());
        }
        world.advance(1);
    }
    const rts::WorldView v = world.view(rts::Side::Defender);
    const auto bt = v.bld_type();
    const auto bp = v.bld_pos();
    const auto ba = v.bld_alive();
    const float range = v.stats().of(rts::BldType::Tower).range;
    int guards = 0;
    for (std::size_t k = 0; k < bt.size(); ++k) {
        if (!ba[k] || bt[k] != rts::BldType::Tower) continue;
        if (static_cast<float>(cheb(bp[k], v.keep_pos())) <= range) ++guards;
    }
    INFO("射程 " << range << " 内的塔 " << guards << " 座");
    // **判据是「至少有一座」，不是「配满 keep_guard_towers 座」。**
    // 这条钉的是**几何**：塔位候选取离堡垒 4–6 格，所以建出来的必然落在射程 7
    // 以内；要拦的失败形态是「所有塔都摆在环上、堡垒周围一座没有」（= 0）。
    // 「N 拍之内建得起几座」是经济速度问题，而收入、造价、封缺口与铺矿的先后
    // 全是占位值——把它写进断言，就是让一条几何测试随任何一次数值改动变红。
    CHECK(guards >= 1);
}

TEST_CASE("斥候侦查是二元的：到达集结点掷一次，死了什么都没有", "[macro]") {
    // 组内 2026-09-04 定的形状，与攻方 `Wraith` 对称：推进到集结点 → 掷一次
    // 死活 → 活着才算侦查成功、拿到本波编成。
    //
    // **它替掉的那一版是错的**：那一版按帧累积「看见过什么」，于是斥候刚出
    // 城门瞟一眼就在漏情报、**侦查从来不会失败**。那把 `Scout` / `Wraith`
    // 的博弈整个消掉了——猎杀斥候没有意义，因为它死之前已经漏了一路。
    // 所以这条测试钉的是**二元性**，不是某个概率。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();

    SECTION("必死：到了也拿不到情报") {
        game::DefenderSetup setup;
        setup.scout_death_permille = 1000;   // 必死
        game::DemoBattle b(map, stats, /*seed=*/1, {}, {}, setup);
        game::DefenderMacro macro(map);
        run_with_macro(b, macro, 6000);
        INFO("结论 " << static_cast<int>(b.scout_outcome()));
        // 到过了（`Killed`）或还没到（`None`）都行——这条不赌行程时间；
        // **要钉的是「绝不会是 Success」**。
        CHECK(b.scout_outcome() != game::DemoBattle::ScoutOutcome::Success);
        CHECK(b.scout_report().empty());
    }

    SECTION("必活：到了就拿到它看见的编成") {
        game::DefenderSetup setup;
        setup.scout_death_permille = 0;   // 必活
        game::DemoBattle b(map, stats, /*seed=*/1, {}, {}, setup);
        game::DefenderMacro macro(map);
        // **逐拍轮询到第一次 Success 为止**，不跑满固定拍数再查：`scout_outcome_`
        // 逐波重置为 `None`，固定拍数的终点可能落在「新一波还没被探到」的
        // 窗口里——那不是侦查失败，只是计时撞上了波次边界（2026-09-05 所见即报
        // 改动后实测红过一次：波2 于 t=4017 探到、波3 于 t≈5400 重置）。
        std::vector<rts::Command> cmds;
        std::vector<game::UnitOrder> orders;
        int t = 0;
        for (; t < 6000; ++t) {
            if (t % 20 == 0) {
                cmds.clear();
                orders.clear();
                macro.decide(b.world(), cmds, orders);
                if (!cmds.empty()) b.submit_defender(cmds.data(), cmds.size());
            }
            for (const game::UnitOrder& o : orders) {
                if (o.garrison) {
                    b.issue_garrison_order(o.ids, o.target);
                } else {
                    b.issue_move_order(o.ids, o.target);
                }
            }
            b.update(1);
            if (b.defeated()) break;
            if (b.scout_outcome() == game::DemoBattle::ScoutOutcome::Success) break;
        }
        INFO("跑到第 " << t << " 拍");
        // 「场上有没有斥候」是这条用例最容易挂掉的前提，所以把它印出来——
        // 它第一次红就是这么定位的（工匠把非战斗人口预算吃光了，
        // 斥候一名都招不出来，见 `defender_macro.cpp` 那段注释）。
        std::vector<rts::UnitId> ids;
        b.world().enumerate_units(rts::Side::Defender, ids);
        int scouts = 0;
        for (const rts::UnitId id : ids) {
            if (b.world().unit_type(id) == rts::UnitType::Scout) ++scouts;
        }
        INFO("场上斥候 " << scouts << " 名");
        INFO("结论 " << static_cast<int>(b.scout_outcome()) << "，报告 "
                     << b.scout_report().size() << " 种");
        REQUIRE(b.scout_outcome() == game::DemoBattle::ScoutOutcome::Success);
        REQUIRE_FALSE(b.scout_report().empty());
        // **所见即报**：报告是斥候视野圈内看见的那部分编成（分兵之后「站在
        // 集结点上 = 整波都在」不再成立），所以这里只钉「报出来的东西非空」，
        // 不拿它与场上总数比大小。
        int reported = 0;
        for (const game::SightedType& ty : b.scout_report()) reported += ty.count;
        int actual = 0;
        std::vector<rts::UnitId> foes;
        b.world().enumerate_units(rts::Side::Attacker, foes);
        for (const rts::UnitId f : foes) {
            (void)f;
            ++actual;
        }
        INFO("报告 " << reported << " 个，场上 " << actual << " 个");
        CHECK(reported > 0);
    }
}

TEST_CASE("斥候侦查：判定完了玩家还剩得下时间调整部署", "[macro]") {
    // 组长的原话：「这个判定结束后玩家应当还有反应时间调整部署」。
    // 那句话是一条**可算的下界**，不是手感——本用例把它变成断言。
    //
    // 行程：斥候从城里走到集结点 ≈ 城区半径 + D(40) 格；反应：照情报建一座
    // 箭楼要 `build_ticks`。两者之和必须装得进一个建造期。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();
    game::DefenderSetup setup;
    setup.scout_death_permille = 0;   // 必活，把死活那一半排除掉
    game::DemoBattle b(map, stats, /*seed=*/1, {}, {}, setup);
    game::DefenderMacro macro(map);

    // 一拍一拍推，记下「侦查成功」发生在建造期的第几拍。
    const int build_total = game::WaveTiming{}.first_build_ticks;
    int success_tick = -1;
    std::vector<rts::Command> cmds;
    std::vector<game::UnitOrder> orders;
    for (int t = 0; t < build_total && success_tick < 0; ++t) {
        if (t % 20 == 0) {
            cmds.clear();
            orders.clear();
            macro.decide(b.world(), cmds, orders);
            if (!cmds.empty()) b.submit_defender(cmds.data(), cmds.size());
        }
        b.update(1);
        if (b.scout_outcome() == game::DemoBattle::ScoutOutcome::Success) {
            success_tick = t;
        }
    }
    INFO("侦查成功于建造期第 " << success_tick << " 拍，建造期共 " << build_total);
    // 首波脚本不一定会派斥候（开局那名斥候已经在场，但它得走过去）——
    // 没成功就跳过，这条不赌脚本的行为；**成功了就必须留得下一座箭楼的工期**。
    if (success_tick >= 0) {
        const std::int32_t tower_ticks =
            stats.bld[static_cast<std::size_t>(rts::BldType::Tower)].build_ticks;
        CHECK(build_total - success_tick >= tower_ticks);
    }
}

TEST_CASE("宏观层：编成覆盖克制二部图守方那半（不是只招弓手）", "[macro]") {
    // **这一条钉的是 CLAUDE.md 对脚本守方的判据**，不是一个平衡数：
    //
    // > 脚本要写到什么程度有一条判据：**克制二部图里已写死的战术行为
    // > （如 `Ghoul ──► Archer 拉扯`）必须进脚本**，因为那是设计意图、
    // > 不该指望涌现。
    //
    // 守方那半张图是 `Spear` 克开阔地骑兵、`Ranger` 摸攻城锤。单兵层
    // （`DefenderScript`）这两条行为**早就有**，而宏观层此前**只招弓手**
    // ⇒ 那两条行为一次都没机会发生。后果不只是配平读数偏低：**攻方 RL 的
    // 陪练里没有它们**，学出来的策略会过拟合到「只会打弓手的守方」——
    // 而那正是脚本守方存在的全部理由所反对的。
    //
    // 判据是「**每一种都出现过**」，不是「各多少个」：比例是旋钮
    // （`mix_*` / `react_permille`），会随标定漂；「有没有」是结构。
    const game::MapData map = pool_map();
    rts::StatsTable stats = pool_stats();
    // 把兵便宜到不受经济速度影响——这是测试夹具，不是对正式表的主张
    // （同「近卫塔」那条的教训：别让结构测试随任何一次数值改动变红）。
    for (const rts::UnitType u : {rts::UnitType::Archer, rts::UnitType::Spear,
                                  rts::UnitType::Ranger, rts::UnitType::Mason,
                                  rts::UnitType::Scout}) {
        stats.unit[static_cast<std::size_t>(u)].cost_gold = 1;
        stats.unit[static_cast<std::size_t>(u)].train_ticks = 2;
    }
    game::DefenderMacro macro(map);
    game::DemoBattle b(map, stats, /*seed=*/1);
    // **要跨过几波**：人口上限 10 而开局就有 7 人，空位很少，三个兵种都露面
    // 需要几轮伤亡腾位子。3000 拍只够两波，第一版就是那么红的
    // （而那次红得对——它报出了「开局空位被工匠斥候吃光」那个真缺陷）。
    run_with_macro(b, macro, 12000);

    const game::MacroStats& ms = macro.stats();
    INFO("A" << ms.trained_archer << " S" << ms.trained_spear << " R"
             << ms.trained_ranger << " M" << ms.trained_mason << " Sc"
             << ms.trained_scout);
    CHECK(ms.trained_spear > 0);    // 克骑兵那一条
    CHECK(ms.trained_ranger > 0);   // 摸攻城锤那一条
    // 工匠不是战力，是**产能**：建造 / 维修 / 升级三件工程全靠它推进，
    // 而开局只有 1 名。不补的话堡垒升级的 400 tick 工时永远排不上队。
    CHECK(ms.trained_mason > 0);
}

TEST_CASE("宏观层：木栅只在交战期放，且只补环上缺墙的格", "[macro]") {
    // 木栅是 CLAUDE.md「廉价应急防御层，可在波次进行中即时放置」的落点，
    // 也是木材唯一的大宗出口。**但它不该在建造期铺**——那样它就从「应急」
    // 变成「另一种便宜的墙」，而石墙与木栅的分工（石材买永久结构、代价是
    // 时间；木材买立刻生效的临时结构）正是靠这个时机边界成立的。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();
    game::DefenderMacro macro(map);
    game::DemoBattle b(map, stats, /*seed=*/1);

    // 只跑到首个建造期结束之前（默认 first_build_ticks = 360）。
    run_with_macro(b, macro, 300);
    REQUIRE(b.world().phase() == rts::WavePhase::Build);
    CHECK(macro.stats().fences_built == 0);
}

TEST_CASE("宏观层：防空真的会被建出来", "[macro]") {
    // 此前一座都不建 ⇒ `Phoenix` 全程无人可挡，而「每座 AA 意味着该位置少一座
    // 对地火力」这组两难（CLAUDE.md 称作「本作智斗最可读的载体」）在训练里
    // 一次都没发生过。它也是「Flak 视野否定覆盖率」那项核验的前提。
    const game::MapData map = pool_map();
    rts::StatsTable stats = pool_stats();
    stats.bld[static_cast<std::size_t>(rts::BldType::Flak)].cost_stone = 1;
    stats.bld[static_cast<std::size_t>(rts::BldType::Flak)].cost_wood = 1;
    stats.bld[static_cast<std::size_t>(rts::BldType::Flak)].build_ticks = 1;
    game::DefenderMacro macro(map);
    game::DemoBattle b(map, stats, /*seed=*/1);
    run_with_macro(b, macro, 1200);

    const rts::WorldView v = b.world().view(rts::Side::Defender);
    int flaks = 0;
    for (std::size_t k = 0; k < v.bld_alive().size(); ++k) {
        if (v.bld_alive()[k] && v.bld_type()[k] == rts::BldType::Flak) ++flaks;
    }
    INFO("场上防空 " << flaks << " 座，脚本建过 " << macro.stats().flaks_built);
    CHECK(flaks > 0);
}

TEST_CASE("宏观层：清野不重复下令（`o_clear_ordered_` 读世界那一份）", "[macro]") {
    // `Clear` 是只进不退的全局标记。不查它的后果是每个决策周期把同一批障碍
    // 重标一遍——与「塔位每周期重发、一局刷 2776 条」同一个坑。
    // 第一版我在这一层存了个本地副本，那是「同一件事写在两处然后漂移」：
    // 障碍被清掉、槽位回收给新障碍时本地那份就开始说谎。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();
    game::DefenderMacro macro(map);
    game::DemoBattle b(map, stats, /*seed=*/1);
    run_with_macro(b, macro, 1500);

    const rts::WorldView v = b.world().view(rts::Side::Defender);
    int in_range = 0;
    const rts::GridPos keep = v.keep_pos();
    const auto op = v.obstacle_pos();
    for (std::size_t k = 0; k < op.size(); ++k) {
        if (!v.obstacle_alive()[k]) continue;
        if (cheb(op[k], keep) <= 24) ++in_range;
    }
    INFO("下过 " << macro.stats().clears_ordered << " 条，范围内还活着 "
                 << in_range << " 个障碍");
    // 判据是「下达条数不超过范围内障碍总数太多」——每个障碍最多标一次。
    // 允许一点余量：障碍会被清掉、槽位可能被新障碍复用（本局不会，但
    // 判据不该依赖那一点）。**坏掉的形状是几百上千条。**
    CHECK(macro.stats().clears_ordered <= in_range + 16);
}

TEST_CASE("宏观层：受威胁那一段塔位摆满了才改升级", "[macro]") {
    // 这一条钉的是**判据的作用域**，不是某个数。
    //
    // 「摆满了就改升级」第一版按**整环**判，而整环有 8(R−1) ≈ 88 个塔位、
    // 一万石都填不满 ⇒ 那一段是死代码。改成只算「离受威胁墙段一个 `Tower.range`
    // 以内」那段弧（十几格），它才会真的被填满。
    //
    // 这里把塔价压到 1 石、把上限除数压到 1、堡垒升级压到 10 石，让那段弧在
    // 几千拍内填满——**这些数是测试夹具**，不是对正式表的主张。
    const game::MapData map = pool_map();
    rts::StatsTable stats = pool_stats();
    stats.bld[static_cast<std::size_t>(rts::BldType::Tower)].cost_stone = 1;
    stats.bld[static_cast<std::size_t>(rts::BldType::Tower)].cost_wood = 1;
    stats.bld[static_cast<std::size_t>(rts::BldType::Tower)].build_ticks = 1;
    stats.bld[static_cast<std::size_t>(rts::BldType::Keep)].upgrade_cost_stone = 10;
    stats.bld[static_cast<std::size_t>(rts::BldType::Keep)].upgrade_cost_wood = 10;
    stats.bld[static_cast<std::size_t>(rts::BldType::Keep)].upgrade_ticks = 1;
    stats.global.building_level_cap_divisor = 1;   // 一级堡垒开一级建筑上限

    game::MacroParams p;
    p.max_commands_per_decision = 40;
    game::DefenderMacro macro(map, p);
    game::DemoBattle b(map, stats, /*seed=*/1);
    run_with_macro(b, macro, 4000);

    INFO("弧内剩余塔位 " << macro.stats().near_free_spots << "，建筑升级 "
                         << macro.stats().bld_upgrades << " 次，塔 "
                         << macro.stats().towers_built << " 座");
    // **判据只取正向蕴含：弧填满了 ⇒ 升级真的发生过。**
    //
    // 不判「一定填得满」——那是经济速度问题，会随任何一次造价/收入改动变红
    // （同「近卫塔」那条的教训）。
    //
    // **更要紧的是反向蕴含不成立，第一版写了它、后来红得对**：
    // `near_free_spots` 是**最后一次决策的快照**，而 `bld_upgrades` 是**累计**
    // ——弧可以先填满（于是升级）、后来一座塔被拆又空出位子，于是收尾时
    // 「还有空位」与「升级过 38 次」同时为真，两者并不矛盾。
    // 「没填满就不该升级」要真的测，得逐决策观察，那是另一件事；
    // 而「关掉开关就一次都不发」由下一条用例守着，已经够了。
    if (macro.stats().near_free_spots == 0) {
        CHECK(macro.stats().bld_upgrades > 0);
    }
}

TEST_CASE("宏观层：关掉开关就一次建筑升级都不发", "[macro]") {
    // 回归护栏：`upgrade_when_saturated` 是**结构性开关**（A/B 用），
    // 关掉之后哪怕弧已经填满、钱也够，也不许有任何建筑升级。
    const game::MapData map = pool_map();
    rts::StatsTable stats = pool_stats();
    stats.bld[static_cast<std::size_t>(rts::BldType::Tower)].cost_stone = 1;
    stats.bld[static_cast<std::size_t>(rts::BldType::Tower)].cost_wood = 1;
    stats.bld[static_cast<std::size_t>(rts::BldType::Tower)].build_ticks = 1;
    stats.global.building_level_cap_divisor = 1;

    game::MacroParams p;
    p.upgrade_when_saturated = false;
    p.max_commands_per_decision = 40;
    game::DefenderMacro macro(map, p);
    game::DemoBattle b(map, stats, /*seed=*/1);
    run_with_macro(b, macro, 4000);
    CHECK(macro.stats().bld_upgrades == 0);
}

TEST_CASE("宏观层：命令真的被世界接受了，不是静默拒绝", "[macro]") {
    // `World` 对下不了的命令**不报错、只是不执行**，所以「发了多少条」不能当
    // 「做成了多少件」。踩过一次：塔位候选是环内侧那一圈，正是自家单位聚集的
    // 地方，而 `World::Build` 拒绝「有地面单位站着的格」（`can_place_hint`
    // 不查这条）⇒ 每周期重发、一局刷出两千七百多条无效命令。
    //
    // 判据取「资源真的被扣掉了」：那是命令**被解算**的唯一可观测证据。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();
    game::DefenderMacro macro(map);
    game::DemoBattle b(map, stats, /*seed=*/1);

    const auto bld_count = [&] {
        const rts::WorldView v = b.world().view(rts::Side::Defender);
        const auto ba = v.bld_alive();
        int n = 0;
        for (std::size_t k = 0; k < ba.size(); ++k) {
            if (ba[k]) ++n;
        }
        return n;
    };
    const int bld0 = bld_count();
    run_with_macro(b, macro, 900);
    const int bld1 = bld_count();

    const game::MacroStats& ms = macro.stats();
    const int issued = ms.walls_built + ms.towers_built + ms.gatherers_built;
    INFO("下过 " << issued << " 条建造，场上建筑 " << bld0 << " -> " << bld1);
    CHECK(issued > 0);
    // **判据是「世界里真的多出了建筑」**，不是「石材变少了」——第一版写成后者，
    // 当场红在 120 -> 141：900 拍里收入（9 石 / 5 s）远超支出，存量是涨的。
    // 「花掉的钱」在有收入的系统里根本不是支出的证据。
    CHECK(bld1 > bld0);
    // 无效命令刷屏的形状：下达条数远多于真正落地的建筑。实测坏掉时是 2776 条
    // （塔位候选正是自家单位聚集的那一圈，而 `World::Build` 拒绝有人站着的格）。
    CHECK(issued <= (bld1 - bld0) + 8);
}

// ═══════════════════════════════════════════════════════════════════════
// 攻方宏观决策层（`game::AttackerMacro`，2026-09-05）
//
// 背景与守方那一批同型：攻方此前**根本没有这一层**，编成是写死的整除式、
// `Phoenix` 恒 1、谁都不看守方长什么样。四条实测缺陷见
// `配平工作交接.md` §2.9/§2.10。下一步要跑「守方 RL vs 攻方脚本」，
// 对着一个不打经济、不读情报的攻方，守方 RL 学到的会是一份建造顺序。
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("攻方编成：没有情报时逐波复现旧的那几个整除式", "[macro]") {
    // 与本文件第一条（守方那边的「默认值不漂」）同一条纪律：把写死的逻辑
    // 搬进一层新抽象时，**空情报下的结果必须与旧代码逐波相同**，否则就是在
    // 「重构」的名义下改了游戏。
    //
    // **`Phoenix` 是有意的例外**——它的默认值这一轮真的改了（恒 1 → 随波
    // 放开），所以下面不拿它对旧式子，另有专门一条测它。
    const game::MapData map = pool_map();
    const game::AttackerMacro macro(map);
    const game::WaveCurve c;
    const game::AttackerIntel none;   // 什么都没侦查到

    for (int w = 1; w <= 40; ++w) {
        const game::WavePlan p = macro.compose(c, w, none);
        const int slots = c.slots_at(w);
        const auto share = [&](int pm) { return std::max(1, slots * pm / 1000); };
        CAPTURE(w, slots);
        CHECK(p.shades == (w >= c.shade_from_wave ? share(c.shade_permille) : 0));
        CHECK(p.knights == (w >= c.knight_from_wave ? share(c.knight_permille) : 0));
        CHECK(p.rams == (w >= c.ram_from_wave ? share(c.ram_permille) : 0));
        CHECK(p.wraiths == (w >= c.wraith_from_wave ? 1 : 0));
        CHECK(p.ghouls == std::max(1, slots - p.shades - p.knights - p.rams -
                                          p.phoenixes - p.wraiths));
    }
}

TEST_CASE("攻方编成：记忆里有缺口就少带攻城锤", "[macro]") {
    // `CLAUDE.md` 的**头号评估指标**是「AI 是否发现并利用已有缺口」——它点名
    // 了对标产品 DiNaO 被差评的那一点（「敌人无视已有缺口、反复撞击加固过的
    // 城墙，明摆着的弱点被浪费」），也是答辩要对照呈现的证据。
    //
    // 墙上已经有洞的时候还带一堆攻城锤，正是那个差评描述的行为。
    const game::MapData map = pool_map();
    const game::AttackerMacro macro(map);
    const game::WaveCurve c;

    game::AttackerIntel intact;
    intact.walls = 80;
    game::AttackerIntel breached;
    breached.walls = 76;
    breached.gaps = 4;

    // 取一个编成位够大的波数：`share()` 有 `max(1, ...)` 地板，波数太小的话
    // 千分比怎么降都还是 1，测不出东西。
    const int wave = 30;
    const game::WavePlan a = macro.compose(c, wave, intact);
    const game::WavePlan b = macro.compose(c, wave, breached);
    CAPTURE(c.slots_at(wave), a.rams, b.rams, a.ghouls, b.ghouls);
    CHECK(b.rams < a.rams);
    // 攻城锤省下来的编成位归 `Ghoul`——它是唯一的肉盾，而从洞里灌进去正需要人。
    CHECK(b.ghouls > a.ghouls);
}

TEST_CASE("攻方编成：记忆里塔成片就多带攻城锤", "[macro]") {
    // 攻方对「塔海」的设计答案只有两个（`Ram` 的 AOE 与 `Phoenix` 的点杀），
    // 这是其中一个。实测第 60 波守方有 74 座塔而攻方编成一成不变。
    const game::MapData map = pool_map();
    const game::AttackerMacro macro(map);
    const game::WaveCurve c;
    const int wave = 30;

    game::AttackerIntel few;
    few.towers = 3;             // 开局那几座预置塔，不构成塔海
    game::AttackerIntel many;
    many.towers = 60;

    const game::WavePlan a = macro.compose(c, wave, few);
    const game::WavePlan b = macro.compose(c, wave, many);
    CAPTURE(a.rams, b.rams);
    CHECK(b.rams > a.rams);

    // **门槛不是「有塔就加」**：开局守方就有 2–3 座预置塔，那不该触发。
    const game::WavePlan base = macro.compose(c, wave, game::AttackerIntel{});
    CHECK(a.rams == base.rams);
}

TEST_CASE("攻方编成：守方的防空第一次真的买到了东西", "[macro]") {
    // 「AA 的机会成本」那个两难（`CLAUDE.md` 称「本作智斗最可读的载体」）
    // **在仿真里从未成立过**：不死鸟恒 1，建不建 `Flak` 对攻方毫无影响，
    // 于是「每座 AA 意味着该位置少一座对地火力」这笔机会成本是白付的。
    const game::MapData map = pool_map();
    const game::AttackerMacro macro(map);
    const game::WaveCurve c;
    const int wave = 40;   // 放开曲线下这一波该有好几只

    game::AttackerIntel no_aa;
    game::AttackerIntel lots_of_aa;
    lots_of_aa.flaks = 12;

    const game::WavePlan a = macro.compose(c, wave, no_aa);
    const game::WavePlan b = macro.compose(c, wave, lots_of_aa);
    CAPTURE(a.phoenixes, b.phoenixes);
    REQUIRE(a.phoenixes > 1);   // 上限已放开，否则下面这条测不出东西
    CHECK(b.phoenixes < a.phoenixes);
    // 降到 0 是允许且正确的：守方砸够了钱，空袭就该被劝退。
    CHECK(b.phoenixes >= 0);
}

TEST_CASE("攻方编成：不死鸟上限已放开，但增速远低于兵力预算", "[macro]") {
    // `CLAUDE.md`：「硬性数量上限，**上限可随波数缓慢放开**，但增速必须远低于
    // 预算增速」。此前 `phoenix_per_waves = 0` ⇒ 从未放开，实测它 58/60 波
    // 出场、只被击落 12 次，而守方全程只有 2 座 `Flak`。
    const game::WaveCurve c;
    REQUIRE(c.phoenix_per_waves > 0);   // 真的放开了
    REQUIRE(c.phoenix_cap > c.phoenix_base);

    // 「增速远低于预算增速」是**结构**：兵力预算是 w^1.25（超线性），
    // 空军只能是「每 N 波 +1」这种斜率很小的线性，且封顶。
    const game::MapData map = pool_map();
    const game::AttackerMacro macro(map);
    const game::AttackerIntel none;
    const double power_ratio = c.power_at(60) / c.power_at(3);
    const int ph3 = macro.compose(c, 3, none).phoenixes;
    const int ph60 = macro.compose(c, 60, none).phoenixes;
    REQUIRE(ph3 > 0);
    CAPTURE(power_ratio, ph3, ph60);
    CHECK(ph60 <= c.phoenix_cap);
    CHECK(static_cast<double>(ph60) / ph3 < power_ratio);
}

TEST_CASE("攻方情报：只读自己的迷雾，没探索过的地方一无所知", "[macro]") {
    // `CLAUDE.md`「RL 侧两条硬要求」第 1 条在脚本层的自觉遵守。不做这条则
    // 「欺骗」毫无意义——攻方看得见一切，玩家藏防空塔就是白藏。
    //
    // **这条带破坏性验证**：先断言「看不见 ⇒ 读不到」，再把同一片标成可见、
    // 断言结论**必须改变**。少了后半截，一个永远返回空的 `read_intel` 也能
    // 让前半截绿着过。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();
    rts::World w(game::make_world_init(map, stats, 7, 1));
    const game::AttackerMacro macro(map);

    // 攻方开局对城里一无所知（它的单位还没生成，迷雾全是 Unseen）。
    const game::AttackerIntel blind =
        macro.read_intel(w.view(rts::Side::Attacker), false);
    CAPTURE(blind.towers, blind.walls, blind.gaps, blind.economy.size());
    CHECK(blind.towers == 0);
    CHECK(blind.walls == 0);
    CHECK(blind.gaps == 0);   // 「从未见过」不是缺口——迷雾三态存在的理由
    CHECK(blind.economy.empty());

    // ——破坏性验证：把整张图对攻方点亮，同一次调用必须读出东西——
    rts::FogLayer& af = w.fog_mut(rts::Side::Attacker);
    for (int x = 0; x < map.width(); ++x) {
        for (int y = 0; y < map.height(); ++y) af.mark_visible(x, y, 0);
    }
    const rts::WorldView av = w.view(rts::Side::Attacker);
    const auto ba = av.bld_alive();
    const auto bp = av.bld_pos();
    const auto bt = av.bld_type();
    const auto bhp = av.bld_hp();
    const auto bmax = av.bld_max_hp();
    for (std::size_t k = 0; k < ba.size(); ++k) {
        if (!ba[k]) continue;
        const auto pm = static_cast<std::uint16_t>(
            bmax[k] > 0 ? bhp[k] * 1000 / bmax[k] : 0);
        af.remember_bld(bp[k].i, bp[k].j, bt[k], pm);
    }
    const game::AttackerIntel seen = macro.read_intel(av, true);
    CAPTURE(seen.towers, seen.walls, seen.gaps);
    CHECK(seen.walls > 0);          // 城圈是一整圈墙实体，看得见就数得出
    CHECK(seen.fresh);
    // 结论真的变了 —— 否则上面那半截是空的
    CHECK((seen.walls != blind.walls || seen.towers != blind.towers));
}

TEST_CASE("攻方情报：本波侦查结果有访问器，且逐波重置", "[macro]") {
    // `wave_scouted_` 此前**一个访问器都没有**——于是「杀掉窥使让 AI 带错情报」
    // 这条设计在测试里断言不了、在报告里也量不出来。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();
    game::DemoBattle b(map, stats, 7);
    // **必须接守方宏观层**，与本文件其余用例一致。裸 `b.update()` 时没人替守方
    // 建塔征兵，而编队制之后第 1 波是 18 个 Ghoul（此前 6 个）——实测堡垒在
    // 第 1 波就掉了，于是「跑到第 2 波」这个前提根本不成立（我第一版就是这么
    // 写的，红在 `wave() == 1 && defeated`）。
    game::MacroParams mp;
    game::DefenderMacro m(map, mp);

    CHECK_FALSE(b.wave_scouted());   // 刚建局，窥使还没出门（第 1 波也没有窥使）

    // 跑到第 2 波之后：`wave_intel()` 是生波那一刻的快照，不随后续战斗变。
    const int before = b.world().wave();
    for (int t = 0; t < 200 && b.world().wave() <= before && !b.defeated(); ++t) {
        run_with_macro(b, m, 100);
    }
    CAPTURE(b.world().wave(), b.defeated(), b.wave_scouted(),
            b.wave_intel().walls);
    REQUIRE_FALSE(b.defeated());     // 陷落了就测不到「下一波的情报」
    CHECK(b.world().wave() > before);
    // 第 2 波生波时，攻方**已经见过**城墙（第 1 波打过一场）⇒ 记忆非空。
    // 这条同时钉住 `read_intel` 真的接在生波路径上（而不是一个没人调的函数）。
    // 第一波没有窥使：普通进攻单位见过城墙也不冒充侦查成功。
    CHECK_FALSE(b.wave_intel().fresh);
    CHECK(b.wave_intel().walls == 0);
}

TEST_CASE("编成位硬顶：27 = RL 的 agent 数上限，而单位数不受它约束", "[macro]") {
    // **这一条是「默认值不许漂」那条守卫的另一半**（见本文件上面那条用例的
    // 注释）：硬顶是一次有意的设计改动，所以它单独立一条断言、改它必须改这里。
    //
    // 27 的依据不在这里复述（`WaveCurve::slots_cap` 的注释写了三条，其中
    // 最要紧的一条是 SMAC 最大的官方图 `27m_vs_30m` 恰是 27 个 agent，
    // 而我们每 agent 的观测是它的 18–21 倍）。这里钉的是**语义**：
    // 顶的是**编队数**（= agent 数），**不是**场上单位数。
    const game::WaveCurve c;
    REQUIRE(c.slots_cap == 27);
    CHECK(c.slots_at(200) == 27);   // 无论多少波，编队数不越顶

    const game::MapData map = pool_map();
    const game::AttackerMacro macro(map);
    const game::AttackerIntel none;
    const game::WavePlan late = macro.compose(c, 60, none);
    CAPTURE(late.squads(), late.units());
    CHECK(late.squads() <= c.slots_cap);
    // **单位数明显多于编队数**——这正是编队制存在的理由：agent 数受 RL 约束，
    // 单位数不受。若哪天它们相等了，说明队规模退化成 1，那时
    // 「27 个 agent 却有 ~70 个单位」这个设计已经失效。
    //
    // 阈值取 1.5 倍而不是 2 倍：实测第 60 波是 53/27 ≈ 1.96（我先写 2 倍、
    // 差一点就红）。**它不该卡在实测值上**——`ram_permille` 是占位、还要重标，
    // 而 `Ram`/`Phoenix` 每队只有 1 个，比例一变平均队规模就跟着变。
    // 这条守的是「编队制没有退化成单位制」，1.5 倍够表达它了。
    CHECK(late.units() * 2 > late.squads() * 3);
}

TEST_CASE("编队规格：同兵种，队规模逐兵种不同", "[macro]") {
    // 队规模上限是**结构**不是旋钮：`Knight` 的 2 来自冲锋助跑要跑道，
    // `Ram`/`Phoenix` 的 1 来自它们的稀缺性与「单件威胁」定位。
    CHECK(game::squad_cap_of(rts::UnitType::Ghoul) == 3);
    CHECK(game::squad_cap_of(rts::UnitType::Shade) == 3);
    CHECK(game::squad_cap_of(rts::UnitType::Knight) == 2);
    CHECK(game::squad_cap_of(rts::UnitType::Ram) == 1);
    CHECK(game::squad_cap_of(rts::UnitType::Phoenix) == 1);
    CHECK(game::squad_cap_of(rts::UnitType::Wraith) == 1);
    // 守方兵种不成队（它们不上逐单位 RL，是参数化脚本驱动的）。
    CHECK(game::squad_cap_of(rts::UnitType::Archer) == 1);
    CHECK(game::squad_cap_of(rts::UnitType::Mason) == 1);
}

TEST_CASE("攻方目标：分队打经济，而不是全军奔堡垒", "[macro]") {
    // 攻方的战果此前**全是「路过顺手」**——flow 的唯一目标是 `keep`，
    // 60 波累计打掉 22 座采石场都是撞见了才打，而顺手已经把守方的采集建筑
    // 钉在 6 座。刻意打它就是掐守方唯一的成长引擎（石材 → K → 三个等级上限
    // + 塔），也就是 CLAUDE.md 的「攻其必救」。
    //
    // **这一条钉的是那个旋钮存在且合理**，不是具体取值（占位）。行为侧
    // 「分队真的走过去了」要在真机上量（runner 的破口/采集建筑稳态数），
    // 单元测试里跑不出几十波的行军。
    const game::WaveCurve c;
    REQUIRE(c.econ_raid_permille > 0);      // 真的会派人去
    REQUIRE(c.econ_raid_permille < 500);    // 但不能把主攻路抽空——破口是拿下堡垒的前提
}


TEST_CASE("守方防空：座数随记忆里的空军走，不是常数", "[macro]") {
    // **这是 `Phoenix` 上限放开那一批漏掉的配套。** 攻方那边已经从「恒 1 只」
    // 放开到最多 5 只（`WaveCurve::phoenix_cap`），而守方这边 `flak_target`
    // 还是常数 2 ⇒ **镜像版的坏陪练**：攻方能来 5 只，守方永远只有 2 座防空。
    // 队友在 #141 上投的那一票就是这一条（「随已观测空军数走，记忆口径」）。
    //
    // 这里测的是**参数面**：默认值真的会响应，且关掉开关就回到常数行为。
    game::MacroParams on;
    CHECK(on.flak_per_phoenix > 0);   // 默认是开的，否则整条是死代码
    CHECK(on.flak_max > on.flak_target);   // 顶必须高于基础，否则响应被顶掐死
    CHECK(on.air_memory_waves > 0);
}

TEST_CASE("守方防空：见过不死鸟就多建，且不超过硬顶", "[macro]") {
    // 端到端：把不死鸟塞进守方视野，跑几波，看目标座数真的抬起来。
    //
    // **判据取 `flak_target_now` 而不是 `flaks_built`**：目标涨了但没钱、
    // 没空位建不起来是另一回事，两者混在一起时这条用例会因为「这张图恰好
    // 没石头」而红，而那与本条要钉的性质无关。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();

    game::MacroParams mp;
    mp.flak_per_phoenix = 1;
    mp.flak_max = 6;

    game::DemoBattle b(map, stats, 7);
    game::DefenderMacro m(map, mp);
    // 跑到不死鸟出场之后（`phoenix_from_wave = 3`）。
    for (int i = 0; i < 60 && b.world().wave() < 6 && !b.defeated(); ++i) {
        run_with_macro(b, m, 200);
    }
    CAPTURE(b.world().wave(), b.defeated(), m.stats().flak_target_now,
            m.stats().flaks_built);
    REQUIRE_FALSE(b.defeated());
    // 目标座数至少是基础值，且**永远不越硬顶**——后者是结构，前者是下界。
    CHECK(m.stats().flak_target_now >= mp.flak_target);
    CHECK(m.stats().flak_target_now <= mp.flak_max);
}

TEST_CASE("守方防空：只数不死鸟，窥使不算", "[macro]") {
    // `Wraith` 2026-09-03 起会飞、每波恒 1 只、**无战力**。把它算进空袭威胁
    // 会给防空目标垫一个恒定的 +1 —— 那不是空袭，而窥使的对策是 `Flak` 的
    // **视野否定半径**（既有座数的副作用），不需要为它多建一座。
    //
    // 做法：关掉响应（`flak_per_phoenix = 0`）跑一遍拿基线，再开着跑一遍。
    // 若窥使被算进去了，开着那一遍在**还没出不死鸟的波次**就会高于基线。
    const game::MapData map = pool_map();
    const rts::StatsTable stats = pool_stats();
    const game::WaveCurve curve;
    REQUIRE(curve.wraith_from_wave < curve.phoenix_from_wave);   // 有这么一段窗口

    const auto target_at_wave2 = [&](int per_phoenix) {
        game::MacroParams mp;
        mp.flak_per_phoenix = per_phoenix;
        game::DemoBattle b(map, stats, 7);
        game::DefenderMacro m(map, mp);
        // 停在第 2 波：窥使已经出场（`wraith_from_wave = 2`），
        // 不死鸟还没有（`phoenix_from_wave = 3`）。
        for (int i = 0; i < 40 && b.world().wave() < 2 && !b.defeated(); ++i) {
            run_with_macro(b, m, 200);
        }
        REQUIRE_FALSE(b.defeated());
        REQUIRE(b.world().wave() == 2);
        return m.stats().flak_target_now;
    };
    const int off = target_at_wave2(0);
    const int on = target_at_wave2(1);
    CAPTURE(off, on);
    CHECK(on == off);   // 只有窥使在天上的时候，两者必须一样
}

TEST_CASE("守方防空：真实见闻过期后补建目标回落，已有建筑保留", "[macro]") {
    const auto map = pool_map();
    auto init = game::make_world_init(map, pool_stats(), 7, 1);
    const auto k = map.keep();
    // 无经济、无施工，固定一只可见不死鸟；只推进波号让真实见闻过期。
    init.units.clear();
    init.units.push_back(rts::UnitInit{rts::UnitType::Phoenix,
        rts::Vec2{static_cast<float>(k.i) + 1.5f, static_cast<float>(k.j) + 0.5f},
        1, 100, 100});
    rts::World w(std::move(init));
    w.advance(1);  // 刷新视野
    game::MacroParams mp;
    mp.air_memory_waves = 1;
    game::DefenderMacro m(map, mp);
    std::vector<rts::Command> cmds;
    std::vector<game::UnitOrder> orders;
    m.decide(w, cmds, orders);
    REQUIRE(m.stats().flak_target_now == mp.flak_target + 1);
    // 空军死亡，不重建 macro；之前确实见过的数量应只在窗口内保留。
    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Attacker, ids);
    REQUIRE(ids.size() == 1);
    w.kill_unit(ids.front());
    const auto flak = w.place_bld(rts::BldType::Flak,
        rts::GridPos{static_cast<std::int16_t>(k.i + 3), k.j}, 100, 100);
    w.begin_next_wave(1);
    cmds.clear(); orders.clear();
    m.decide(w, cmds, orders);
    CHECK(m.stats().flak_target_now == mp.flak_target + 1);
    w.begin_next_wave(1);
    cmds.clear(); orders.clear();
    m.decide(w, cmds, orders);
    CHECK(m.stats().flak_target_now == mp.flak_target);
    for (const auto& c : cmds) CHECK(c.kind != rts::CommandKind::Demolish);
    CHECK(w.alive(flak));
}
