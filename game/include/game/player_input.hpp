// 玩家输入的**语义层**：把「点了哪一格」翻成命令（`game/` 管输入，CLAUDE.md
// 架构表），render/ 只负责拾取（screen_to_grid）与绘制（虚影、高亮）。
//
// 放在 game/ 而不是 render/ 的理由与 BattleScene 相同：这一层不含像素、
// 且要在默认构建里被测——「点墙下的是驻守不是开拔」是一条真断言，GCC 侧也验。
//
// ## 右键的语义（2026-09 起）：一个键，按格上的东西分三类
//
//   * 己方**完工**的墙 / 门 → 驻墙（`ClickTarget::Wall`——落成框选单位的
//     临时驻墙指令，走 `DefenderScript::issue_garrison_order`，**不是命令**）
//   * 活着的障碍 → `Clear`（清野——这是唯一还走 `World::submit` 的逐格命令）
//   * 其余 → 开拔（`ClickTarget::Ground`——落成临时开拔指令，走
//     `DefenderScript::issue_move_order`，同样不是命令）
//
// 编队系统移除后单兵全自主，右键是**辅助性覆盖**：它不改变任何持久状态，
// 到达即失效（`game/defender_script.hpp` 文件头）。所以这里只做「判语义」，
// 两种单兵指令都不再包装成 `rts::Command`。
//
// ## 三种「点一格生效」的模式共用一套形状
//
// 建造 / 征兵 / 维修都是「先选模式，再点一格」。三者各有一个 `*_hint()`
// （虚影该绿还是红）与一个 `*_command()`（点下去发什么命令）。
//
// **hint 是提示，不是裁决。** 真正的裁决在 `World` 的解算里——它拒绝时是
// **静默**丢弃（`break`，什么都不说），所以 hint 骗一次，玩家看到的就是
// 「点了没反应」而查不出原因。两层刻意不共用代码（hint 错了顶多虚影颜色骗
// 一下，命令该拒还是拒），但 hint 也**不能只查一半**——凡是前端查得动的
// 规则都要查，资源点归属（结构规则）与**造价够不够**（数值规则）是**同一条
// 要求的两个来源**，不能只补结构那一半。
//
// 造价查询与结构查询**分成两个函数**（`can_place_hint`/`can_afford_build`
// 这一对，`can_train_hint`/`can_afford_train`，`can_repair_hint`/
// `can_afford_repair`），理由是弹窗一次列出**好几个选项**、每项造价不同
// （建几种建筑、征哪个兵种），而「这一格能不能弹出菜单」在还不知道玩家会
// 选哪一项时就要判断——两者粒度不同，压成一个函数会把其中一层判断丢掉。
// 调用方（`render/` 的弹窗）对每一项把两者取 **AND** 才是完整的绿框判据；
// 只查结构不查造价，绿框在钱不够时仍然亮着，就是这一层曾经的 bug（一次
// 试玩报出来的：招兵/建筑/维修弹窗只查了位置合法性，没查资源够不够）。

#ifndef GAME_PLAYER_INPUT_HPP
#define GAME_PLAYER_INPUT_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "game/iso_projection.hpp"
#include "rts/command.hpp"
#include "rts/roster.hpp"
#include <array>

#include "rts/types.hpp"
#include "rts/world_view.hpp"

namespace game {

// 右键点一格，格上是什么。`cell` 必须在界内（越界归拾取层挡，这里 assert）。
// 调用方（`render/`）按它分流：Wall → 框选单位的临时驻墙指令；
// Obstacle → `clear_command` 走 `World::submit`；Ground → 临时开拔指令；
// None → 什么也不做。
enum class ClickTarget : int { None = 0, Wall, Obstacle, Ground };
ClickTarget classify_click(const rts::WorldView& view, rts::GridPos cell);

// 清野命令（右键语义里唯一还走命令通道的一种）。点在活着的障碍上才有意义，
// 调用方先经 `classify_click` 判过；这里不再复查（命令解算层会静默拒）。
rts::Command clear_command(rts::GridPos cell, int map_width);

// 框选：`ids` 里屏幕位置落在 `rect`（像素，宽高可为负——鼠标可以往任何
// 方向拖）内的单位。纯几何判断，不读单位状态；选出的是**单位**而不是任何
// 归属概念——编队移除后框选的产物就是一批 UnitId，直接喂给
// `DefenderScript::issue_move_order`/`issue_garrison_order`。
std::vector<rts::UnitId> units_in_rect(const rts::WorldView& view,
                                       std::span<const rts::UnitId> ids,
                                       const IsoProjection& proj, Rect rect);

// ——建造——
//
// 建造面板上的建筑清单，**顺序就是 TAB 的循环顺序**。放在 `game/` 而不是
// `render/` 的理由与本文件其余部分相同：它要被测（`Keep` 不可在列——
// 「不可再建」是结构，见 `World` 的 Build 解算；采集三座必须在列——
// 少了它们石木两种资源永远没有收入，而那不是「少一个入口」，
// 是**整条经济轴不通**）。
const std::vector<rts::BldType>& buildable_types();

// 建造虚影的合法性**提示**（绿 / 红）。查四条：地形可建、格上没有建筑、
// 没有障碍、**资源点归属**（采集建筑必须落在对应种类的资源点上，其余建筑
// 不得占资源点——`World` 的 Build 解算里那条结构规则）。
//
// 第四条是后加的：少了它，玩家会在草地上看到一个绿框然后点出一个采石场，
// 而命令被静默拒绝。资源点在画面上没有专门的标记，所以这个绿框是唯一的线索。
bool can_place_hint(const rts::WorldView& view, rts::BldType bt, rts::GridPos cell);

// 造价够不够（石 + 木）。**与 `can_place_hint` 分开查**：结构合法性与买不买得起
// 是两件独立的事——弹窗里同时列着好几种建筑，每种造价不同，两者混在一起会把
// 「这一格能不能盖东西」与「这一种买不买得起」这两条不同粒度的判断压成一条。
// 调用方（弹窗）把它与 `can_place_hint` 取 AND 才是完整的绿框判据。
bool can_afford_build(const rts::WorldView& view, rts::BldType bt);

rts::Command build_command(rts::BldType bt, rts::GridPos cell, int map_width);

// ——征兵——
//
// 可征的兵种清单（守方全部五种），顺序即 TAB 的循环顺序。
const std::vector<rts::UnitType>& trainable_types();

// 这一格能不能出兵：**完工**的 `Barrack` 或 `Keep`，且没有正在练的。
// 「`Keep` 也行」不是宽松，是 CLAUDE.md 花名册里那条兜底（兵营可退化为
// 从堡垒出兵），机制侧同一条。**不查造价**——这一格能不能弹出征兵菜单，
// 与「菜单里哪个兵种买得起」是两个不同粒度的问题，后者见 `can_afford_train`。
bool can_train_hint(const rts::WorldView& view, rts::GridPos cell);

// 造价够不够（金），且 `level` 没有顶过 `unit_level_cap()`。同
// `can_afford_build` 的理由：弹窗里每个兵种一行，造价各不相同，只有知道
// 具体兵种（现在还要知道具体等级）才能判断买不买得起。`level` 是兵种
// 等级上限落地时追加的参数——顶不过上限直接算不够格，不单独开一个函数。
bool can_afford_train(const rts::WorldView& view, rts::UnitType ut, std::int32_t level);

// 人口满（守方）：存活守方单位 + 在训占位达到 `defender_pop_cap()`。
// 它是**全局**状态不是逐格属性，所以不进 `can_train_hint`（那个管「这格
// 能不能弹出菜单」）——满了菜单照样弹，选项灰掉并把原因印在标签里
// （同 `upgrade_block` 把「先升堡垒」印在升级行那条先例：「为什么不能做」
// 在画面上要有一个字）。机制侧 `World::apply_one` 的 `Train` 分支自己
// 也查这一条，这里是给玩家看的同一份答案。
bool train_pop_full(const rts::WorldView& view);

// 征兵命令。`level` 是兵种等级上限落地时追加的（1..`unit_level_cap()` 任选，
// 越高越贵——`view.train_cost_gold()` 查具体数额）。新兵不带任何归属概念：
// 编队系统移除后，出兵即自主行动（`game/defender_script.hpp`）。
rts::Command train_command(rts::UnitType u, std::int32_t level,
                           rts::GridPos cell, int map_width);

// ——维修——
//
// 这一格能不能修：己方**完工**的建筑、掉了血、且没有正在进行的工时
// （施工或上一次维修）。工地不「修」——工地是往前盖，那是 `Cancel` 的事。
// **不查造价**，理由同上——见 `can_afford_repair`。
bool can_repair_hint(const rts::WorldView& view, rts::GridPos cell);

// 造价够不够（木，按缺口血量折算——公式与 `World` 的 `Repair` 解算逐字相同：
// `ceil(缺口 × repair_wood_per_1000hp ÷ 1000)`，两处算法分叉是「绿框骗人」这
// 类 bug 的另一个来源，所以照抄，不重新推一遍）。
bool can_afford_repair(const rts::WorldView& view, rts::GridPos cell);

// 修这一格要花多少木材——`can_afford_repair` 内部就是算这个数再比大小，
// 单独露出来是因为**弹窗要把价钱印在「维修」两个字后面**：光有红绿框，
// 玩家看不出到底差多少（一次试玩报出来的：维修选项只有「维修」两个字，
// 不知道要花什么）。这一层与建造/征兵的价钱标签同一个来源，理由见
// `render/` 的弹窗代码。这一格无效或没有缺口时返回 0。
std::int64_t repair_wood_cost(const rts::WorldView& view, rts::GridPos cell);

rts::Command repair_command(rts::GridPos cell, int map_width);

// ——取消施工 / 拆除——
//
// 工地与成品建筑使用两条独立命令：工地取消固定全额返还基础造价；成品拆除
// 返还 `GlobalStats::demolish_refund_permille`（正式表为 800‰）。`Keep` 是失败
// 条件的锚点，不允许主动拆除。
bool can_cancel_build_hint(const rts::WorldView& view, rts::GridPos cell);
bool can_demolish_hint(const rts::WorldView& view, rts::GridPos cell);
rts::Command cancel_build_command(rts::GridPos cell, int map_width);
rts::Command demolish_command(rts::GridPos cell, int map_width);

// ——升级（建筑等级上限，守方升级轴第一个输出）——
//
// 这一格**为什么**不能升级。`can_upgrade_hint` 就是它 `== None`，两者
// 共用同一份判定、不各写一遍（同 `can_afford_repair` 内部调
// `repair_wood_cost` 的做法）。
//
// **露出理由而不只给一个 bool，是因为其中一种原因在画面上没有任何线索。**
// 血条看得见掉血、施工进度看得见在忙，而「这座建筑几级」与「当前上限是
// 几级」画面上一个字都没有。于是 `LevelCap` 那一种若只表现为「菜单里没
// 有升级这一行」，玩家不会知道存在升级这件事，更不会想到该先去升堡垒
// ——而占位系数 `building_level_cap_divisor = 2` 下，1 级堡垒的上限就是
// 1 级，**开局每一座建筑都是 `LevelCap`**。所以弹窗对己方完工建筑
// **恒显示**升级行（`!= NoBuilding` 即显示），靠这个理由说明为什么灰着。
enum class UpgradeBlock : int {
    None = 0,      // 能升（还要另查造价，见 `can_afford_upgrade`）
    NoBuilding,    // 这一格没有己方活着的建筑：升级行不该出现
    Unbuilt,       // 是工地：工地往前盖，不谈升级（同 `can_repair_hint`）
    Busy,          // 在建 / 在修 / 已经在升——同一时刻只能有一件工程在推进
    LevelCap,      // 顶到 `building_level_cap()`（`Keep` 永远不会是这个）
};
UpgradeBlock upgrade_block(const rts::WorldView& view, rts::GridPos cell);

// 这一格的建筑现在几级——弹窗要把 `Lv2 → 3` 印出来，理由同上（等级在
// 画面上没有别的来源）。这一格没有活着的建筑时返回 0。
std::int32_t bld_level_at(const rts::WorldView& view, rts::GridPos cell);

// 这一格能不能升级：己方**完工**建筑、没有在建/在修/在升、没顶到
// `WorldView::building_level_cap()`（`Keep` 本身不受这条上限约束——
// 「堡垒等级本身不设上限」，CLAUDE.md）。**不查造价**，理由同
// `can_repair_hint`。
bool can_upgrade_hint(const rts::WorldView& view, rts::GridPos cell);

// 升一级要花多少石/木——判定逻辑只在这里查一遍 `view.stats()`，不重新推
// `rts_core/src/world.cpp` 的 `Upgrade` 解算（同 `repair_wood_cost` 的纪律：
// 两处算法分叉是「绿框骗人」这类 bug 的来源）。这一格无效时返回 0。
std::int64_t upgrade_cost_stone(const rts::WorldView& view, rts::GridPos cell);
std::int64_t upgrade_cost_wood(const rts::WorldView& view, rts::GridPos cell);

// 造价够不够（石 + 木都要够）。同 `can_afford_build`/`can_afford_repair`
// 的理由：与 `can_upgrade_hint` 分开查，粒度不同。
bool can_afford_upgrade(const rts::WorldView& view, rts::GridPos cell);

rts::Command upgrade_command(rts::GridPos cell, int map_width);

// ——免费情报：主攻方向——

// 「攻方兵力最多的那个集结点」的下标；没有集结点、或一个攻方单位都没有时
// 返回 -1（调用方据此不显示这一行，而不是显示一个假方向）。
//
// **它刻意不过迷雾。** CLAUDE.md 的情报划分把「本波总兵力」与「大致方向」
// 列在**免费**那一列，要花钱侦查的是「编成构成」与「精确的分兵情况」；
// 而迷雾一开，前者若也没了，玩家就是全盲乱找——设计要的是**情报有起点**。
//
// **诚实但可被欺骗，这正是设计要的**：提示本身不撒谎，但它诚实的对象由攻方
// 控制——把佯攻堆得比主攻更多，这行提示就成了诱饵（CLAUDE.md「集结区」原文）。
//
// 判据是「离哪个集结点最近」而不是「从哪个集结点生出来的」：后者要在
// `World` 里给每个单位记一个出生点（动布局、动哈希），而前者用现成的数据
// 就能算，且在部队开拔之后仍然给出「他们现在从哪个方向压过来」——
// 那反而比出生地更贴近玩家想知道的东西。
int strongest_spawn(const rts::WorldView& view);

// ——需侦查的情报：来袭编成——

// 一种敌方单位，以及玩家**当前看得见**几个。
struct SightedType {
    rts::UnitType type{};
    int count = 0;
};

// 玩家当前看得见的敌方编成，按花名册顺序（`rts::unit_at`）排列。
//
// **过迷雾，与 `BattleScene::sorted` 用同一个判据**（`view.fog()`、
// 只算 `Vis::Visible`）——「编成构成」是 CLAUDE.md 情报划分里**需要侦查**的那一列，
// 所以这个读数必须只反映玩家真的看见的东西。它与 `strongest_spawn`
// （免费方向提示，**刻意不过迷雾**）正好是那张表的两侧，放在一起便于对照。
//
// 存在的理由：迷雾落地之后，玩家侦查到的编成**没有任何面板可读**，只能靠数
// 精灵。而 CLAUDE.md 要求「玩家必须能快速读懂来袭编成才能应对」，并明写
// 不透明会让「被 AI 针对」退化为「被系统坑」。
std::vector<SightedType> sighted_composition(const rts::WorldView& view);

// 敌方的幽影窥使当前是否在我方视野里。判据与 `sighted_composition` 逐字相同
// ——实现就是拿它的结果查一项，两处因此**不可能**各自漂移。
//
// 用途只有一个：HUD 顶中的「窥使入境」警报横幅（2026-09-03 试玩反馈「侦查
// 阶段感知不明显」）。窥使进视野的这段时间是玩家唯一的反制窗口——击落它，
// 敌 AI 这波就带不到新情报（`CLAUDE.md`「双向欺骗」）——而此前这个窗口只有
// 地图上一个不起眼的小精灵在「提醒」。
bool enemy_wraith_sighted(const rts::WorldView& view);

// 本波是否已接战：场上**任意一侧**有任意单位的攻击四位之一亮在动作掩码里
// （`AtkNear`/`AtkWeak`/`AtkBld`/`AtkWall`）。
//
// 用途是 HUD 阶段行的「敌袭迫近 / 交战」二分：开打（`WavePhase::Assault`）
// 之后大军还要行军几十秒，这期间旧文案「进攻中」既不准确也不给信息（同一条
// 2026-09-03 反馈）。
//
// 拿 `World&` 而不是 `WorldView`：精确的攻击四位挂在 `World::action_mask`
// 上，而枚举单位要 `World::enumerate_units`。**不过迷雾不泄露情报**——攻击
// 四位亮着，说明你的单位或建筑已在对手射程内，那样的接触本来就发生在你的
// 视野所及之处；且阶段行不是情报读数（它是全局事实，攻方也看得见）。
bool combat_engaged(const rts::World& w);

// 「这种敌人被谁克」——`CLAUDE.md`「克制二部图」那张图的玩家侧读数。
//
// **这不是一张 N×N 伤害倍率表**，CLAUDE.md 明确禁止那个（「克制关系由三条
// 正交属性轴推导，不要退化成平铺的 N×N 伤害倍率表」）。倍率仍然只从机制来
// （`anti_charge_permille`、`is_aerial`、`vs_structure_permille`、溅射半径……）；
// 这里给的是**设计意图的陈述**，用途是让玩家读得懂——而「克制必须在 UI 中
// 完全透明」同样是 CLAUDE.md 的明文要求，两条不矛盾：一条管数值从哪来，
// 一条管玩家能不能看见。
//
// 其中几条是**机制决定的、不是选择**，由 `tests/player_input_test.cpp` 与
// 数值表交叉核对（例如「`Spear` 克 `Knight`」要求 `anti_charge_permille > 1000`、
// 「只有 `Flak` 与墙上的 `Archer` 能打 `Phoenix`」要求 `Phoenix` 是空中单位）
// ——**表若与机制漂移，那条测试会红**，而画面上看不出来。
struct CounterHint {
    std::span<const rts::UnitType> units;
    std::span<const rts::BldType> blds;
};
CounterHint counters_of(rts::UnitType attacker) noexcept;

}  // namespace game

#endif  // GAME_PLAYER_INPUT_HPP
