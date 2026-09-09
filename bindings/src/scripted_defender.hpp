#pragma once

// **把真正的守方接进训练回路。**（2026-09-07）
//
// ## 为什么在 `bindings/` 而不在 `rts_core/`
//
// 攻方 RL 跑满 40M 步之后查出训练图里**一个守方单位都没有**——`enemy_*`
// 那几条观测通道十万局零梯度（`配平工作交接.md` §2.14.3d）。而
// **光把守方单位摆进 `World` 是不够的**：单位出生动作是 `Stop`，攻击阶段
// 只处理攻击类动作 ⇒ 它们会站在原地被打死而一枪不放。
//
// 所以要有人**每个决策拍替守方下命令与动作**。而真正的守方是
// `game::DefenderScript`（单兵）+ `game::DefenderMacro`（宏观），
// **`rts_core` 不能依赖 `game/`** ⇒ 那一层只提供
// `BatchedEnvInit::opponent_hook` 这个钩子，实现放在这里。
//
// > **我第一版想在 `rts_core` 里写一个二十行的「自杀式陪练」夹具**，理由是
// > 分层不允许依赖 `game/`。**那个推理是错的**（队友在 #146 §2.16 指出）：
// > `bindings/` 已经依赖 `game/`，所以完全可以在这里组合既有的守方逻辑。
// > 差别不小——夹具只会贴脸，而真守方会风筝、会登墙、按克制关系挑目标，
// > 那才是攻方最终真要面对的东西。夹具那一版已撤，没进仓库。
//
// ## 与 `DemoBattle` / `calibration_runner` 是同一条接法
//
// 三处都是同一个序列：`DefenderMacro::decide` → `submit`（宏观命令）、
// `DefenderScript::decide` → `submit_actions` + `submit_garrison_wishes`
// （单兵动作与登墙意愿）。**刻意不抄一份简化版**：简化版会与真机行为漂移，
// 而攻方 RL 会过拟合到那个漂移。
//
// ## 一条必须守住的纪律：**只碰第 i 局自己的状态**
//
// 钩子在 `for_each_env` 的工作线程里被调用。所以每一局各持一份
// `DefenderScript` + `DefenderMacro` + 三块缓冲，**下标即环境号**。
// 共用任何一块都是数据竞争，而它不会报错——只会让同一个种子跑两遍得到
// 两份不同的数据（`batched_env.hpp` 文件头把这条列为最要防的失败形态）。

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "game/defender_macro.hpp"
#include "game/defender_script.hpp"
#include "game/map_data.hpp"
#include "rts/types.hpp"
#include "rts/world.hpp"

namespace bindings {

// 逐局一份的守方大脑 + 缓冲。`operator()` 就是那个钩子。
class ScriptedDefender {
public:
    std::unique_ptr<rts::World> prepare(rts::WorldInit init, int i, int ticks,
                                      int decision_ticks) {
        // Preserve the exact requested roster, but do not expose it to defenders
        // or let it take tower damage during preparation.
        std::vector<rts::UnitInit> attackers;
        std::vector<rts::UnitInit> defenders;
        for (const auto& unit : init.units) {
            (rts::side_of(unit.type) == rts::Side::Attacker ? attackers : defenders).push_back(unit);
        }
        init.units = std::move(defenders);
        auto world = std::make_unique<rts::World>(std::move(init));
        for (int elapsed = 0; elapsed < ticks;) {
            (*this)(*world, i);
            const int step = std::min(decision_ticks, ticks - elapsed);
            world->advance(step);
            elapsed += step;
        }
        for (const auto& unit : attackers)
            world->spawn_unit(unit.type, unit.pos, unit.level, unit.hp, unit.max_hp, unit.squad);
        world->begin_assault();
        (void)world->take_tally(rts::Side::Attacker);
        (void)world->take_tally(rts::Side::Defender);
        // The same per-slot brain continues into combat: do not discard its
        // construction quotas, orders or RNG after preparing the city.
        return world;
    }
    // `n` = 批大小。`seed` 给每局的脚本 RNG 错开（`DefenderScript` 内部有
    // `Rng`，同一个种子会让所有局的随机决策完全同步——那不是错，但会让
    // 一批 256 局的多样性凭空少一维）。
    ScriptedDefender(const game::MapData& map, int n, std::uint64_t seed,
                     const game::MacroParams& mp = {},
                     const game::ScriptParams& sp = {},
                     int macro_period_steps = 1)
        : ScriptedDefender(std::vector<game::MapData>{map},n,seed,mp,sp,macro_period_steps) {}

    ScriptedDefender(const std::vector<game::MapData>& maps, int n, std::uint64_t seed,
                     const game::MacroParams& mp = {}, const game::ScriptParams& sp = {},
                     int macro_period_steps = 1, std::vector<game::MacroParams> profiles = {})
        : maps_(maps), mp_(mp), sp_(sp), profiles_(std::move(profiles)), seed_(seed),
          macro_period_(macro_period_steps < 1 ? 1 : macro_period_steps) {
        if(maps_.empty()) throw rts::ContractError("ScriptedDefender requires at least one map");
        per_.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            per_.push_back(std::make_unique<Per>(
                maps_.front(), mp, sp, seed + static_cast<std::uint64_t>(i) * 7919u));
        }
    }

    // 换局时重置第 i 局的守方大脑。**必须做**：`DefenderMacro` 有跨波状态
    // （`last_wave_`、空军见闻滑窗、逐波限额），不重置的话新局会继承上一局
    // 的「这一波已经铺过两座采集建筑了」。
    void reset(int i, const game::MapData& map, std::uint64_t seed,
               const game::MacroParams& mp, const game::ScriptParams& sp) {
        per_[static_cast<std::size_t>(i)] = std::make_unique<Per>(map, mp, sp, seed);
    }

    // 钩子本体。**只碰 `per_[i]`**，见文件头那条纪律。
    void operator()(rts::World& w, int i) {
        // reset_one replaces World but does not recreate this captured callback.
        // Reset at tick zero, including the first episode, using world seed rather
        // than batch index so frozen evaluation is independent of batching.
        // Training assigns map index = world seed modulo the map pool size.
        // The same rule applies after reset_one, independent of batch slot.
        if (w.now() == 0) {
            // Whole map cycle per profile; independent of worker/batch slot.
            const auto& params=profiles_.empty()?mp_:profiles_[(w.seed()/maps_.size())%profiles_.size()];
            reset(i, maps_[w.seed()%maps_.size()], seed_ ^ w.seed(), params, sp_);
        }
        Per& s = *per_[static_cast<std::size_t>(i)];

        // ——宏观：建 / 修 / 招 / 升 / 清野——
        //
        // **不必每个决策拍都跑**：它扫城区那 (2R+1)² 格 + 资源点，而它的
        // 动作是波次级的（建造要几百 tick）。`macro_period_` 是那个节流，
        // 默认 1（与 `calibration_runner` 的 20 tick / 决策拍同量级）。
        if (s.tick % macro_period_ == 0) {
            s.cmds.clear();
            s.orders.clear();
            s.macro.decide(w, s.cmds, s.orders);
            if (!s.cmds.empty()) {
                // **逐条提交**，与 `DemoBattle::submit_defender` 同形
                // ——那一侧要逐条过一个 `Summon` 护栏，这里没有护栏但保持
                // 同样的粒度，免得将来那条护栏加进来时两处行为分叉。
                for (const rts::Command& c : s.cmds) {
                    w.submit(rts::Side::Defender, &c, 1);
                }
            }
            // 宏观层产出的临时单兵指令（框选+右键那条通道的脚本版）。
            // **它不进 `World`**——`DefenderScript` 自己记账，到达即失效。
            for (const game::UnitOrder& o : s.orders) {
                if (o.garrison) {
                    s.script.issue_garrison_order(o.ids, o.target);
                } else {
                    s.script.issue_move_order(o.ids, o.target);
                }
            }
        }
        ++s.tick;

        // ——单兵：动作 + 登墙意愿，两条并行通道——
        //
        // **意愿必须每拍重发**（`defender_script.hpp`：世界是「意愿与现状
        // 不一致即纠偏」的语义 ⇒ 不重发下一拍就被放下墙）。所以这一段
        // **不受 `macro_period_` 节流**。
        w.enumerate_units(rts::Side::Defender, s.ids);
        s.script.decide(w.view(rts::Side::Defender), s.ids, s.acts, s.wishes);
        w.submit_actions(rts::Side::Defender, s.acts.data(), s.acts.size());
        w.submit_garrison_wishes(rts::Side::Defender, s.wishes.data(),
                                 s.wishes.size());
    }

private:
    struct Per {
        game::DefenderMacro macro;
        game::DefenderScript script;
        std::vector<rts::Command> cmds;
        std::vector<game::UnitOrder> orders;
        std::vector<rts::UnitId> ids;
        std::vector<rts::UnitAction> acts;
        std::vector<std::uint16_t> wishes;
        int tick = 0;

        Per(const game::MapData& map, const game::MacroParams& mp,
            const game::ScriptParams& sp, std::uint64_t seed)
            : macro(map, mp), script(sp, seed) {}
    };
    // `unique_ptr` 而不是直接 `vector<Per>`：`Per` 含 `DefenderMacro`
    // （内部有若干预算好的固定序候选表），而 vector 扩容会搬它们。
    // 搬动本身是安全的，但钩子在多线程里按下标取引用 ⇒ **绝不能让
    // 那些引用因为一次扩容而失效**。指针稳定这条比省一层间接重要。
    std::vector<std::unique_ptr<Per>> per_;
    std::vector<game::MapData> maps_;
    game::MacroParams mp_;
    game::ScriptParams sp_;
    std::vector<game::MacroParams> profiles_;
    std::uint64_t seed_;
    int macro_period_ = 1;
};

}   // namespace bindings
