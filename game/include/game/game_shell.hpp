// 「程序现在在哪一屏」——主菜单 / 对局 / 暂停 / 说明 / 败局的状态机，
// 外加对局本身的生命周期（开一局、重开一局、丢掉一局）。
//
// ## 为什么要有这一层，而不是在渲染循环里放几个 bool
//
// 几个 bool 是能跑的，但它们的组合里有一半是无意义状态（`paused && in_menu &&
// !has_battle` 该显示什么？），而每个新屏都会让这个乘积再翻一倍。更要紧的是
// **屏与屏之间的规则本身就是要被测的东西**：
//
//   * 「返回主菜单」是**导航**，不丢对局；丢对局的地方全项目只有「开始新对局」
//     一处（于是「我的进度什么时候会没」只有一个答案）
//   * 「重新开始」必须真的从头（tick 归零、波次归 1），不是接着打
//   * 对局一败就该自己转到败局屏，不能停在战场上让玩家等
//   * 从暂停进说明，返回时要回到**暂停**而不是主菜单
//   * 「继续对局」的可用性由状态**算出来**（有一局且没败），不由某个入口顺手设
//
// 这四条都是纯逻辑，放在 `game/` 就进默认构建、GCC 侧也验（`render/` 默认不配置，
// 挂在那边的测试在默认构建里根本不存在、ctest 照样全绿）。
//
// ## 它不推进仿真
//
// 本类**不调用** `DemoBattle::update()`。推进多少 tick 是帧率的事（渲染循环按
// 20 Hz 补步），而本类只回答「现在该不该推进」（`should_advance()`）。
// 这条边界让「暂停」与「败局定格」这两件事各只有一个来源：前者是屏幕状态，
// 后者在 `DemoBattle` 自己里面。
//
// 交付要求「描述软件的类对象设计」要的正是这类东西：一个有明确职责边界、
// 有不变量、可被测试固定住的类（`交付要求的工程影响.md`）。

#ifndef GAME_GAME_SHELL_HPP
#define GAME_GAME_SHELL_HPP

#include <cstdint>
#include <optional>

#include "game/demo_driver.hpp"
#include "game/chronicle.hpp"
#include "game/map_data.hpp"
#include "game/menu_model.hpp"
#include "rts/stats.hpp"

namespace game {

enum class Screen : std::uint8_t {
    Main,     // 主菜单
    Battle,   // 对局中（唯一会推进仿真的一屏）
    Paused,   // 暂停菜单，战场仍画着、只是不动
    Help,     // 操作说明
    Defeat,   // 堡垒陷落
};

class GameShell {
public:
    // `map` 与 `stats` 按值收下：**重开一局要重建世界，而重建要原始输入**。
    // 存引用的话，调用方一旦让那两个对象先死掉（渲染循环里它们本来就是局部变量），
    // 「重新开始」就会读到已析构的内存——而那是个只在第二局才现形的 bug。
    GameShell(MapData map, rts::StatsTable stats, std::uint64_t base_seed);

    Screen screen() const noexcept { return screen_; }
    // 对局中的那一屏没有菜单（返回空的 `MenuModel`），所以调用方可以无条件问。
    const MenuModel& menu() const noexcept { return menu_; }
    MenuModel& menu() noexcept { return menu_; }

    bool has_battle() const noexcept { return battle_.has_value(); }
    // 没有对局时返回 nullptr。**不给「反正有一局」的假象**：主菜单上确实没有。
    const DemoBattle* battle() const noexcept {
        return battle_ ? &*battle_ : nullptr;
    }
    DemoBattle* battle() noexcept { return battle_ ? &*battle_ : nullptr; }

    const MapData& map() const noexcept { return map_; }

    // 第几次开局（从 1 起）。渲染侧用它判断「该重新预载素材了」。
    int attempt() const noexcept { return attempt_; }
    bool quitting() const noexcept { return quitting_; }

    // 这一帧该不该推进仿真。见文件头「它不推进仿真」。
    bool should_advance() const noexcept;
    const ChronicleDecision& chronicle() const noexcept { return chronicle_; }
    bool choose_chronicle(ChronicleChoice choice);

    // 菜单项被确认。
    void apply(MenuAction a);

    // Esc 的语义随屏而变，收在这里而不是散在按键处理里：
    // 对局→暂停、暂停→回对局、说明→回来处、败局→回主菜单、主菜单→不动
    //（主菜单的退出走菜单项与窗口关闭按钮，Esc 在那里什么都不做是刻意的
    //  ——「按 Esc 直接把程序关掉」是最容易误触的一种设计）。
    void on_escape();

    // 每帧调一次：对局已败而屏幕还停在战场上，就转到败局屏。
    //
    // **做成拉取而不是回调**：`DemoBattle` 不认识 `GameShell`（也不该认识），
    // 而每帧问一次的成本是一次布尔读。
    void poll();

private:
    void go_(Screen s);
    void start_battle_();

    MapData map_;
    rts::StatsTable stats_;
    std::uint64_t base_seed_ = 0;
    std::optional<DemoBattle> battle_;
    Screen screen_ = Screen::Main;
    // 从哪一屏进的说明。**必须记住**，否则从暂停看完说明会掉回主菜单，
    // 而那意味着当前这局悄悄没了。
    Screen help_from_ = Screen::Main;
    MenuModel menu_;
    int attempt_ = 0;
    bool quitting_ = false;
    ChronicleDecision chronicle_;
};

}  // namespace game

#endif  // GAME_GAME_SHELL_HPP
