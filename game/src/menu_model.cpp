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
        MenuItem{MenuAction::Guide, "图鉴", true},
        MenuItem{MenuAction::Quit, "退出游戏", true},
    };
}

std::vector<MenuItem> pause_menu_items() {
    return {
        MenuItem{MenuAction::Resume, "继续对局", true},
        MenuItem{MenuAction::Save, "保存对局", true},
        MenuItem{MenuAction::Restart, "重新开始", true},
        MenuItem{MenuAction::Help, "操作说明", true},
        MenuItem{MenuAction::Guide, "图鉴", true},
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
    // **士兵是自主的**（2026-09 编队系统移除）：弓手自会找墙登墙、枪卫堵口
    // 不追、游骑避骑士摸攻城锤、工匠自找活干、斥候自动巡逻——玩家不必也不
    // 能再编队。框选 + 右键仍在，但它是**辅助性的临时指令**（开拔 / 驻墙，
    // 到达即失效、回归自主），不是常态操作。
    //
    // 建造 / 征兵 / 维修是「先点目标，弹出选项」
    // ——不需要记 B/T/R 三个键，也不需要按 TAB 在类型间循环。
    //
    // 「升级」并进同一批入口：点己方任何完工建筑都有这一行，**顶到等级上限
    // 时它照样出现、灰着并说明原因**。这条要写进说明是因为它连着一句玩家
    // 不可能自己猜到的规则——建筑的等级上限由堡垒等级给出，所以「想升塔，
    // 先升堡垒」。`Keep` 自己不受这条上限约束。
    //
    // 兵种等级上限（守方升级轴第三个输出）落地后加了一条：`[`/`]` 调征兵
    // 等级——持久值，只在征兵弹窗里用得上（`render/` 侧的状态）。
    static const std::vector<HelpEntry> kEntries = {
        {"M", "开启或关闭声音；暂停与阅读日记时停止战场音效"},
        {"士兵是自主的", "弓手自会登墙、枪卫堵口、游骑袭攻城锤、工匠找活、斥候巡逻"},
        {"左键拖拽", "空地起手框选；从墙上士兵起手则拖到目标格下墙参战"},
        {"右键点一格", "给选中集下临时指令：点墙段=驻墙，点障碍=清野，点其余=开拔；到达后回归自主"},
        {"Shift + 右键点工地", "选中工匠强制抢修该建筑，忽略危险；橙色标记表示强制中，完工或目标消失后恢复自主"},
        {"B / 底部建造按钮", "进入地面放置模式；Tab 切换建筑，左键连续放置，右键或 Esc 退出"},
        {"放置模式拖拽", "墙与木栅沿主轴铺线，Shift 换轴；绿可建、红无效、灰库存不足，只提交买得起的段"},
        {"左键点空地（可建造）", "弹出建造菜单，选择建筑后再点地面放置；高建筑不会挡住地面拾取"},
        {"左键点兵营 / 堡垒",
         "弹出征兵菜单；掉血多一行维修，另有升级与拆除"},
        {"左键点其他己方建筑", "弹出菜单：升级、拆除，以及掉了血时的维修"},
        {"重叠建筑连续点击", "同一位置再次点击切换建筑；移开鼠标后重新从最前面的建筑选择"},
        {"左键点施工中的建筑", "取消施工并全额返还石材与木材"},
        {"建筑等级上限", "由堡垒等级给出——塔升不动时先升堡垒。堡垒自己无上限"},
        {"[ / ]", "调征兵等级（1..兵种等级上限，同样由堡垒等级给出）"},
        {"左键点其他", "取消当前选中集"},
        {"右键（菜单开着时）", "关闭菜单，不生效"},
        {"N", "提前召唤下一波，不等建造倒计时"},
        {"斥候侦查", "斥候到了集结点会掷一次死活：活着拿回本波编成，死了本波编成不明"},
        {"幽影窥使", "敌侦查机只在你的视野里现形；瞭望塔看得最远，想提前发现就造它"},
        {"空格 / 底部按钮", "暂停 / 继续仿真；底部按钮也可回到堡垒或查看全图"},
        {"J / 日记按钮", "每10波自动暂停展示新剧情；新局重新解锁，第70波选择结局"},
        {"选中箭楼 / 弩楼", "显示实际射程边界：箭楼对地，蔽空弩楼仅对空"},
        {"Alt + F12", "开启开发者模式：资源、人口无限，可查看全部剧情与结局；输入波数并回车，Esc 关闭面板；本局不保存，重新开局退出"},
        {"窥使情报", "窥使成功后显示提醒；本波编队增减来自上一波情报，侦查不到就沿用旧情报"},
        {"保存 / F5", "每波、每分钟和退出时自动保存；暂停菜单可手动保存，下次启动继续对局"},
        {"V", "切换黄昏氛围与原始画面，不影响战斗规则"},
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
