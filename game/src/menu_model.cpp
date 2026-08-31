#include "game/menu_model.hpp"

#include <cstddef>

namespace game {

void MenuModel::reset(std::vector<MenuItem> items) {
    items_ = std::move(items);
    selected_ = -1;
    for (std::size_t k = 0; k < items_.size(); ++k) {
        if (items_[k].enabled) {
            selected_ = static_cast<int>(k);
            break;
        }
    }
}

void MenuModel::move(int delta) {
    const int n = static_cast<int>(items_.size());
    if (n == 0 || delta == 0) return;
    const int step = delta > 0 ? 1 : -1;
    // 从当前项出发一步一步找下一个可用项，最多走 n 步。
    //
    // **不按 `delta` 的绝对值一次跳过去**：中间可能有禁用项，跳过去正好落在
    // 一个禁用项上，然后要么卡住、要么再补一次搜索——两种写法都比「一步一步」
    // 更难说清行为。这里的 n 是个位数，效率不是问题。
    int at = selected_ < 0 ? (step > 0 ? -1 : 0) : selected_;
    for (int k = 0; k < n; ++k) {
        at = ((at + step) % n + n) % n;   // 环绕；C++ 的 % 对负数向零取整，故补一次
        if (items_[static_cast<std::size_t>(at)].enabled) {
            selected_ = at;
            return;
        }
    }
    // 一项可用的都没有：保持原样（可能仍是 -1）。
}

bool MenuModel::point_at(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) return false;
    if (!items_[static_cast<std::size_t>(index)].enabled) return false;
    if (selected_ == index) return false;
    selected_ = index;
    return true;
}

MenuAction MenuModel::confirm() const noexcept { return action_at(selected_); }

MenuAction MenuModel::action_at(int index) const noexcept {
    if (index < 0 || index >= static_cast<int>(items_.size())) return MenuAction::None;
    const MenuItem& it = items_[static_cast<std::size_t>(index)];
    return it.enabled ? it.action : MenuAction::None;
}

// ——各屏的条目表——
//
// 文案用词的两条纪律：
//
//   * **动词开头、说清后果**：「返回主菜单」而不是「主菜单」——后者读不出
//     「当前这局会被丢掉」。
//   * 与 HUD 用同一套词：暂停屏的「继续对局」对应 HUD 上的「已暂停」。

std::vector<MenuItem> main_menu_items(bool can_resume) {
    return {
        MenuItem{MenuAction::StartNew, "开始新对局", true},
        // 首次启动时它是灰的，**但仍然在**（见 `MenuItem::enabled` 的注释）。
        MenuItem{MenuAction::Resume, "继续对局", can_resume},
        MenuItem{MenuAction::Help, "操作说明", true},
        MenuItem{MenuAction::Quit, "退出游戏", true},
    };
}

std::vector<MenuItem> pause_menu_items() {
    return {
        MenuItem{MenuAction::Resume, "继续对局", true},
        MenuItem{MenuAction::Restart, "重新开始", true},
        MenuItem{MenuAction::Help, "操作说明", true},
        MenuItem{MenuAction::ToMain, "返回主菜单", true},
        MenuItem{MenuAction::Quit, "退出游戏", true},
    };
}

std::vector<MenuItem> defeat_menu_items() {
    // 败局屏**不给「继续对局」**：那一局已经结束了，`DemoBattle` 也定格了
    //（败局后 update 不再推进，见 `demo_driver.hpp`）。给一个按了不动的选项
    // 比不给更糟。
    return {
        MenuItem{MenuAction::Restart, "重新开始", true},
        MenuItem{MenuAction::ToMain, "返回主菜单", true},
        MenuItem{MenuAction::Quit, "退出游戏", true},
    };
}

std::vector<MenuItem> help_menu_items() {
    return {MenuItem{MenuAction::Back, "返回", true}};
}

const std::vector<HelpEntry>& help_entries() {
    // 这份表就是**唯一**的操作说明。按键绑定改了要改这里，而它会同时改到
    // 说明面板与字体码点集合——不存在「改了绑定忘了改说明」的第二处。
    //
    // 顺序按「玩起来的先后」：先框选与下令，再点目标弹菜单，再镜头。
    //
    // **框选取代了「先按数字键选编队、再右键」**（#57 重开，理由与取舍见
    // `CLAUDE.md`「框选决定」）：编队号不再是操作的前提，只在征兵弹窗里
    // 出现一次（选新兵进哪支编队——编队仍然只在训练时决定一次，往后不变）。
    // 建造 / 征兵 / 维修也不再是「先进模式再点」，改成「先点目标，弹出选项」
    // ——不需要记 B/T/R 三个键，也不需要按 TAB 在类型间循环。
    //
    // 「升级」并进同一批入口：点己方任何完工建筑都有这一行，**顶到等级上限
    // 时它照样出现、灰着并说明原因**。这条要写进说明是因为它连着一句玩家
    // 不可能自己猜到的规则——建筑的等级上限由堡垒等级给出，所以「想升塔，
    // 先升堡垒」。`Keep` 自己不受这条上限约束。
    static const std::vector<HelpEntry> kEntries = {
        {"左键拖拽", "框选：矩形内的己方单位进入选中集"},
        {"左键点空地（可建造）", "弹出建造菜单，点一种建筑即放置"},
        {"左键点兵营 / 堡垒", "弹出征兵菜单；掉血多一行维修，另有一行升级"},
        {"左键点其他己方建筑", "弹出菜单：升级，以及掉了血时的维修"},
        {"建筑等级上限", "由堡垒等级给出——塔升不动时先升堡垒。堡垒自己无上限"},
        {"左键点其他", "取消当前选中集"},
        {"右键点一格", "对选中集下令：点墙段=驻守，点障碍=清野，点其余=开拔"},
        {"右键（菜单开着时）", "关闭菜单，不生效"},
        {"N", "提前召唤下一波，不等建造倒计时"},
        {"空格", "暂停 / 继续仿真"},
        {"方向键 / WASD", "平移镜头"},
        {"滚轮 / 中键", "缩放 / 拖拽镜头"},
        {"F", "把整张地图重新装进画面"},
        {"Esc", "暂停菜单"},
        {"目标", "守住领主堡垒。它被拆即本局结束，看的是撑过几波"},
    };
    return kEntries;
}

const std::vector<std::string_view>& all_menu_strings() {
    static const std::vector<std::string_view> kAll = [] {
        std::vector<std::string_view> out;
        // **机械地**把每个 builder 的产物遍历一遍。`main_menu_items` 两种入参
        // 都走一次——文案不随它变，但这一行写下来是为了让「哪天它变了」
        // 也不必回头改这里。
        const auto take = [&out](const std::vector<MenuItem>& items) {
            for (const MenuItem& it : items) out.push_back(it.label);
        };
        take(main_menu_items(true));
        take(main_menu_items(false));
        take(pause_menu_items());
        take(defeat_menu_items());
        take(help_menu_items());
        for (const HelpEntry& e : help_entries()) {
            out.push_back(e.keys);
            out.push_back(e.what);
        }
        return out;
    }();
    return kAll;
}

}  // namespace game
