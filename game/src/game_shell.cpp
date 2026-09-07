#include "game/game_shell.hpp"

#include <utility>
#include <stdexcept>

namespace game {
namespace {

// 第 n 次开局用哪个种子。
//
// **不是平衡数值，是种子**，所以定在代码里不违反「数值一律待定」。
// 每次「重新开始」换一个：同一个种子重开会让守方脚本的每一次掷点完全重演，
// 那对调试有用、对玩却不是——它会让「这一波从哪儿来、谁先动」变成背下来的东西。
// 换成 attempt 的函数而不是挂钟：**仍然可复现**（第 3 局永远是第 3 局），
// 而挂钟会直接踩确定性禁令（`tools/check_determinism_bans.py` 扫 `game/`）。
std::uint64_t seed_for_attempt(std::uint64_t base, int attempt) {
    // 黄金比例常数混一下，免得相邻两局的种子只差 1（xoshiro 的低位在种子相近时
    // 前几个输出也相近，见 `rts/rng.hpp`）。
    //
    // **常数必须显式声明成 `std::uint64_t`，不能直接写 `...ull` 字面量。**
    // LP64（Linux）上 `std::uint64_t` 是 `unsigned long`，而 `ull` 字面量是
    // `unsigned long long`——两者宽度相同却是**不同类型**，于是乘法结果要隐式
    // 转回 `unsigned long`，GCC 的 `-Wsign-conversion` 因此报错（「may change
    // the sign of the result」，尽管两边都是无符号）。Windows 是 LLP64、
    // `uint64_t` 就是 `unsigned long long`，所以 MSVC 一声不响。
    constexpr std::uint64_t kGolden = 0x9e3779b97f4a7c15ULL;
    return base ^ (kGolden * static_cast<std::uint64_t>(attempt));
}

}  // namespace

GameShell::GameShell(MapData map, rts::StatsTable stats, std::uint64_t base_seed)
    : map_(std::move(map)), stats_(std::move(stats)), base_seed_(base_seed) {
    go_(Screen::Main);
}

bool GameShell::should_advance() const noexcept {
    return screen_ == Screen::Battle && battle_ && !battle_->defeated() &&
           (battle_->developer() || (!chronicle_.completed() && !chronicle_.pending(battle_->world().wave())));
}

bool GameShell::choose_chronicle(ChronicleChoice choice) {
    if(!battle_ || battle_->defeated() || !chronicle_.choose(choice,battle_->world().wave())) return false;
    // 结局作为文字过场完成；不向仿真伪造伤害或训练胜负。
    // Release 终止本局的交互推进，回主菜单后不可 Resume。
    if(chronicle_.completed()) go_(Screen::Main);
    return true;
}

void GameShell::adopt_saved_battle(DemoBattle&& battle,int attempt,ChronicleChoice choice) {
    ChronicleDecision decision;
    if(choice!=ChronicleChoice::None && !decision.choose(choice,battle.world().wave()))
        throw std::runtime_error("存档剧情选择与波次不符");
    battle_.emplace(std::move(battle));
    attempt_=attempt;chronicle_=decision;
    go_(Screen::Main);
}

void GameShell::go_(Screen s) {
    screen_ = s;
    // 每次换屏都重建菜单：条目的可用性依赖状态（「继续对局」要有一局在跑），
    // 而「换屏时忘了刷新」的症状是一个灰着但其实能按的选项。
    switch (s) {
        case Screen::Main:
            menu_.reset(main_menu_items(/*can_resume=*/battle_ &&
                                        !battle_->defeated() && !chronicle_.completed()));
            break;
        case Screen::Paused:
            menu_.reset(pause_menu_items());
            break;
        case Screen::Defeat:
            menu_.reset(defeat_menu_items());
            break;
        case Screen::Help:
            menu_.reset(help_menu_items());
            break;
        case Screen::Battle:
            menu_.reset({});   // 对局中没有菜单
            break;
    }
}

void GameShell::start_battle_() {
    chronicle_=ChronicleDecision{};
    ++attempt_;
    // 先 reset 再 emplace：`DemoBattle` 里有一整个 `World`，两局同时活着没有
    // 意义，而 `optional::emplace` 本来就会先析构旧的——写出来是为了让
    // 「上一局的内存什么时候还」这件事在代码里看得见。
    battle_.reset();
    battle_.emplace(map_, stats_, seed_for_attempt(base_seed_, attempt_));
}

void GameShell::apply(MenuAction a) {
    switch (a) {
        case MenuAction::None:
        case MenuAction::Save: // 磁盘 IO 属于前端；状态机保持当前屏
            break;
        case MenuAction::StartNew:
        case MenuAction::Restart:
            start_battle_();
            go_(Screen::Battle);
            break;
        case MenuAction::Resume:
            // 只在真有一局能接着打时才回战场。菜单那边已经把它灰掉了，
            // 这里再挡一次——键盘、鼠标、Esc 三条路都汇到 apply()，
            // 而「可用性只在一处判」比「三处各判一次」可靠。
            if (chronicle_.completed()) {
                go_(Screen::Main);
            } else if (battle_ && !battle_->defeated()) {
                go_(Screen::Battle);
            } else if (battle_) {
                go_(Screen::Defeat);
            }
            break;
        case MenuAction::Help:
            help_from_ = screen_;
            go_(Screen::Help);
            break;
        case MenuAction::Back:
            go_(help_from_);
            break;
        case MenuAction::ToMain:
            // **不丢对局，只是换一屏。** 曾经写成「顺手 `battle_.reset()`」，
            // 那会让主菜单的「继续对局」**永远**是灰的（回主菜单是进入那一屏的
            // 唯一路径，而它自己把对局删了）——一个恒不可用的选项比没有更糟。
            //
            // 真正丢掉上一局的地方只有一处：`start_battle_()`。于是「什么时候
            // 会失去当前进度」只有一个答案——你自己按了「开始新对局」。
            go_(Screen::Main);
            break;
        case MenuAction::Quit:
            quitting_ = true;
            break;
    }
}

void GameShell::on_escape() {
    switch (screen_) {
        case Screen::Battle:
            go_(Screen::Paused);
            break;
        case Screen::Paused:
            apply(MenuAction::Resume);
            break;
        case Screen::Help:
            apply(MenuAction::Back);
            break;
        case Screen::Defeat:
            apply(MenuAction::ToMain);
            break;
        case Screen::Main:
            break;   // 见头文件：主菜单上 Esc 什么都不做
    }
}

void GameShell::poll() {
    if (screen_ == Screen::Battle && battle_ && battle_->defeated()) {
        go_(Screen::Defeat);
    }
}

}  // namespace game
