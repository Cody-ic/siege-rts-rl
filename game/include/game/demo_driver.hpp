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
#include "game/rl_policy.hpp"
#include "game/macro_policy.hpp"
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
    friend struct SnapshotCodec;
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
    void set_tactical_policy(std::shared_ptr<TacticalPolicy> policy);
    void set_defender_policy(std::shared_ptr<MacroPolicy> policy);
    std::string defender_policy_identity() const { return defender_policy_?defender_policy_->identity():std::string{}; }
    rts::Rng::State defender_policy_rng() const noexcept { return defender_rng_.state(); }
    std::string tactical_policy_identity() const { return policy_ ? policy_->identity() : std::string{}; }
    std::size_t learned_squads() const noexcept { return learned_squads_; }
    // Read-only coverage: remembered economy targets, policy-supported economy
    // squads, all policy-supported squads in the current assault.
    std::array<std::size_t,3> tactical_goal_diagnostics() const;

    const rts::World& world() const noexcept { return w_; }
    bool developer() const noexcept {return w_.developer();}
    void enable_developer();
    void developer_wave(int wave);
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
    const WavePlan& wave_plan() const noexcept {return wave_plan_;}
    const WavePlan& baseline_plan() const noexcept {return baseline_plan_;}
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
    struct PlayerEvent {
        rts::Tick tick=0;
        int kind=0; // 0 command, 1 move, 2 garrison, 3 forced work
        rts::Command command{};
        std::vector<rts::UnitId> ids;
        rts::GridPos target{};
    };
    const std::vector<PlayerEvent>& player_events() const noexcept { return player_events_; }

    // ——不死鸟的跨波状态（2026-09-10，#170）——
    //
    // 只读，给测试、runner 与 #171 的彩蛋判据看。**跨波身份不在 `World` 里**
    // （见私有段那条注释），所以没有这几个访问器就完全观察不到——同
    // `wave_scouted()` 当初的处境。
    struct PhoenixRecord {
        int id = 0;             // 跨波身份号（`next_phoenix_id_` 发的）
        int waves_alive = 0;    // 连续存活了几波（撤离一次 +1，被击落即出局）
    };
    //
    // 撤离成功、下一波会回来的那些。`waves_alive` 是**连续**存活波数，
    // 被击落即出局归零——#171《一跃》数的就是它。
    const std::vector<PhoenixRecord>& phoenix_roster() const noexcept {
        return phoenix_roster_;
    }
    bool earned_white_feather() const noexcept {return white_feather_;}
    // 被击落、正在等重生的那些：{身份号, 还差几波}。
    const std::vector<std::pair<int,int>>& phoenix_respawn() const noexcept {
        return phoenix_respawn_;
    }
    // 场上这只不死鸟的跨波身份号（不是不死鸟或没有身份则 −1）。
    int phoenix_identity(rts::UnitId id) const noexcept {
        const std::size_t ui = id.index();
        return w_.alive(id) && w_.unit_type(id) == rts::UnitType::Phoenix && ui < phoenix_id_of_.size()
            ? phoenix_id_of_[ui] : -1;
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
            player_events_.push_back({w_.now(),0,cmds[i],{}, {}});
        }
    }

    // 框选 + 右键的辅助性单兵指令：转给执行层脚本，**不进 `World`**——
    // 它是临时覆盖（到达即失效），不是持久状态，理由见
    // `game/defender_script.hpp` 文件头。对已上墙的单位，开拔指令同时
    // 是「下来」（脚本清空它的登墙意愿，世界放人）。
    void issue_move_order(std::span<const rts::UnitId> ids, rts::GridPos target) {
        script_.issue_move_order(ids, target);
        player_events_.push_back({w_.now(),1,{},std::vector<rts::UnitId>(ids.begin(),ids.end()),target});
    }
    bool issue_forced_work(std::span<const rts::UnitId> ids, rts::GridPos target) {
        if (!script_.issue_forced_work(w_.view(rts::Side::Defender), ids, target)) return false;
        player_events_.push_back({w_.now(),3,{},std::vector<rts::UnitId>(ids.begin(),ids.end()),target});
        return true;
    }
    bool forced_work_active(rts::UnitId id) const {
        return script_.forced_work_active(w_.view(rts::Side::Defender), id);
    }
    void issue_garrison_order(std::span<const rts::UnitId> ids,
                              rts::GridPos wall_cell) {
        script_.issue_garrison_order(ids, wall_cell);
        player_events_.push_back({w_.now(),2,{},std::vector<rts::UnitId>(ids.begin(),ids.end()),wall_cell});
    }

private:
    std::shared_ptr<TacticalPolicy> policy_;
    std::shared_ptr<MacroPolicy> defender_policy_;
    rts::Rng defender_rng_{1};
    std::size_t learned_squads_ = 0;
    std::vector<PlayerEvent> player_events_;
    void issue_actions();
    void spawn_wave();
    bool keep_alive() const;
    // 本波打完了没有：判据是「攻方**还能打的**归零」而不是「存活数归零」，
    // 理由（一个 CLAUDE.md 早就写下、直到守方真的守得住才发作的坑）见 .cpp。
    bool attacker_can_fight() const;
    void withdraw_noncombat_attackers();
    // 超时收场：把场上所有攻方单位撤走（本波打不动了，残兵撤退）。
    void withdraw_all_attackers();
    // 飞回最近的集结点。`Wraith`（看完就撤）与 `Phoenix`（掉血就撤）共用。
    rts::UnitAction retreat_action(rts::UnitId id) const;
    // 这只不死鸟该撤了吗（血量低于阈值，或本波已无地面战斗单位）。见 .cpp。
    bool phoenix_should_withdraw(rts::UnitId id, bool any_ground_combat) const;
    // 每拍查一次「有没有正在撤的不死鸟到了集结点」，到了就离场。
    void tick_phoenix_withdrawal();
    // 换波时结算不死鸟的去留（重生倒计时 / 击落入队 / 花名册）。见 .cpp。
    void settle_phoenix_roster();
    // 把这只不死鸟记进花名册并移出世界。**撤不是死**：超时收场那条路也走它。
    void retire_phoenix(rts::UnitId id);
    rts::UnitAction greedy_move(rts::UnitId id, rts::Vec2 target) const;
    // 攻方推进：按 flow field 取下一步（机制第六批的消费侧）。field 指向
    // 被墙占着的格是正常输出——移动机制把那一步变成自动破坏，「绕远走缺口
    // vs 就近砸墙」由代价模型自己比较。不可达退回贪心（演示不卡死）。
    rts::UnitAction flow_step(rts::UnitId id);
    // 这一队读哪张 field（0 = 打堡垒，1 = 打经济）。见 .cpp。
    int squad_goal_of(rts::UnitId id) const;
    const std::vector<rts::GridPos>& goal_cells(int set) const;
    rts::GridPos goal_anchor(int set) const;

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
    // 攻方的编队归属：**按单位槽位下标**（`UnitId::index()`）存编队号，−1 = 不属于
    // 任何编队（守方单位、或已被复用的空槽）。
    //
    // 编队是 **RL 动作空间的分组**：一支编队 = 一个 agent，动作复制给队里每个
    // 单位、观测取队长。它因此**不进 `World`/`Command`/回放**——动作提交仍是
    // 逐单位的（`submit_actions` 按 `enumerate_units` 序）。
    //
    // ⚠️ **别把它与 2026-09 移除的那套「守方编队」混起来。** 那套是**玩家下的
    // 指令**（`MoveForce`/`SelectForce`/`Garrison`/`UpgradeForce`，移除时
    // `kCommandKindCount` 13→10、回放 v3→v4）。这一套只在 `game/` 内部记账，
    // 不新增任何命令，也不是把删掉的东西捡回来。
    std::vector<int> squad_of_;
    // 每支编队的**意图**：读哪个目标集。按单位槽位下标存（与 `squad_of_` 同款）。
    //
    // **这就是「编队动作」本身。** 文献一致（TStarBot2 / ROMA / RODE）：
    // group action 是共享的**子目标**，不是共享的输出——每个成员仍在自己那一格
    // 采样 flow field。所以这一层不需要任何「把动作抄给队员」的代码。
    std::vector<int> squad_goal_;
    // 两个目标集的格子。构造/生波时算好（固定序）。
    std::vector<rts::GridPos> keep_goals_;   // 恒为 {keep}
    std::vector<rts::GridPos> econ_goals_;   // 记忆里的采集建筑，可能为空
    std::vector<int> econ_squads_;           // 本波派去打经济的编队号（固定序）
    // 本波生波时攻方以为守方长什么样（`AttackerMacro::read_intel` 的产物）。
    // 存下来是给测试与 runner 看的——否则「攻方读到了什么」只能从编成反推。
    AttackerIntel wave_intel_{};

    // ——不死鸟的跨波身份（2026-09-10，#170）——
    //
    // `CLAUDE.md`「空中单位」给 `Phoenix` 定了四条结构约束，其中两条此前从未
    // 落地：**跨波留存**与**击落后 N 波重生**。后果不只是彩蛋没法挂
    // （#171）——`配平工作交接.md` §2.10 实测它 58/60 波出场、只被击落 12 次，
    // 而守方全程只有 2 座 `Flak` ⇒ 「AA 的机会成本」那个两难从未成立过；
    // `攻守配平的数学模型.md` §5.1 还量出塔损耗是**阶跃**（0 或全灭）而不是
    // 渐进，成因正是它不会走：要么被 AA 清掉、要么 AA 被清掉后无人能挡。
    // 会撤的不死鸟才是设计说的「手术刀」（点一座塔、掉血就走、下一波再来）。
    //
    // **身份号只在 `game/` 里**，`World` 的 `UnitId` 每次重生都是新的——同
    // `squad_of_` 不进 `World` 的先例，`rts_core` 一行不动。
    //
    // **等级与血量刻意不跨波带**（2026-09-10 组内定）：回来的不死鸟是**当波
    // 名义等级、满血**。理由是代价该放在哪——`CLAUDE.md` 原文说「击落一次
    // 意味着攻方**数波内失去空中打击能力**」，所以惩罚定义在**重生延迟**上，
    // 而不是让老鸟随波次相对变弱。若保留出生等级，第 10 波的 12 级鸟到第 30
    // 波只有当波 31 级新鸟的 67% 血伤 ⇒ 它变成编成里的死重，且 #171《一跃》
    // 那条「放它活 N 波」会自我拆台（越活越弱 ⇒ 越难保住，而不是越难忍住）。
    //
    // 于是这条记录只需要**身份**与**连续存活波数**——后者正是《一跃》要数的
    // （`PhoenixRecord` 声明在公开段，因为访问器要返回它）。
    //
    // 撤离成功、下一波回来的那些（固定序：按撤离先后）。
    std::vector<PhoenixRecord> phoenix_roster_;
    // 被击落的：{身份号, 还差几波重生}。数到 0 那一波以**满血**回来。
    std::vector<std::pair<int,int>> phoenix_respawn_;
    // 场上每只不死鸟的身份号，按单位槽位下标存（同 `squad_of_` 那套记账）。
    // −1 = 这个槽位上不是不死鸟（或已被复用）。
    std::vector<int> phoenix_id_of_;
    int next_phoenix_id_ = 0;
    bool white_feather_ = false;
    AttackerIntel recon_intel_{};
    WavePlan wave_plan_{},baseline_plan_{};
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
    // 目标集数（形状）。0 = 打堡垒，1 = 打经济。
    // 加这一维**按比例增加每决策拍的 field 重算量**（`flow_` 每拍全废，因为
    // 破坏代价读实时墙血），而那直接是 RL 训练吞吐。所以刻意只有两档，
    // 且经济那一档只分给少数编队。
    static constexpr std::size_t kGoalSetCount = 2;
    rts::FlowTiering tiering_{};
    std::array<std::optional<rts::FlowField>,
               static_cast<std::size_t>(rts::kUnitTypeCount) * rts::kFlowTierCount *
                   kGoalSetCount>
        flow_{};
};

}  // namespace game

#endif  // GAME_DEMO_DRIVER_HPP
