// Demo 对局驱动：一张地图 + 数值表 → 一场脚本化攻防。
//
// 它是**演示与联调工具**。守方一侧现在走真的执行层脚本
// （`game::DefenderScript`，README 第 6 项的最终形态）——弓手拉扯、枪卫
// 堵口不追、游骑避骑士摸攻城锤都由脚本给出；攻方一侧仍是占位脚本
// （打得着就打、否则按 flow field 向堡垒推进），它的正式形态是逐单位 RL。
// 本类只做四件事：建世界、摆两边的开局兵力、每个决策周期发一遍动作、
// 推进 tick。
//
// 放在 `game/`（不在 `render/`）：它不含像素，且要在默认构建里被测——
// 「demo 跑 N tick 后墙被打破」是一条真断言，GCC 侧也验。

#ifndef GAME_DEMO_DRIVER_HPP
#define GAME_DEMO_DRIVER_HPP

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "game/defender_script.hpp"
#include "game/map_data.hpp"
#include "game/player_input.hpp"   // SightedType（侦查报告的元素）
#include "rts/flow.hpp"
#include "rts/rng.hpp"
#include "rts/stats.hpp"
#include "game/attacker_macro.hpp"
#include "rts/world.hpp"

namespace game {

// **`WaveCurve` 2026-09-05 搬到了 `game/attacker_macro.hpp`**：它是攻方宏观层
// 的参数集，而 `AttackerMacro` 要吃它，留在这里会让那个头与本头循环包含。
// 本头包含它，所以下游（runner、测试）的 `game::WaveCurve` 一字不用改。

// 波次节奏。**从编译期常量提成运行期参数**（2026-09-03）：
//
// 组长指出「每两波之间的时间间隔也是影响平衡性的关键」，而它此前是
// `demo_driver.cpp` 里的两个 `constexpr`，配平搜索够不着。它管的不只是
// 「多久来一波」，还有两件更要紧的：
//
//   1. **收入是按 tick 结算的**，所以守方每波拿到的资源 ∝ 波长；
//   2. **施工要工匠工时、且波次进行中的工地在杀伤区里**——建造阶段是
//      唯一能安全把资源**转化**成防御的窗口。它比收入那一条更硬：
//      钱攒着不花不会变成塔。
//
// 两个值分开，是 2026-08-31 试玩反馈定下的形状（波次间隔 +5s、首波再退后
// 10s，各自独立从原先那个 8s 起算），此处只是把它变成可传参，取值不变。
//
// **2026-09-04：260 → 520、360 → 720（13 → 26 秒、18 → 36 秒）。**
// 试玩反馈两条，其实是同一件事：「斥候汇报之后玩家没有反应时间敌人就进攻了」
// 「两波之间的时间稍稍有点少，玩家建造时间有点紧」。
//
// 这不是「反应时间少」，是**结构上做不到**，算术如下（都从数值表读）：
//
//   * 斥候视野 10、速度 0.13 格/tick，集结点在墙外 D = 40 格
//     ⇒ 它要走到 40 − 10 = 30 格才看得见，单程 231 tick ≈ **12 秒**
//   * 一座箭楼 240 tick = **12 秒**
//   ⇒ 「派斥候 → 看到编成 → 还来得及照它建一座塔」的下界是 **24 秒**，
//     而建造期只有 13 秒。**侦查到的编成永远来不及变成建筑。**
//
// **2026-09-04 二次上调：520 → 660、720 → 900（26 → 33 秒、36 → 45 秒）。**
// 同日把斥候改成「到达集结点才判定」之后，上面那笔账里的行程要按**全程**算，
// 而不是按「走到视野够得着」算：
//
//   * 斥候从城里出发，到集结点是 城区半径 R(9–15) + D(40) ≈ 50–55 格
//     ⇒ 0.13 格/tick ⇒ **约 19–21 秒**
//   * 判定通过之后，玩家要来得及照情报改部署：一座箭楼 **12 秒**
//   ⇒ 下界 ≈ 33 秒。
//
// 这正是组长那句「这个判定结束后玩家应当还有反应时间调整部署」的算术形式。
// 首波再多 12 秒：那一波还得先把斥候招出来（3 秒），而且城里什么都还没有。
//
// **顺带解掉一个闭环**：`Keep` 升级要 400 tick = 20 秒 > 原来整个建造期
// ⇒ 它此前只能拖进交战期，跟维修抢同一名工匠。26 秒之后它**装得进一个
// 建造期**了，而「守方唯一的成长轴启动不了」正是
// `波次预算曲线与堡垒等级曲线.md` §2.3(1) 那条闭环的第 4 环。
//
// ⚠️ **它同时放大了一个已知的洞**，写下来免得被当成新 bug：建造期越长，
// 「等窥使看完再建」这条免费的欺骗手段就越好用（`波次分段与侦查时序.md`
// §6 第 1 条把上界算在「单座防御建筑的工期」上，正是这个理由）。那个洞的
// 正解是该提案的第三段冻结，**不是把建造期压回 13 秒**——压回去等于用
// 「玩家来不及侦查」去换「玩家不能作弊」，两头都输。
struct WaveTiming {
    int build_ticks = 660;         // 33 秒 @ 20 Hz：清波后到下一波的建造倒计时
    int first_build_ticks = 900;   // 45 秒 @ 20 Hz：开局到第 1 波

    // **进攻阶段的 tick 上限**，超时即本波结束、残兵撤走。0 = 不限（旧行为）。
    //
    // 波次循环原本只有一个出口：攻方被**全歼**。那在守方总是先死的年代够用，
    // 而守方一旦真的守得住就会死锁——实测第 3 波跑了 54391 tick，场上剩
    // `{Knight 1, Phoenix 1, Wraith 1}`：塔打不到空中、`Phoenix` 也啃不动堡垒，
    // 谁也杀不掉谁。（`attacker_can_fight()` 那次修的是**不求战**的单位，
    // 这里是**求战但打不动**的僵局，两回事。）
    //
    // 取 2400 不是拍的：CLAUDE.md「一波 = 一个 RL episode」，而地图校验器第 5 条
    // 按 episode ∈ [1200, 2400] tick 算 `Ram` 的行军占比——那个上界已经是全仓
    // 对「一波多长」的既有承诺，这里只是让 demo 真的守住它。
    int assault_max_ticks = 2400;
};

// 守方的建局参数。**只有人口上限这一对**，因为它是 `WorldInit` 的字段而不是
// 数值表的字段（`rts/world.hpp`：「人口上限是建局输入」），于是它此前根本没有
// 运行期入口——配平要 A/B 它就得重编。
//
// 默认 8+2K 是 2026-09-02 的定版（`波次预算曲线与堡垒等级曲线.md` §2.1），
// 而 §2.2 查出**那个 `2K` 从未兑现**：`K` 实战中恒等于 2，玩家拿到的永远是
// `base + 2×2`。所以调这一对时要分清——改 `base` 当场生效，改 `per` 要先让
// `K` 动起来才看得到。
struct DefenderSetup {
    std::int32_t pop_cap_base = 8;
    std::int32_t pop_cap_per_keep_level = 2;

    // ——斥候侦查：到达集结点即判定一次死活（2026-09-04）——
    //
    // **形状与攻方的 `Wraith` 对称**，这是组内定的：窥使推进到看得见防御布局
    // 就「情报到手」并掉头；斥候推进到集结点，**掷一次死活**——死了本轮侦查
    // 失败、一个字都不给玩家，活下来就视为侦查成功、当场拿到本波编成。
    //
    // 它替掉的是一版**错的**设计（同日早些时候我写的 `IntelLog`）：那一版
    // 按帧累积「看见过什么」，于是斥候刚出城门瞟一眼就已经在漏情报，
    // **侦查从来不会失败**，只会「看到多少算多少」。那把 `Scout` / `Wraith`
    // 的整个博弈消掉了——猎杀斥候没有意义，因为它死之前已经漏了一路。
    // 侦查必须是**二元事件**：成功或失败，中间没有渐进渗漏。
    //
    // **概率不随敌方数量缩放，而是靠「派几只」表达冗余**：每只斥候各掷一次
    // 独立的点，派两只的成功率是 `1 − p²`。这与提案里攻方那一侧逐字同构
    // （`波次分段与侦查时序.md` §4.1：「派两只 = 拿编成位买冗余」），
    // 而且它让 `CLAUDE.md`「斥候必须便宜到可以消耗，情报交易才诚实」这条
    // 在玩家侧真的可操作。若改成随敌方数量缩放，后期侦查会趋近必然失败——
    // 那等于在最需要情报的时候把这条机制关掉。
    std::int32_t scout_death_permille = 400;   // 占位：单只成功率 60%
    // 「到达」的判据：离集结点这么近就算到了（格，切比雪夫）。
    // **0 = 用斥候自己的视野半径**（默认，且它是自维护的：改数值表里的
    // `Scout.vision`，这条判据自动跟着走）。
    //
    // 判据取「走到看得清集结点的距离」而不是「走进集结点那一格」，
    // 有一条实现上的硬理由：`DefenderScript` 让斥候走向**最近的当前不可见的**
    // 集结点，所以它一进视野就把目标看见了、目标随即被排除 ⇒ **它永远走不到
    // 那一格**。第一版把这里写成 3，测试跑 6000 拍结论恒为 `None`，就是这个。
    //
    // 而这也是更对的设计：侦查该在看得清的地方完成，不是走进敌阵中央。
    // 风险由掷点承担，不由「站得多近」承担。
    std::int32_t scout_arrive_cells = 0;
};

class DemoBattle {
public:
    // 建世界并摆守方开局兵力（城内一小队 + 箭楼与防空各一座）。
    // 攻方**不再开局就位**：波次循环生效后，每波在建造阶段结束时于集结点
    // 生成（编成是占位曲线，无平衡含义），打完进下一波。
    DemoBattle(const MapData& map, const rts::StatsTable& stats, std::uint64_t seed,
               WaveTiming timing = {}, WaveCurve curve = {},
               DefenderSetup setup = {});

    // 推进 `ticks` 个 tick，途中每个决策周期（kDecisionPeriodMax）重发一遍动作。
    // 波次循环也在这里驱动：建造倒计时 → 生波 → 攻方清空 → 下一波。
    // **败局（Keep 被拆）后世界定格**——再 update 也不推进，好让人看清最后一帧。
    void update(int ticks);

    const rts::World& world() const noexcept { return w_; }
    bool defeated() const noexcept { return defeated_; }
    int build_ticks_left() const noexcept { return build_left_; }

    // ——本波的侦查结果（守方侧）——
    //
    // **二元**：`ScoutOutcome::None` = 还没派/还没到；`Killed` = 到了但被杀，
    // 本轮侦查失败、`report()` 为空；`Success` = 到了且活下来，`report()` 是
    // **判定那一刻斥候视野圈内看见的那部分编成**（所见即报：分兵之后，佯攻路
    // 与主攻路各是一份独立情报）。理由见 `DefenderSetup` 的那段注释。
    //
    // 快照而不是实时读数：情报的价值在于「提前知道」，而它一旦到手就不该再
    // 随战场变化——那是记忆，不是视野。它也因此在交战期仍然可读。
    enum class ScoutOutcome { None, Killed, Success };
    ScoutOutcome scout_outcome() const noexcept { return scout_outcome_; }
    const std::vector<SightedType>& scout_report() const noexcept {
        return scout_report_;
    }

    // ——攻方侧的侦查与情报（只读，给测试、HUD 与 runner）——
    //
    // `wave_scouted()` 是**本波**攻方窥使有没有看到防御布局；`wave_intel()`
    // 是**生波那一刻**攻方以为守方长什么样（读的是它自己的迷雾记忆）。
    // 两者此前一个访问器都没有——于是「杀掉窥使让 AI 带错情报」这条设计
    // 在测试里断言不了、在报告里也量不出来。
    bool wave_scouted() const noexcept { return wave_scouted_; }
    const AttackerIntel& wave_intel() const noexcept { return wave_intel_; }

    // 玩家命令的入口（交互层从这里进，不直接碰 World——写入面收在一处）。
    // 校验与解算都归 World：形状不合法当场抛，语义不合法（买不起、点位
    // 不对）在解算时静默拒绝。`Summon` 也走这里：phase 一变，update 里的
    // 波次机就会在下一 tick 生波——倒计时与提前召唤殊途同归。
    //
    // **Summon 护栏（2026-09-03）**：建造期开始后的前 `kSummonGuardTicks`
    // tick 里 Summon 在这里被丢掉（与解算层同例：静默拒绝）。要挡的是
    // 「上一波交战时连打 N，最后一发溢进新建造期第一拍」——那一发不是
    // 「想提前开打」，是上一波操作的尾巴。护栏**不**管也更管不了「按住
    // N」的键盘自动重复：raylib 的 `IsKeyPressed` 是边沿触发、一次物理
    // 按压只来一发，而护栏期之后的每一发都视为玩家本意。
    //
    // 护栏放在 demo 层而不是 `World`：RL 侧（train/）直接驱动 `World`，
    // 「提前召唤」是它的合法动作，不该吃这道交互层的防误触。
    static constexpr rts::Tick kSummonGuardTicks = 2;
    bool summon_accepted_now() const noexcept {
        return w_.phase() == rts::WavePhase::Build &&
               w_.now() - build_start_ >= kSummonGuardTicks;
    }
    void submit_defender(const rts::Command* cmds, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            // 护栏期内的 Summon 不进 World（见上）。交互层的「已提前召唤」
            // 确认反馈必须用 `summon_accepted_now()` 这同一条判据，否则
            // 会骗玩家「你的操作生效了」。
            if (cmds[i].kind == rts::CommandKind::Summon && !summon_accepted_now()) {
                continue;
            }
            w_.submit(rts::Side::Defender, &cmds[i], 1);
        }
    }

    // 框选 + 右键的辅助性单兵指令：转给执行层脚本，**不进 `World`**——
    // 它是临时覆盖（到达即失效），不是持久状态，理由见
    // `game/defender_script.hpp` 文件头。对已上墙的单位，开拔指令同时
    // 是「下来」（脚本清空它的登墙意愿，世界放人）。
    void issue_move_order(std::span<const rts::UnitId> ids, rts::GridPos target) {
        script_.issue_move_order(ids, target);
    }
    void issue_garrison_order(std::span<const rts::UnitId> ids,
                              rts::GridPos wall_cell) {
        script_.issue_garrison_order(ids, wall_cell);
    }

private:
    void issue_actions();
    void spawn_wave();
    bool keep_alive() const;
    // 本波打完了没有：判据是「攻方**还能打的**归零」而不是「存活数归零」，
    // 理由（一个 CLAUDE.md 早就写下、直到守方真的守得住才发作的坑）见 .cpp。
    bool attacker_can_fight() const;
    void withdraw_noncombat_attackers();
    // 超时收场：把场上所有攻方单位撤走（本波打不动了，残兵撤退）。
    void withdraw_all_attackers();
    rts::UnitAction greedy_move(rts::UnitId id, rts::Vec2 target) const;
    // 攻方推进：按 flow field 取下一步（机制第六批的消费侧）。field 指向
    // 被墙占着的格是正常输出——移动机制把那一步变成自动破坏，「绕远走缺口
    // vs 就近砸墙」由代价模型自己比较。不可达退回贪心（演示不卡死）。
    rts::UnitAction flow_step(rts::UnitId id);

    rts::World w_;
    // 守方执行层（参数取占位默认；种子从对局种子派生，demo 因此仍是确定性的）。
    DefenderScript script_;
    // 攻方宏观决策层。**声明顺序即初始化顺序**——它在 `timing_`/`curve_` 之前，
    // 与构造函数的初始化列表一致（不一致会吃 `-Wreorder`）。
    AttackerMacro macro_;
    WaveTiming timing_{};     // 波次节奏（占位取值，见结构体注释）
    WaveCurve curve_{};       // 攻方曲线与编成（同上）
    DefenderSetup setup_{};   // 守方建局参数（人口上限、斥候侦查）
    int build_left_ = 0;      // 建造阶段剩余 tick
    int assault_ticks_ = 0;   // 本波进攻阶段已经跑了多少 tick
    // 本建造阶段开始的那一刻（`w_.now()`）。唯一的用途是 Summon 护栏
    // （见 `summon_accepted_now`）：首波从建局（now == 0）进建造期，
    // 默认 0 即正确；之后每波在 update 的清波分支里随 `build_left_` 一起重记。
    rts::Tick build_start_ = 0;
    // 本波的 `Wraith` 侦查到手了没有（攻方迷雾里看见过任意一座**防御布局
    // 建筑**——塔/防空/堡垒，墙门不算，见 .cpp）。逐波重置——每一波都要
    // 重新去看，而这正是「波次结构让 AI 的记忆天然过时」那条设计的直接
    // 后果（玩家的新建筑是在波次之间造的）。
    // 斥候侦查：每拍查一次「有没有斥候到了集结点」，到了就掷一次死活。
    void tick_scout_recon();

    bool wave_scouted_ = false;
    // **上一波**的侦查结果，逐波从 `wave_scouted_` 结转。
    //
    // `spawn_wave()` 需要的是它而不是 `wave_scouted_`：编成在波次开始时定死
    // （`波次分段与侦查时序.md` §1.1），而那一刻本波的 `wave_scouted_` 刚被
    // 重置成 false，用它只会恒假。
    bool prev_wave_scouted_ = false;
    // 本波生波时攻方以为守方长什么样（`AttackerMacro::read_intel` 的产物）。
    // 存下来是给测试与 runner 看的——否则「攻方读到了什么」只能从编成反推。
    AttackerIntel wave_intel_{};
    // ——斥候侦查的本波状态（逐波重置）——
    //
    // `recon_rng_` 与 `script_` 的种子同源派生，所以 demo 仍然是确定性的。
    // **它不是 `World` 的 RNG**：从 `game/` 去消费仿真那条流会让 `World` 的
    // 随机序列依赖于上层逻辑，而那条流要给回放对齐用。
    rts::Rng recon_rng_;
    ScoutOutcome scout_outcome_ = ScoutOutcome::None;
    std::vector<SightedType> scout_report_;
    // 本波已经掷过点的斥候（原始句柄值）。**按只记账**：每只各掷一次独立的
    // 点，派两只就是两次机会——「派几只」是玩家的冗余决策。
    std::vector<std::uint32_t> scout_rolled_;
    bool defeated_ = false;   // Keep 被拆即败（丢失即败是设计，不是演示便宜）
    int since_decision_ = 0;
    std::vector<rts::UnitId> ids_;
    std::vector<rts::UnitAction> acts_;
    std::vector<std::uint16_t> wishes_;   // 守方登墙意愿，与 acts_ 同拍同序
    // field 缓存：兵种 × 等级档，每个决策拍作废重算（墙血变了破坏代价就变）。
    // demo 的攻方全是 1 级（低档），但按契约的形状存——这就是「档数烤进
    // 下游缓存下标」的那个下游。档界用占位默认值（rts/flow.hpp）。
    rts::FlowTiering tiering_{};
    std::array<std::optional<rts::FlowField>,
               static_cast<std::size_t>(rts::kUnitTypeCount) * rts::kFlowTierCount>
        flow_{};
};

}  // namespace game

#endif  // GAME_DEMO_DRIVER_HPP
