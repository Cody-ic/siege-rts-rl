#pragma once

// 守方的**宏观决策层**（脚本版）：把「玩家会下的那些指令」变成代码。
//
// ## 为什么必须有它
//
// 全仓唯一产出 `Build`/`Train`/`Upgrade`/`Repair` 的地方是
// `game/src/player_input.cpp`——也就是**人点鼠标**。于是无渲染跑 `DemoBattle`
// 的时候守方**不封缺口、不建塔、不征兵、不升级**，只有 `DefenderScript` 在发
// 单兵动作。`tools/calibration_runner` 首批 36 局「全部陷落、中位第 2 波、
// 33/36 局攻方直接从设计缺口走进城」量到的，其实是**「守方什么都不做」**，
// 不是数值失衡（`攻守配平的数学模型.md` §8.1）。
//
// 配平要在仿真上做（那是唯一能覆盖全部变量的模型），而仿真里缺的就是这一层。
//
// ## 它是什么，不是什么
//
// **是** `守方AI与协同演化.md` 里那个「守方 RL 只在决策层」的**脚本替身**：
// 动作面严格等于玩家能下的宏观命令，一条不多。它同时是 `AI vs AI` 演示与
// 平衡测试的守方，也是攻方 RL 的陪练里「宏观那一半」的下限。
//
// **不是**单兵微操——那归 `game::DefenderScript`（弓手自动登墙、枪卫堵口、
// 游骑摸攻城锤……）。两层的分界与 CLAUDE.md「守方 RL 控制的是玩家会下的那些
// 指令，守方单兵由参数化脚本驱动」逐字一致。
//
// **不是**最优策略。它是一族可调策略里的一个点：`MacroParams` 全是旋钮，
// 配平搜索要在这些旋钮**与**数值表、地图、攻方曲线上一起搜。所以这里的取值
// 一律是占位值，判据写在注释里而不是写死在数字里。
//
// ## 三条实现纪律
//
// 1. **只读 `const World&`，只吐 `rts::Command`。** 不碰 `World` 的任何写接口，
//    命令一律经 `DemoBattle::submit_defender` 入队——与人类玩家同一条路。
//    于是它天然进回放、天然可复现，也天然不可能做出玩家做不到的事。
// 2. **同一轮决策里自己记账。** `submit` 只入队、要下一次 `advance()` 才结算，
//    所以一轮里连下两条 `Build` 时第二条看到的库存**还是旧的**。不自己扣就会
//    超发，症状是「命令被静默拒绝」（`World` 不会报错，只是不执行）。
// 3. **遍历顺序即规范。** 候选格按预先算好的固定序扫（构造时定，不随 tick 变），
//    不用任何未定序容器驱动——`determinism_bans` 扫 `game/`，但那条守卫拦得住
//    `unordered_map` 拦不住「按 hp 排序时 hp 相等」这类不稳定序。

#include <cstdint>
#include <vector>

#include "game/map_data.hpp"
#include "rts/command.hpp"
#include "rts/roster.hpp"
#include "rts/types.hpp"
#include "rts/world.hpp"

namespace game {

// 一族策略的参数。**全部是占位值**，取值归配平搜索（见文件头第 3 条纪律）。
struct MacroParams {
    // ——结构性开关（不是数值旋钮）——

    // 第一件事：把地图自带的设计缺口砌上。**它不是「优化」，是前提**——
    // 不封的话攻方直接走进城，门前攻防整段不发生（runner 首批 33/36 局如此）。
    bool seal_gaps = true;

    // 把弓手的驻守意愿集中到受威胁那一面。免费情报（`strongest_spawn`）已经
    // 给出方向，所以这**不是作弊**；而「把弓手拖到主攻门」是杂活不是战术，
    // 正合「守方单兵全自主、玩家只下宏观命令」那条设计。
    // `攻守配平的数学模型.md` §5.3 实测它值 4–6 倍，是全场最大的单一杠杆。
    bool concentrate_archers = true;

    // **敌人进城就把弓手叫下墙去堵**。
    //
    // 这一条是被实测逼出来的，而且它订正了我自己的一个错误结论：城破那几局
    // 波末守方是「9 名弓手全活着、2 名枪卫全死、堡垒掉了」——弓手在墙上，
    // 城破在墙下。我一度把它读成「内城没有守方参与者、缺第二道线」，
    // **那是错的**：下墙的机制早就有（意愿清空 ⇒ 世界放人），玩家也早就能手动
    // 触发（框选 + 右键点空地，`DefenderScript::issue_move_order` 的注释原文：
    // 「对已上墙的单位同时是『下来』」）。缺的只是**脚本会不会用这一手**。
    //
    // 而本层的定义就是「玩家会下的那些指令」，所以它必须会。不会的话，
    // 量出来的陷落波读的是「不会拉弓手下墙的守方」，不是数值。
    //
    // 弓手自主逻辑里没有下墙的触发条件（`decide_unit` 里已上墙的弓手每拍
    // 无条件把意愿写回去，「弓手留任」），那属单兵层、另一条线；本层用玩家
    // 动作（开拔指令）绕过它，两边不冲突。
    bool defend_breach = true;

    // 城内出现几个攻方单位才拉弓手下墙。1 = 见一个就下。**它是真旋钮**：
    // 太小会为一个残血 Ghoul 把整条墙放空，太大则等到守不住才动。
    int breach_intruders_min = 1;

    // 人口咬住时招高级兵而不是多招人。由 `c ∝ √B(L)` 推出：每金买到的战力与
    // 等级无关，但一名 L 级兵占 1 人口却顶 √B(L) 名 1 级兵（同上 §3.2）。
    bool train_at_cap_level = true;

    // ——分配——
    //
    // **两个大额支出都不走「按库存分成」，走「限额」**：这是实测订正过两次的
    // 地方。按千分比分成时，塔在每一轮把库存抽干，于是分给堡垒的那一份永远
    // 到不了 200（一次升级的价钱），分给采集建筑的那一份永远到不了 40
    // （一座采石场）——实测 `upg=0 gath=0`，两个输出全程是装饰品。
    // 大额、低频的支出要靠**预留/限额**，不能靠比例。
    //
    // 堡垒：只在「人口顶满」或「已有建筑顶到等级上限」时才预留升级钱（结构判据，
    // 不是比例），见 .cpp。
    // 采集建筑：每波最多铺几座。它单座 15 秒回本，早铺早复利，所以正确的形状是
    // 「限速」而不是「限钱」；上限本身是**真旋钮**——全押经济会让门前没有塔，
    // `攻守配平的数学模型.md` §3.3 实测过那一头也会更早死。
    int gatherers_per_wave = 2;

    // **先铺哪一种资源点。**
    //
    // 2026-09-03 实测（`资源点分布与经济平衡.md` §4.5）：三种资源里**只有石材
    // 是紧的**——木材在 82% 的波里净增长、金币被人口上限掐死，而石材有 57% 的
    // 波在净减少。所以「先铺哪一种」不是无关紧要的顺序问题，它直接决定了
    // 新增收入落在活的那条轴上还是死的那两条上。
    //
    // `MapOrder` 是这一层最早的行为：**按资源点在地图文件里的顺序**铺。
    // 那既不看种类也不看距离，纯属没想过——留着只是为了让旧测量可复现。
    enum class GatherOrder {
        MapOrder,      // 地图文件顺序（历史行为，无取舍可言）
        NearestFirst,  // 离堡垒近的先铺（工匠往返短 = 早回本）
        StoneFirst,    // 石材优先，同种类再按距离
        GoldFirst,     // 金币优先，同上。人口上限抬高之后金币才有 sink，
                       // 两件事要一起测（`资源点分布与经济平衡.md` §4.5）
    };
    GatherOrder gather_order = GatherOrder::MapOrder;


    // ——边界——

    // 工匠往返得了的城外距离上界（格）。工匠 0.08 格/tick ⇒ 40 格往返 100 s，
    // 已经比一波还长；外环带（≥55 格）往返 74–78 s 且要穿集结点环，
    // 实测那 37 个点人类吃不到（`资源点分布与经济平衡.md` §2.2）。
    int gatherer_max_dist = 40;

    // 血量掉到这个千分比以下就修。维修只要木材且远便宜于重建，所以门槛可以低；
    // 但**不设成 1000**：满血也发 `Repair` 会让命令队列里全是无效命令。
    int repair_hp_permille = 900;

    // 每轮决策最多下几条命令。防的是「一轮里把整波的钱一次花光、之后无法应变」，
    // 同时让每轮的记账误差有界。
    int max_commands_per_decision = 12;

    // 塔的上限（每个入口）。**不是平衡旋钮，是防跑飞的护栏**——配平要动的是
    // 塔造价与损耗率，不是这个数。
    int max_towers_per_entry = 64;

    // **先给堡垒配几座近卫塔。**
    //
    // 这一条是几何逼出来的：`Tower` 射程 7，而城区半径 9–10 ⇒ **堡垒周围有一
    // 整圈谁都覆盖不到**。摆在环内侧的塔（离堡垒 8–9 格）打不到堡垒脚下，
    // 墙上的弓手（射程 6.5）也打不到。实测后果：攻方破门进城后一路走到堡垒
    // 脚下无人可挡，6 只 1 级亡灵步兵破 520 血的门要 24 秒、再砸 2400 血的
    // 堡垒要 112 秒，全程零抵抗——第 1 波就能这么赢。
    //
    // 这也解释了两件之前读不通的事：把弓手叫下墙**中性**（它们下来也走不过去、
    // 也打不过），压平攻方兵力曲线**几乎无效**（alpha 1.25 → 0.40 只把中位陷落
    // 波从 3 挪到 3–4）。**堵不住的不是火力不够，是那一圈没有火力。**
    int keep_guard_towers = 2;

    // **环上没地方摆新塔了就改升级已有的。**
    //
    // 这一条补的是「机制落地了但没人用」——`CommandKind::Upgrade` 与
    // `building_level_cap()` 都在，而本层此前只升堡垒、**一次都没升过别的建筑**，
    // 于是「堡垒等级 → 建筑等级上限」这个输出在仿真里从未被消费过
    // （同 CLAUDE.md 记的那次「升级落地了但玩家点不到」）。
    //
    // 顺序是**先铺开、再升高**，不是随便定的：2026-09-03 重定价之后
    // 每石买到的火力与等级无关（`World::bld_upgrade_cost_stone()`），
    // 于是石材上两者等价、差别在别处——新塔**多覆盖一片墙**，升级只是把
    // 同一片打得更疼。所以有空位时新建更值，空位耗尽后升级是唯一出口。
    // （反过来的证据也在：`balance_solver.py upgrade` 里少数强塔在入口赛跑
    // 上赢得比 √ 更快，但那是「同一个门口」的比较，不含覆盖面。）
    bool upgrade_when_saturated = true;
};

// 一次决策要下的**辅助性临时指令**（到达即失效、回归自主），不进
// `World`/`Command`/回放。两种形态共用一个结构，因为它们在 `DefenderScript`
// 里本来就是同一条通道的两个取值（`ManualOrder::garrison`）。
struct UnitOrder {
    std::vector<rts::UnitId> ids;
    rts::GridPos target{};
    // true = 驻进 `target` 那段墙（`issue_garrison_order`）；
    // false = 开拔到 `target`（`issue_move_order`）——**对已上墙的单位就是「下来」**。
    bool garrison = false;
};

// 累计统计，给 runner 报告用（也用来判「这一族参数到底做了什么」）。
struct MacroStats {
    int walls_built = 0;
    int towers_built = 0;
    int gatherers_built = 0;
    int units_trained = 0;
    int upgrades = 0;       // 升堡垒（那条轴的闸门）
    int bld_upgrades = 0;   // 升其余建筑（塔 / 受威胁那一面的墙）
    // 受威胁墙段一个射程以内还剩几个空塔位。**诊断用**：它是 6c「该改升级了」
    // 的判据，而那个判据一旦永远 > 0，升级那一段就是死代码（整环 88 个位子
    // 正是这么被否掉的，见 .cpp）。
    int near_free_spots = 0;
    int repairs = 0;
    int garrison_wishes = 0;
    int breach_recalls = 0;   // 「把弓手叫下墙」下过几次
};

class DefenderMacro {
public:
    DefenderMacro(const MapData& map, const MacroParams& params = {});

    // 一个决策周期调用一次。**追加**到两个输出里（不清空——调用方可能在攒批）。
    //
    // `w` 只读；`cmds` 交给 `DemoBattle::submit_defender`；
    // `wishes` 交给 `DemoBattle::issue_garrison_order`（每拍重发才有效，
    // 那是意愿通道的既有语义，见 `game/defender_script.hpp`）。
    void decide(const rts::World& w, std::vector<rts::Command>& cmds,
                std::vector<UnitOrder>& orders);

    const MacroStats& stats() const noexcept { return stats_; }

    // 环上没写墙段的格（设计缺口）。构造时从 `MapData` 算好，固定序。
    const std::vector<rts::GridPos>& gaps() const noexcept { return gaps_; }
    int ring_radius() const noexcept { return ring_r_; }

private:
    MacroParams p_;
    MacroStats stats_{};

    rts::GridPos keep_{};
    int ring_r_ = 0;
    int map_w_ = 0;
    int map_h_ = 0;

    // 预先算好的固定序候选表（构造时定，不随 tick 变——纪律 3）。
    std::vector<rts::GridPos> gaps_;         // 环上缺墙的格
    std::vector<rts::GridPos> wall_cells_;   // 环上有墙/门的格（驻守目标从这里选）
    std::vector<rts::GridPos> tower_spots_;       // 环内侧一格的候选塔位
    std::vector<rts::GridPos> keep_guard_spots_;  // 离堡垒 4–6 格的候选塔位

    // 「每波最多铺几座采集建筑」的记账。波号变了就清零。
    int last_wave_ = -1;
    int gath_this_wave_ = 0;
};

}   // namespace game
