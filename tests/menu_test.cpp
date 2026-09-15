// 主菜单与屏幕状态机（`game::MenuModel` / `game::GameShell`）。
//
// 这一层是「玩家看到的程序」的骨架，而它的 bug 全是**语义**的：灰项能被选中、
// 返回主菜单没把对局丢掉、重新开始接着上一局打、败了还停在战场上。
// 它们不含像素，所以在默认构建里测（GCC 侧也验）——`render/` 那半边默认不配置，
// 挂在那儿的测试在默认构建里根本不存在、ctest 照样全绿。

#include <cstddef>
#include "game/chronicle.hpp"
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/game_shell.hpp"
#include "game/map_loader.hpp"
#include "game/menu_model.hpp"
#include "game/stats_loader.hpp"

namespace {

game::MapData demo_map() {
    return game::MapLoader::from_file(std::string(GAME_DATA_DIR) +
                                      "/demo_skirmish.json");
}

rts::StatsTable demo_stats() {
    return game::StatsLoader::from_file(std::string(GAME_DATA_DIR) +
                                        "/stats_placeholder.json");
}

game::GameShell make_shell() { return game::GameShell(demo_map(), demo_stats(), 7); }

// 某个动作在当前菜单里是第几项（-1 = 没有）。
int index_of(const game::MenuModel& m, game::MenuAction a) {
    for (std::size_t k = 0; k < m.items().size(); ++k) {
        if (m.items()[k].action == a) return static_cast<int>(k);
    }
    return -1;
}

bool enabled(const game::MenuModel& m, game::MenuAction a) {
    const int k = index_of(m, a);
    return k >= 0 && m.items()[static_cast<std::size_t>(k)].enabled;
}

}  // namespace

TEST_CASE("菜单导航：跳过禁用项、到头环绕", "[menu]") {
    game::MenuModel m(std::vector<game::MenuItem>{
        game::MenuItem{game::MenuAction::StartNew, "A", true},
        game::MenuItem{game::MenuAction::Resume, "B", false},
        game::MenuItem{game::MenuAction::Help, "C", true},
    });
    // 初始落在第一个**可用**项上。
    REQUIRE(m.selected() == 0);

    m.move(1);
    REQUIRE(m.selected() == 2);   // 跳过禁用的 B
    m.move(1);
    REQUIRE(m.selected() == 0);   // 环绕
    m.move(-1);
    REQUIRE(m.selected() == 2);   // 反向也环绕，也跳过 B

    // 鼠标指到禁用项不改选中——否则选中框会跳到一个按了没反应的地方。
    REQUIRE_FALSE(m.point_at(1));
    REQUIRE(m.selected() == 2);
    REQUIRE(m.point_at(0));
    REQUIRE(m.selected() == 0);

    // 确认禁用项是 None，不是抛：按一下灰项是完全正常的用户操作。
    REQUIRE(m.action_at(1) == game::MenuAction::None);
    REQUIRE(m.action_at(99) == game::MenuAction::None);
    REQUIRE(m.confirm() == game::MenuAction::StartNew);
}

TEST_CASE("全禁用与空菜单：selected 是 -1，确认什么都不发生", "[menu]") {
    game::MenuModel empty;
    REQUIRE(empty.selected() == -1);
    REQUIRE(empty.confirm() == game::MenuAction::None);
    empty.move(1);
    REQUIRE(empty.selected() == -1);

    game::MenuModel all_off(std::vector<game::MenuItem>{
        game::MenuItem{game::MenuAction::Resume, "A", false},
        game::MenuItem{game::MenuAction::Resume, "B", false},
    });
    REQUIRE(all_off.selected() == -1);
    all_off.move(1);
    REQUIRE(all_off.selected() == -1);   // 不能挑一个禁用项出来充数
    REQUIRE(all_off.confirm() == game::MenuAction::None);
}

TEST_CASE("开局与暂停：只有对局那一屏推进仿真", "[menu]") {
    game::GameShell s = make_shell();
    REQUIRE(s.screen() == game::Screen::Main);
    REQUIRE_FALSE(s.has_battle());
    REQUIRE_FALSE(s.should_advance());
    // 首次启动时「继续对局」在，但是灰的。
    REQUIRE(index_of(s.menu(), game::MenuAction::Resume) >= 0);
    REQUIRE_FALSE(enabled(s.menu(), game::MenuAction::Resume));

    s.apply(game::MenuAction::StartNew);
    REQUIRE(s.screen() == game::Screen::Battle);
    REQUIRE(s.has_battle());
    REQUIRE(s.battle()->world().now() == 0);
    REQUIRE(s.should_advance());
    REQUIRE(s.attempt() == 1);

    s.battle()->update(40);
    s.on_escape();   // → 暂停
    REQUIRE(s.screen() == game::Screen::Paused);
    REQUIRE_FALSE(s.should_advance());   // 暂停屏不推进
    REQUIRE(s.battle()->world().now() == 40);   // 而且世界没被谁偷偷推

    s.on_escape();   // 暂停屏的 Esc = 继续
    REQUIRE(s.screen() == game::Screen::Battle);
    REQUIRE(s.should_advance());
}

TEST_CASE("重新开始真的从头，且只有它会丢掉上一局", "[menu]") {
    game::GameShell s = make_shell();
    s.apply(game::MenuAction::StartNew);
    s.battle()->update(60);
    REQUIRE(s.battle()->world().now() == 60);

    // 暂停 → 重新开始：新的一局，tick 归零、波次归 1。
    s.on_escape();
    s.apply(game::MenuAction::Restart);
    REQUIRE(s.screen() == game::Screen::Battle);
    REQUIRE(s.attempt() == 2);
    REQUIRE(s.battle()->world().now() == 0);
    REQUIRE(s.battle()->world().wave() == 1);
    REQUIRE_FALSE(s.battle()->defeated());
}

TEST_CASE("返回主菜单只是换一屏，进度还在，「继续对局」因此可用", "[menu]") {
    // 这条钉的是一处**改过的**行为：早先「返回主菜单」顺手把对局删了，
    // 于是主菜单上的「继续对局」永远是灰的——一个恒不可用的选项比没有更糟。
    game::GameShell s = make_shell();
    s.apply(game::MenuAction::StartNew);
    s.battle()->update(60);
    s.on_escape();                         // → 暂停
    s.apply(game::MenuAction::ToMain);     // → 主菜单

    REQUIRE(s.screen() == game::Screen::Main);
    REQUIRE(s.has_battle());
    REQUIRE(s.battle()->world().now() == 60);   // 进度原样在
    REQUIRE(enabled(s.menu(), game::MenuAction::Resume));
    REQUIRE_FALSE(s.should_advance());          // 但主菜单上不推进

    s.apply(game::MenuAction::Resume);
    REQUIRE(s.screen() == game::Screen::Battle);
    REQUIRE(s.battle()->world().now() == 60);
    REQUIRE(s.attempt() == 1);   // 「继续」不是「新开一局」

    // 而「开始新对局」是那个唯一会丢掉进度的入口。
    s.on_escape();
    s.apply(game::MenuAction::ToMain);
    s.apply(game::MenuAction::StartNew);
    REQUIRE(s.attempt() == 2);
    REQUIRE(s.battle()->world().now() == 0);
}

TEST_CASE("说明屏记得从哪儿来", "[menu]") {
    game::GameShell s = make_shell();
    // 主菜单 → 说明 → 返回主菜单
    s.apply(game::MenuAction::Help);
    REQUIRE(s.screen() == game::Screen::Help);
    REQUIRE(s.menu().items().size() == 1);   // 只有「返回」
    s.on_escape();
    REQUIRE(s.screen() == game::Screen::Main);

    // 对局 → 暂停 → 说明 → **回暂停**。掉回主菜单的话，当前这局就悄悄没了。
    s.apply(game::MenuAction::StartNew);
    s.on_escape();
    s.apply(game::MenuAction::Help);
    REQUIRE(s.screen() == game::Screen::Help);
    s.apply(game::MenuAction::Back);
    REQUIRE(s.screen() == game::Screen::Paused);
    REQUIRE(s.has_battle());
}

TEST_CASE("败局：世界一定格就自动转屏，且不再推进", "[menu]") {
    // 把攻方步兵改成拆迁队，尽快打出败局（无任何平衡含义，同 demo_test）。
    rts::StatsTable stats = demo_stats();
    rts::UnitStats& g = stats.unit[static_cast<std::size_t>(rts::UnitType::Ghoul)];
    g.max_hp = 4000;
    g.damage = 5000;
    g.speed = 0.6f;
    g.vs_structure_permille = 4000;

    game::GameShell s(demo_map(), stats, 7);
    s.apply(game::MenuAction::StartNew);
    s.battle()->update(4000);
    REQUIRE(s.battle()->defeated());

    // **转屏靠每帧一次的 poll()**，不是靠谁记得调。
    REQUIRE(s.screen() == game::Screen::Battle);   // 还没 poll
    s.poll();
    REQUIRE(s.screen() == game::Screen::Defeat);
    REQUIRE_FALSE(s.should_advance());
    // 败局屏不给「继续对局」——那一局已经定格了，给一个按了不动的选项更糟。
    REQUIRE(index_of(s.menu(), game::MenuAction::Resume) < 0);
    REQUIRE(index_of(s.menu(), game::MenuAction::Restart) >= 0);

    // 败了之后回主菜单：对局对象还在（回主菜单不丢它），但「继续对局」是灰的
    // ——可用性判的是「有一局**且没败**」，两个条件都要。
    s.apply(game::MenuAction::ToMain);
    REQUIRE(s.screen() == game::Screen::Main);
    REQUIRE(s.has_battle());
    REQUIRE_FALSE(enabled(s.menu(), game::MenuAction::Resume));
    // 就算硬把 Resume 送进去（键鼠三条路径之外的将来入口），也不会回到一局
    // 已经结束的对局，而是回败局屏。
    s.apply(game::MenuAction::Resume);
    REQUIRE(s.screen() == game::Screen::Defeat);

    // 从败局屏重开：新的一局是活的。
    s.apply(game::MenuAction::Restart);
    REQUIRE(s.screen() == game::Screen::Battle);
    REQUIRE_FALSE(s.battle()->defeated());
    REQUIRE(s.should_advance());
}

TEST_CASE("退出：quitting 一旦为真就不再回头", "[menu]") {
    game::GameShell s = make_shell();
    REQUIRE_FALSE(s.quitting());
    s.apply(game::MenuAction::Quit);
    REQUIRE(s.quitting());
}

TEST_CASE("每一屏的文案都进了字体覆盖清单", "[menu]") {
    // `all_menu_strings()` 是前端推导字体码点集合的输入，而**漏一个字符的后果
    // 是画到它的那一帧直接抛**（render/text.hpp 的第 2 个坑）。它是机械推导的，
    // 所以这条测试真正盯的是「机械推导有没有漏掉某个 builder」：
    // 新加一屏菜单却忘了把它加进 all_menu_strings()，这里会红。
    const std::vector<std::string_view>& cover = game::all_menu_strings();
    const auto covered = [&cover](std::string_view s) {
        for (std::string_view c : cover) {
            if (c == s) return true;
        }
        return false;
    };

    game::GameShell s = make_shell();
    // 逐屏走一遍，把每一屏的条目文案都要求在清单里。
    const auto check_menu = [&](const game::MenuModel& m) {
        for (const game::MenuItem& it : m.items()) {
            INFO("没登记的菜单文案：" << std::string(it.label));
            REQUIRE(covered(it.label));
        }
    };
    check_menu(s.menu());                    // 主菜单
    s.apply(game::MenuAction::Help);
    check_menu(s.menu());                    // 说明
    s.apply(game::MenuAction::Back);
    s.apply(game::MenuAction::StartNew);
    s.on_escape();
    check_menu(s.menu());                    // 暂停

    for (const game::HelpEntry& e : game::help_entries()) {
        INFO("没登记的说明文案：" << std::string(e.keys) << " / " << std::string(e.what));
        REQUIRE(covered(e.keys));
        REQUIRE(covered(e.what));
    }
    // 说明表不能是空的：空表会让上面几条断言全部空过（同「扫到 0 个文件即失败」）。
    REQUIRE(game::help_entries().size() >= 6);
}

TEST_CASE("日记解锁边界与章节顺序", "[menu]") {
    REQUIRE(game::chronicle_unlocked(0)==0);
    REQUIRE(game::chronicle_unlocked(1)==1);
    REQUIRE(game::chronicle_unlocked(9)==1);
    REQUIRE(game::chronicle_unlocked(10)==2);
    REQUIRE(game::chronicle_unlocked(69)==7);
    REQUIRE(game::chronicle_unlocked(70)==8);
    REQUIRE(game::chronicle_unlocked(80)==8);
    REQUIRE(game::chronicle_unlocked(800)==8);
    int previous=0;
    for(const auto& entry:game::kChronicle) {
        REQUIRE(entry.wave>previous);
        REQUIRE_FALSE(entry.title.empty());
        REQUIRE_FALSE(entry.text.empty());
        REQUIRE(game::chronicle_unlocked(entry.wave)==game::chronicle_unlocked(entry.wave-1)+1);
        previous=entry.wave;
    }
    REQUIRE_FALSE(game::kChronicleGuard.empty());
    REQUIRE_FALSE(game::kChronicleRelease.empty());
}

TEST_CASE("第七十波剧情选择只提交一次", "[menu]") {
    game::ChronicleDecision release;
    REQUIRE_FALSE(release.choose(game::ChronicleChoice::Release,69));
    REQUIRE_FALSE(release.pending(69));
    REQUIRE(release.pending(70));
    REQUIRE(release.choose(game::ChronicleChoice::Release,70));
    REQUIRE(release.completed());
    REQUIRE_FALSE(release.pending(70));
    REQUIRE_FALSE(release.choose(game::ChronicleChoice::Guard,80));
    game::ChronicleDecision guard;
    REQUIRE(guard.choose(game::ChronicleChoice::Guard,70));
    REQUIRE_FALSE(guard.completed());
    REQUIRE_FALSE(guard.pending(80));
    REQUIRE_FALSE(guard.choose(game::ChronicleChoice::Release,80));
}

TEST_CASE("剧情伪通关冻结本局且不能从菜单恢复", "[menu]") {
    auto shell=make_shell();
    REQUIRE_FALSE(shell.choose_chronicle(game::ChronicleChoice::Release));
    shell.apply(game::MenuAction::StartNew);
    REQUIRE_FALSE(shell.choose_chronicle(game::ChronicleChoice::Release));
    // 仅测试夹具：底层对象实际非 const，使用 World 公开波次接口快速抵达边界，
    // 不运行七十波战斗，也不为生产 UI 增加任意修改世界的接口。
    auto& world=const_cast<rts::World&>(shell.battle()->world());
    for(int wave=1;wave<70;++wave) world.begin_next_wave(1);
    REQUIRE(game::chronicle_unlocked(world.wave())==8);
    REQUIRE_FALSE(shell.should_advance());
    SECTION("放下武器结束本局，返回和 Resume 都不能继续") {
        const auto tick=world.now();
        REQUIRE(shell.choose_chronicle(game::ChronicleChoice::Release));
        REQUIRE(shell.screen()==game::Screen::Main);
        REQUIRE_FALSE(enabled(shell.menu(),game::MenuAction::Resume));
        shell.apply(game::MenuAction::Resume);
        shell.on_escape();shell.poll();
        REQUIRE_FALSE(shell.should_advance());
        REQUIRE(world.now()==tick);
        REQUIRE_FALSE(shell.choose_chronicle(game::ChronicleChoice::Guard));
    }
    SECTION("继续守护恢复无尽推进") {
        REQUIRE(shell.choose_chronicle(game::ChronicleChoice::Guard));
        REQUIRE(shell.should_advance());
        REQUIRE_FALSE(shell.choose_chronicle(game::ChronicleChoice::Release));
    }
    shell.apply(game::MenuAction::Restart);
    REQUIRE(shell.battle()->world().wave()==1);
    REQUIRE(game::chronicle_unlocked(shell.battle()->world().wave())==1);
    REQUIRE(shell.chronicle().choice()==game::ChronicleChoice::None);
    REQUIRE(shell.should_advance());
}

TEST_CASE("图鉴从主菜单和暂停菜单进入并返回原处", "[menu]") {
    game::GameShell s=make_shell();
    REQUIRE(enabled(s.menu(),game::MenuAction::Guide));
    s.apply(game::MenuAction::Guide);REQUIRE(s.screen()==game::Screen::Guide);
    REQUIRE_FALSE(s.should_advance());s.on_escape();REQUIRE(s.screen()==game::Screen::Main);
    s.apply(game::MenuAction::StartNew);s.battle()->update(20);s.on_escape();
    s.apply(game::MenuAction::Guide);REQUIRE_FALSE(s.should_advance());
    s.apply(game::MenuAction::Back);REQUIRE(s.screen()==game::Screen::Paused);
    REQUIRE(s.battle()->world().now()==20);
}
