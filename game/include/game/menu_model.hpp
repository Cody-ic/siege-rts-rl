// 菜单的**模型**：有哪几项、哪一项被选中、键鼠怎么改选中、确认了等于什么动作。
//
// ## 为什么菜单会有一个「不含像素」的层
//
// 与 `player_input.hpp` 同一条理由，而且这里更值得：菜单的 bug 全是**语义**的
// ——「禁用项能被选中」「按下键跳过了两项」「环绕到头之后卡住」「确认后走错屏」。
// 这些在默认构建里可测（GCC 侧也验），而 `render/` 默认不配置、挂在它下面的测试
// 在默认构建里根本不存在、ctest 照样全绿（见 `render/CMakeLists.txt` 开头）。
//
// 于是分工是：本层回答「菜单现在是什么状态」，`render::MenuView` 回答
// 「它画在屏幕的哪些像素上、鼠标落在第几项」。**版面属于后者**——两者靠同一份
// 布局函数保持一致（见那个头）。
//
// ## 文案在这里，登记也在这里
//
// 条目文案是中文，而中文字符必须**先登记进字体的码点集合**才画得出来
// （漏登记会当场抛，见 `render/text.hpp` 的第 2 个坑）。所以本文件同时提供
// `all_menu_strings()`，而它是**机械地**把下面每个 builder 的产物遍历出来的
// ——不是手抄一份清单。手抄的那种迟早漏，而漏掉的症状是「按 H 打开说明就崩」。

#ifndef GAME_MENU_MODEL_HPP
#define GAME_MENU_MODEL_HPP

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace game {

// 菜单项被确认之后要发生什么。**这一层只说「要什么」，不说「怎么做」**
// ——怎么做归 `GameShell::apply()`，那里才知道有没有对局在跑。
enum class MenuAction : std::uint8_t {
    None,       // 没有可确认的项（空菜单、或当前项被禁用）
    StartNew,   // 开始新对局
    Resume,     // 回到已有对局
    Restart,    // 丢掉当前对局，从第 1 波重来
    Help,       // 打开操作说明
    Back,       // 从操作说明返回来处
    ToMain,     // 回主菜单（丢掉当前对局）
    Quit,       // 退出程序
    Save,       // 由前端持久化当前完整对局
};

struct MenuItem {
    MenuAction action = MenuAction::None;
    std::string_view label;
    // 禁用项**仍然显示**（灰掉），不是隐藏。理由是稳定性：菜单项的位置不随
    // 状态跳动，「继续对局」在第一次启动时灰着，玩家由此知道它存在。
    bool enabled = true;
};

// 一屏菜单的选中状态与导航。
class MenuModel {
public:
    MenuModel() = default;
    explicit MenuModel(std::vector<MenuItem> items) { reset(std::move(items)); }

    // 换一批条目，选中项落在**第一个可用项**上（全不可用则 -1）。
    void reset(std::vector<MenuItem> items);

    const std::vector<MenuItem>& items() const noexcept { return items_; }
    bool empty() const noexcept { return items_.empty(); }
    // -1 表示没有任何可选项。**不要用 0 表示「没有」**：0 是一个合法下标，
    // 两者混同的症状是空菜单上按回车触发了第一项。
    int selected() const noexcept { return selected_; }

    // 上下移动 `delta` 步，**跳过禁用项、到头环绕**。没有可用项时什么都不做。
    void move(int delta);

    // 鼠标指到第 `index` 项（-1 = 没指到任何项）。禁用项不改选中——
    // 否则鼠标扫过灰项时选中框会跟着跳到一个按了没反应的地方。
    // 返回选中项是否真的变了（给「换项时播个音」之类留的钩子，现在没人用）。
    bool point_at(int index);

    // 当前项的动作。空菜单或当前项禁用时是 `None`（**不是抛**：确认一个
    // 灰项是完全正常的用户操作，不是程序错误）。
    MenuAction confirm() const noexcept;

    // 第 `index` 项的动作。鼠标点击走这条（点哪项就是哪项，不经选中）。
    // 越界或禁用时 `None`。
    MenuAction action_at(int index) const noexcept;

private:
    std::vector<MenuItem> items_;
    int selected_ = -1;
};

// ——各屏的条目表——
//
// 做成自由函数而不是 `GameShell` 的私有成员：`all_menu_strings()` 要能把它们
// 全部遍历一遍才能把文案机械地收齐，而那是字体码点集合的输入。
std::vector<MenuItem> main_menu_items(bool can_resume);
std::vector<MenuItem> pause_menu_items();
std::vector<MenuItem> defeat_menu_items();
std::vector<MenuItem> help_menu_items();

// 操作说明的一行：左边按键、右边中文。
//
// **拆成两栏而不是一条串**：一条串里靠空格对齐在比例字体下对不齐，
// 而对不齐的操作说明恰好是「看着像没写好」的那种界面。
struct HelpEntry {
    std::string_view keys;
    std::string_view what;
};

const std::vector<HelpEntry>& help_entries();

// 菜单与说明会显示的**全部**串。前端用它推导字体码点集合。
//
// 机械地由上面四个 builder 与 `help_entries()` 得出。加一项菜单、改一句说明
// 都不需要动这里——而「不需要动」正是它存在的意义（见文件头）。
const std::vector<std::string_view>& all_menu_strings();

}  // namespace game

#endif  // GAME_MENU_MODEL_HPP
