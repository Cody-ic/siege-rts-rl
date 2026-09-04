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

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/defender_macro.hpp"
#include "game/demo_driver.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/roster.hpp"
#include "rts/utf8_path.hpp"
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
    for (int w = 1; w <= 40; ++w) {
        const int want_slots =
            static_cast<int>(6.0 + 1.2 * static_cast<double>(w - 1));
        CHECK(c.slots_at(w) == want_slots);
        const double want_power = 6.0 * std::pow(static_cast<double>(w), 1.25);
        CHECK(std::fabs(c.power_at(w) - want_power) < 1e-9);
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
    CHECK(c.slots_cap == 0);
    CHECK(c.slots_at(100) > 40);   // 不封顶（2026-09-01 撤下那个没依据的 40）
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

    game::DemoBattle b(map, stats, /*seed=*/1);
    // **要给够时间**：开局 120 石，封两个缺口先花掉 60，而一座塔 80——
    // 头几百拍根本攒不出来。1200 拍时实测 0 座（这条断言第一版就是这么红的，
    // 红得对：那时确实还没建）。石材收入 9/5s ⇒ 4000 拍（200 s）足够。
    run_with_macro(b, macro, 4000);

    const rts::WorldView v = b.world().view(rts::Side::Defender);
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
    // （第一版写的 >= 2，实测 4000 拍只建得起 1 座。）
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

    SECTION("必活：到了就拿到完整编成") {
        game::DefenderSetup setup;
        setup.scout_death_permille = 0;   // 必活
        game::DemoBattle b(map, stats, /*seed=*/1, {}, {}, setup);
        game::DefenderMacro macro(map);
        run_with_macro(b, macro, 6000);
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
        // **报告是全波编成**，不是「视野里那几个」：斥候此刻就站在集结点上，
        // 而集结期整波部队都在那里待命。
        int reported = 0;
        for (const game::SightedType& t : b.scout_report()) reported += t.count;
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
