// 守方单兵执行层：一族参数化脚本（README 未认领工作第 6 项，就此认领落地）。
//
// ## 它是最终形态，不是「RL 做好之前的临时凑合」
//
// 判据与理由整段在 `守方AI与协同演化.md` 2.5：攻方需要见到的是**覆盖真人
// 水平区间的一个分布**，而 RL 只会收敛到一个点——脚本的微操强度是可调参数，
// RL 的不是。攻方在全部训练阶段面对的执行层都是它。
//
// ## 2026-09 起：全自主为体，手动指令为辅
//
// 编队系统（`MoveForce` / `Garrison` 命令、编队归属）已整体移除。守方玩家
// （人类或决策层 RL）的**持久**手段只剩下宏观命令：建、修、拆、招、升、
// 召唤、清野——没有一个能指到具体单位头上。单兵因此必须自己拿主意，
// 「呆滞」（没人下令就站着）从设计上不再可能：每个兵种每个决策拍都有一条
// 自己的默认行为。
//
// 但人类玩家仍可以**框选 + 右键**给具体单位下一道临时指令（开拔到一格 /
// 驻进一段墙）——它是辅助性的覆盖：优先于自主默认，**到达即失效**，
// 单位随即回归自主行为。它不编队、不进 `World` 状态（脚本状态本来就不是
// 世界状态，回放存的是脚本由此选出的动作与意愿），也不阻止世界照旧按
// 登墙意愿解算——指令只是脚本决策时的一个高优先级输入。
//
// 逐条对照（同文 2.5 那张表，克制二部图里已写死的战术行为进脚本）：
//
//   * `Archer` **拉扯 + 自动驻墙**：近战威胁贴近就后撤拉开射程，威胁退出触发圈
//     就回头放箭；手上没仗打就找最近的空墙段登墙（高度优势是机制白给的，
//     弓手不上去就是浪费）——登墙意愿经 `submit_garrison_wishes` 通道提交，
//     登墙动作本身由 `World::tick_garrison` 解算
//   * `Spear` **守家迎击，拴堡垒不追远**（「Knight ──► Spear（开阔地）」）：
//     打得着就打；敌人进堡垒 `spear_engage_cells` 圈就主动出击迎击最近
//     来敌，出圈即收兵回驻防环——被风筝出阵位正是 Shade 克枪卫的机制，
//     圈就是那条风筝防线，脚本不能亲手送。没仗打时在堡垒周围的驻防环上站位
//   * `Ranger` **突袭而不对冲**（「Ram ──► Ranger 快速切入」+
//     「Ranger ──► Knight（骑兵对冲，遏制出城）」）：主动摸向敌方攻城锤；
//     骑士进到保持距离内就脱离，不与之对冲
//   * `Mason` **自动找活 + 任务认领**：优先挑「本拍还没别的工匠认领」的
//     最近任务（工地 / 维修点 / 升级工程），各管一个；任务数少于工匠数时才多人同
//     任务——同任务的加速在机制层（`World::mason_count` 线性加速）。
//     半径内工时才会走（机制第二批），「到了」的判据就是那个半径
//   * `Scout` **自动巡逻**：盯集结点——攻方从集结点来，盯着那里就是盯着
//     威胁的来路。目标选定后**承诺走到底**（看见为止，优先从未看过的），
//     不每拍重选——视野圈沿上迷雾逐拍闪，重选会让斥候原地踏步
//
// 包夹、佯攻、择机——图里没写的一概不在此处，那是攻方 RL 的涌现空间。
//
// ## 与仿真的关系：只读进、动作与登墙意愿出
//
// 输入是 `WorldView`（不变量 2，编译期只读），输出是两份与
// `enumerate_units(Defender)` 同序的数组：动作（给 `submit_actions`）与登墙
// 意愿（给 `submit_garrison_wishes`）。脚本自身的状态（参数、RNG、威胁计时）
// **不是世界状态**：回放存的是脚本选出的动作与意愿（契约 §4.6），所以换脚本、
// 换参数都不触碰回放口径。确定性：同种子 + 同输入序列 ⇒ 同输出序列
// （RNG 是 `rts::Rng`，掷点次序跟着规范单位序走）。

#ifndef GAME_DEFENDER_SCRIPT_HPP
#define GAME_DEFENDER_SCRIPT_HPP

#include <cstdint>
#include <span>
#include <vector>

#include "rts/action.hpp"
#include "rts/flow.hpp"
#include "rts/rng.hpp"
#include "rts/types.hpp"
#include "rts/world_view.hpp"

namespace game {

// 框选 + 右键下达的临时指令（辅助性覆盖，见文件头）。**逐单位、不进
// `World`**——它是脚本自己的状态，作用是在决策时压过自主默认一拍。
struct ManualOrder {
    bool active = false;
    rts::GridPos target{};
    // true = 驻进 target 那段墙（登墙意愿 + 走位一起给）；false = 开拔到
    // target（对已上墙的单位同时意味着「先下来」——意愿清空，世界放人）。
    bool garrison = false;
    // 配 `UnitId::generation()`：槽位被复用（旧单位死、新单位占了同一个
    // 下标）时旧指令必须视为失效，否则新单位会莫名其妙走向一个没人告诉过
    // 它的目标——`Handle` 的 tag/generation 机制原是防「拿地址/下标当 key」
    // 那类 bug，这里是它被 `game/` 自己的每单位状态复用，同一条纪律。
    std::uint16_t generation = 0;
    bool forced = false;
    std::uint32_t building = 0; // Generational identity, not just a cell/slot.
    bool upgrade = false;
};

// 斥候巡逻的当前目标集结点。**承诺到「看过」为止**：目标一旦选定就走到底，
// 不每拍按「最近的不可见集结点」重选——集结点恰在视野圈沿上时迷雾在
// V/R 之间逐拍闪（进一步 V、退一步 R），每拍重选会让斥候在两个目标之间
// 原地踏步（2026-09-05 探针实测：波2 全程钉在已探空的集结点旁，新一波的
// 部队在另一路从没被找到）。世代纪律同 `ManualOrder`。
struct PatrolGoal {
    bool active = false;
    rts::GridPos target{};
    std::uint16_t generation = 0;
};

// A committed movement goal sampled at the last visited cell. Scouts resample
// after crossing a cell so a held action cannot skip a narrow gate's turn.
struct ScoutNavigation {
    bool active = false;
    rts::GridPos cell{};
    rts::GridPos target{};
    std::uint16_t generation = 0;
};

// 微操参数。**数值全部占位**（CLAUDE.md「关于数值」）；「一族」脚本 =
// 把它们在一个范围内随机化后各造一个实例（domain randomization，
// `守方AI与协同演化.md` 2.5 那张表的「可调 / 可控」列）。
struct ScriptParams {
    float kite_trigger_cells = 2.5f;    // 占位：近战威胁进此距离，弓手后撤
    std::int32_t kite_permille = 1000;  // 占位：每个决策拍真的后撤的概率
    std::int32_t reaction_decisions = 0;  // 占位：威胁持续几拍才响应（生疏度）
    float avoid_knight_cells = 3.0f;    // 占位：游骑对骑士的保持距离
    float arrive_cells = 1.5f;          // 占位：距目标点多近算「到了」
    // 占位：敌人进到堡垒此圈以内，枪卫主动出击迎击（出圈即收兵回驻防环——
    // 圈就是风筝防线， Shade 在圈外挑逗时枪卫不上当）。
    float spear_engage_cells = 8.0f;
    // `muster_delay_decisions` 曾在这里：它挡的是「命令刚提交、还卡在 World
    // 入队与生效之间那一拍」的假阴性。命令通道没了（编队系统移除），这个
    // 参数守护的问题随之不存在——删参数而不是留着它假装还有用。
};

class DefenderScript {
    friend struct SnapshotCodec;
public:
    DefenderScript(ScriptParams p, std::uint64_t seed) noexcept
        : p_(p), rng_(seed) {}

    // 每个决策拍调一次。`ids` 必须是 `enumerate_units(Side::Defender, …)` 的
    // 产物（规范序）；`out` / `garrison_out` 与之同序，分别直接交给
    // `submit_actions` / `submit_garrison_wishes`。
    //
    // **意愿必须每拍重发，包括「保持现状」**：世界是「意愿与现状不一致即
    // 纠偏」的语义（意愿清空 ⇒ 下墙），所以已在墙上的弓手每拍都要重写一次
    // 自己脚下那段墙，否则下一拍就被放下来。
    void decide(const rts::WorldView& view, std::span<const rts::UnitId> ids,
                std::vector<rts::UnitAction>& out,
                std::vector<std::uint16_t>& garrison_out);

    // Per-tick navigation only: preserve tactical decisions and garrison wishes.
    // Returns current actions in canonical order, with scout turns refreshed.
    bool refresh_scout_navigation(const rts::WorldView& view,
                                  std::span<const rts::UnitId> ids,
                                  std::vector<rts::UnitAction>& out);

    // 给这批单位（必须是活着的守方单位）下一道临时开拔指令（框选 + 右键
    // 点空地的落点）。**只影响它们、不进 `World`**——这是它与旧 `MoveForce`
    // 的分界。优先于一切自主默认；到达目标格后自动清除，回归自主。
    // 对已上墙的单位同时是「下来」：登墙意愿清空，世界放人，再走过去。
    void issue_move_order(std::span<const rts::UnitId> ids, rts::GridPos target);
    bool issue_forced_work(const rts::WorldView& view, std::span<const rts::UnitId> ids,
                           rts::GridPos target);
    bool forced_work_active(const rts::WorldView& view, rts::UnitId id) const;

    // 同上，但目标是**驻进 `wall_cell` 那段墙**（右键点墙的落点）：走位到
    // 墙边 + 登墙意愿一起给，登墙本身照旧由 `tick_garrison` 解算。登顶后
    // 指令完成、自动清除——弓手随后会被自主逻辑留任在墙上，其余兵种
    // 回归自主（会自行下来，他们在墙上够不着人）。
    void issue_garrison_order(std::span<const rts::UnitId> ids,
                              rts::GridPos wall_cell);

private:
    rts::UnitAction decide_unit(const rts::WorldView& view, rts::UnitId id,
                                std::uint16_t& wish);
    // 给弓手挑一段墙登：最近的「完工 Wall/Gate、无人驻守（含在爬）、本拍也
    // 还没指派给别的弓手」的墙格。返回格线性下标，-1 = 没有可登的墙。
    // 「本拍已指派」由调用方在 decide() 里维护——两个弓手同时看上同一段墙
    // 正是旧编队时代被抓到过的那个 bug（挤向同一段），现在靠这张表结构性地
    // 防掉。
    int find_wall_post(const rts::WorldView& view, rts::Vec2 me) const;
    // 朝目标格走：flow field（按 (目标, 兵种, 档) 在一拍内缓存），
    // 不可达退回贪心。穿城门出城靠 field——贪心会怼在自家墙上。
    rts::UnitAction move_towards(const rts::WorldView& view, std::size_t slot,
                                 rts::GridPos goal, std::uint16_t mask);
    rts::UnitAction flee_from(const rts::WorldView& view, std::size_t slot,
                              rts::Vec2 threat, std::uint16_t mask);
    // 没有更具体的事做时的兜底：先响应清野旗（`Clear` 挂上的障碍——能破
    // 结构的闲人走去砸最近那面），没旗可响应再回驻防位（堡垒周围按槽位
    // 散开的环上一点，到了就停）。**它不是「呆住」**：环的位置贴着内城，
    // 任何方向的突破都要先过这条线。
    rts::UnitAction hold_position(const rts::WorldView& view, std::size_t slot,
                                  std::uint16_t mask);
    static rts::GridPos hold_point_for(std::size_t slot, rts::GridPos keep) noexcept;

    ScriptParams p_;
    rts::Rng rng_;
    std::vector<std::int32_t> threat_streak_;   // 按槽位：威胁已持续的决策拍数
    std::vector<ManualOrder> manual_order_;      // 按槽位：框选下达的临时指令
    std::vector<PatrolGoal> patrol_goal_;        // 按槽位：斥候巡逻承诺目标
    std::vector<ScoutNavigation> scout_navigation_;
    std::vector<std::uint16_t> wall_claimed_;   // 本拍已占 / 已指派的墙格
    // 本拍已被认领的工匠任务（建筑槽位）：工匠优先各管一个，任务不够分时
    // 才多人同任务（同任务的加速在机制层，`World::mason_count`）。
    std::vector<std::size_t> bld_claimed_;

    // 一拍内的 flow field 缓存（decide 开头重建）。
    struct CachedField {
        std::uint16_t goal;
        rts::UnitType type;
        int tier;
        rts::FlowField field;
    };
    std::vector<CachedField> fields_;
};

}  // namespace game

#endif  // GAME_DEFENDER_SCRIPT_HPP
