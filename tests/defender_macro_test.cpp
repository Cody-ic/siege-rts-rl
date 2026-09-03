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
    CHECK(t.first_build_ticks == 360);   // 18 s @ 20 Hz
    CHECK(t.build_ticks == 260);         // 13 s @ 20 Hz
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
