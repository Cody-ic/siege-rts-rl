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
#include "rts/flow.hpp"
#include "rts/stats.hpp"
#include "rts/world.hpp"

namespace game {

// 攻方的波次曲线与编成。**从编译期常量提成运行期参数**（2026-09-03）。
//
// 此前 `demo_driver.cpp` 里有六个 `constexpr` 与一串写死的整除式，配平搜索
// 一个都够不着，于是那份搜索只能在「塔造价 / 供给缩放」这类下游旋钮上打转
// （`攻守配平的数学模型.md` §4 的那条命题正好说明它们只能平移、翻不了斜率）。
//
// **形式也是旋钮，不只是系数。** 没有任何设计文档规定兵力预算必须是幂律；
// 它可以是线性、对数、或饱和的 S 形。`PowerForm` 因此是一个枚举而不是一个
// 注释——「换一条曲线」要能在参数里表达，不能要求改代码。
//
// 与 `StatsTable` 的分界：数值表是**双方共享的机制数值**（血量、伤害、造价），
// 这里是**攻方这一侧的生成规则**。同 `WorldInit::tier_income_permille` 那条
// 先例：与数值表并列的外生输入，不进 `StatsTable::fingerprint()`。
struct WaveCurve {
    // ——编成位（买人数）——
    double slots_base = 6.0;
    double slots_per_wave = 1.2;
    // 0 = 不封顶。CLAUDE.md 要求「必须有硬上限」（RL 可训练规模），
    // 而 2026-09-01 撤下的是**没有依据的取值 40**、不是那条结构主张。
    int slots_cap = 0;

    // ——兵力预算（买等级）——
    enum class PowerForm {
        Power,      // base · w^alpha        —— 现行（alpha = 1.25）
        Linear,     // base · (1 + alpha·(w−1))
        Log,        // base · (1 + alpha·ln w)
        Saturating  // base · (1 + alpha·w/(1 + w/half))  —— 后期趋平
    };
    PowerForm power_form = PowerForm::Power;
    double power_base = 6.0;
    double power_alpha = 1.25;
    double power_half = 20.0;   // 只有 Saturating 用：转折处的波数

    // ——编成比例（千分比，余量全是 Ghoul）与出场门槛——
    int shade_permille = 200;    // 原 slots/5
    int knight_permille = 150;   // 原 slots*3/20
    int ram_permille = 100;      // 原 slots/10
    int shade_from_wave = 2;
    int knight_from_wave = 2;
    int ram_from_wave = 3;
    int phoenix_from_wave = 3;
    int wraith_from_wave = 2;
    // 不死鸟数量。CLAUDE.md：「硬性数量上限，上限可随波数缓慢放开，
    // 但增速必须远低于预算增速」。现行实现恒 1、从未放开——
    // `攻守配平的数学模型.md` §5.1 记了它的后果（防空两难无可预判）。
    int phoenix_base = 1;
    int phoenix_per_waves = 0;   // 0 = 恒 phoenix_base；否则每这么多波 +1
    int phoenix_cap = 1;

    // ——分兵——
    int main_permille = 700;   // 主攻拿几成（其余给佯攻）

    // 第 w 波的编成位与兵力预算。**唯一实现处**，`demo_driver.cpp` 与
    // `tools/balance/budget_curves.py` 都以它为准。
    int slots_at(int wave) const;
    double power_at(int wave) const;
};

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
struct WaveTiming {
    int build_ticks = 260;         // 13 秒 @ 20 Hz：清波后到下一波的建造倒计时
    int first_build_ticks = 360;   // 18 秒 @ 20 Hz：开局到第 1 波

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

class DemoBattle {
public:
    // 建世界并摆守方开局兵力（城内一小队 + 箭楼与防空各一座）。
    // 攻方**不再开局就位**：波次循环生效后，每波在建造阶段结束时于集结点
    // 生成（编成是占位曲线，无平衡含义），打完进下一波。
    DemoBattle(const MapData& map, const rts::StatsTable& stats, std::uint64_t seed,
               WaveTiming timing = {}, WaveCurve curve = {});

    // 推进 `ticks` 个 tick，途中每个决策周期（kDecisionPeriodMax）重发一遍动作。
    // 波次循环也在这里驱动：建造倒计时 → 生波 → 攻方清空 → 下一波。
    // **败局（Keep 被拆）后世界定格**——再 update 也不推进，好让人看清最后一帧。
    void update(int ticks);

    const rts::World& world() const noexcept { return w_; }
    bool defeated() const noexcept { return defeated_; }
    int build_ticks_left() const noexcept { return build_left_; }

    // 玩家命令的入口（交互层从这里进，不直接碰 World——写入面收在一处）。
    // 校验与解算都归 World：形状不合法当场抛，语义不合法（买不起、点位
    // 不对）在解算时静默拒绝。`Summon` 也走这里：phase 一变，update 里的
    // 波次机就会在下一 tick 生波——倒计时与提前召唤殊途同归。
    void submit_defender(const rts::Command* cmds, std::size_t count) {
        w_.submit(rts::Side::Defender, cmds, count);
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
    WaveTiming timing_{};     // 波次节奏（占位取值，见结构体注释）
    WaveCurve curve_{};       // 攻方曲线与编成（同上）
    int build_left_ = 0;      // 建造阶段剩余 tick
    int assault_ticks_ = 0;   // 本波进攻阶段已经跑了多少 tick
    // 本波的 `Wraith` 侦查到手了没有（攻方迷雾里看见过任意一座**防御布局
    // 建筑**——塔/防空/堡垒，墙门不算，见 .cpp）。逐波重置——每一波都要
    // 重新去看，而这正是「波次结构让 AI 的记忆天然过时」那条设计的直接
    // 后果（玩家的新建筑是在波次之间造的）。
    bool wave_scouted_ = false;
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
