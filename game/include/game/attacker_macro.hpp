#pragma once

// 攻方的**宏观决策层**（脚本版）：把「攻方 RL 宏观层会决定的那些事」变成代码。
//
// ## 为什么必须有它
//
// 攻方此前**根本没有这一层**：编成写死在 `spawn_wave()` 的几个整除式里、
// 主攻方向是一行 `wave % n_spawn`、目标是一个常量 `keep_pos()`。四条实测缺陷
// （`配平工作交接.md` §2.9/§2.10）全是这一个空缺的不同侧面：
//
//   * 60 波累计打掉 67 座塔 / 22 座采石场，**全是「路过顺手」**——而顺手已经
//     把守方的采集建筑钉在 6 座。刻意打它就是掐守方唯一的成长引擎
//   * `wave_scouted_` 只有一个消费者（`Wraith` 自己掉头）⇒「杀掉窥使让 AI 带错
//     情报」零收益
//   * `Phoenix` 恒 1 只，而守方全程只有 2 座 `Flak` ⇒「AA 的机会成本」那个
//     两难在仿真里从未成立过
//   * 编成不看守方长什么样
//
// 这直接是**陪练质量**问题，和 2026-09-04 那次守方脚本重写同型：下一步要跑
// 「守方 RL vs 攻方脚本」，对着一个不打经济、不读情报、方向按取余轮转的攻方，
// 守方 RL 学到的会是一份**建造顺序**而不是策略。
//
// ## 它是什么，不是什么
//
// **是** `CLAUDE.md`「宏观层是 bandit 尺度的问题：给定本波预算决定兵种配比，
// 一波决策一次……**前期甚至可先写死脚本**」那句话的落地。动作面严格等于攻方
// RL 宏观层将来要控制的三样：**编成 / 主攻与佯攻方向 / 目标优先级**。
//
// **不是**逐单位微操——那在 `DemoBattle::issue_actions()` 里，是战术层
// （将来归逐单位 RL）。两层的分界与守方 `DefenderMacro` / `DefenderScript`
// 那一对完全对称。
//
// **不是**最优策略。`AttackerParams` 全是旋钮，取值一律是占位值。
//
// ## 情报口径：只读攻方自己的迷雾
//
// 这是 `DefenderMacro` 那条「不读 god 视角编成」的镜像，也是 `CLAUDE.md`
// 「RL 侧两条硬要求」第 1 条在脚本层的自觉遵守。攻方看得见的只有
// `FogLayer` 里的**记忆建筑**——它跨波持久，而波次之间玩家会改建筑，
// 于是「记忆天然过时」这条设计不需要任何新机制就成立了：本波没侦查到
// （窥使死了），攻方就照上一波的记忆开打。
//
// > ⚠️ **一个已经存在、本层不修也不扩大的洞**：`rts::FlowField::compute()`
// > 读的是 **god 视角**的墙血（`flow.cpp` 走 `view.bld_hp()`），而攻方的寻路
// > 一直是这么算的。所以本层让**决策**（打哪、走哪个方向）过迷雾，而
// > **执行**（沿途墙血的代价估计）仍是 god 视角。要堵它得给 `FlowField`
// > 一个吃迷雾的代价来源，那是 `rts_core` 的 API 改动，不在这一层。
//
// ## 两条实现纪律（与 `DefenderMacro` 同款）
//
// 1. **只读，不写。** 本层不碰 `World`，只返回一份计划；生成与下令仍在
//    `DemoBattle`。于是它天然可测、天然不会做出「玩家/RL 做不到的事」。
// 2. **遍历顺序即规范。** 候选格在构造时按行主序算好、此后不变；
//    资源点按地图文件序。不用任何未定序容器——`determinism_bans` 拦得住
//    `unordered_map`，拦不住「按距离排序时距离相等」这类不稳定序。

#include <cstdint>
#include <vector>

#include "game/map_data.hpp"
#include "rts/roster.hpp"
#include "rts/types.hpp"
#include "rts/world_view.hpp"

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
//
// **2026-09-05 从 `demo_driver.hpp` 搬到这里**：它是攻方宏观层的参数集，
// 而 `AttackerMacro` 要吃它——留在原处会让本文件与 `demo_driver.hpp` 循环
// 包含。`demo_driver.hpp` 包含本文件，所以下游（runner、测试）不受影响。
struct WaveCurve {
    // ——编成位（买**编队**数，2026-09-05 起；此前是单位数）——
    double slots_base = 6.0;
    double slots_per_wave = 1.2;
    // **编队数硬顶 = RL 的 agent 数上限。取 27，这是第一个有依据的取值。**
    //
    // 2026-09-01 撤下的是没有依据的 40；这次的 27 有三条依据：
    //
    //   1. **SMAC 最大的官方图 `27m_vs_30m` 恰是 27 个 agent**，官方难度标
    //      「Super Hard」，默认 QMIX 只有 56% 胜率、调参后才 100%。这是最
    //      直接可比的对标——同样是战术微操、同样参数共享。
    //   2. **我们每个 agent 的观测是 SMAC 的 18–21 倍**（3166 float = 12.4 KB
    //      vs SMAC 的 ~150–176 float）。成因是编码方式：SMAC 是实体列表，
    //      我们是 15×15×14 稠密网格。所以该取文献安全带（20–30）的**下沿**。
    //   3. 检索找不到任何一例「参数共享 PPO + ~3000 维观测 + 40 个 agent」
    //      收敛的公开工作。40 在带外、无先例背书。
    //
    // ⚠️ **`CLAUDE.md` 引 arXiv 1804.00810 说「40v60 胜率明显下滑」，数字对
    // 但读法错**：那 80.5% 是**零样本泛化**（只训练到 20v30 直接拿去跑
    // 40v60），不是「训练 40 个失败」；而且它用 93 维状态 + Sarsa(λ)，
    // 不是 PPO。那句话不能当「40 是 RL 的墙」引。
    //
    // **单位数不受这个顶约束**（约 27 × 2.58 ≈ 70）：约束 agent 数的理由是
    // RL 可训练规模，它不适用于单位数。见 `WavePlan::units()`。
    int slots_cap = 27;

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
    //
    // **千分比现在作用在编队数上，所以 `ram_permille` 必须重标**：`Ram` 单独
    // 成队 ⇒ Ram 台数 = Ram 队数。旧值 100‰ 在无硬顶的 76 编成位下给 7 台，
    // 在 27 个编队下只给 2 台——而 `攻守实力模型与平衡分析.md` §3 有一条
    // 「Ram 阈值」（要 N 台**同时**到墙下才破得开），7 → 2 是结构性变化。
    //
    // 250‰ = 6 台，接近旧行为。**这是占位**：真正的目标值该来自那条 Ram 阈值，
    // 不是「凑回旧数字」。`Shade`/`Knight` 每队 3 / 2 个，所以它们的千分比
    // 不需要同比例抬——它们的单位数本来就跟着队规模乘回来了。
    int shade_permille = 200;
    int knight_permille = 150;
    int ram_permille = 250;      // 占位，见上；旧值 100 在编队语义下只给 2 台
    int shade_from_wave = 2;
    int knight_from_wave = 2;
    int ram_from_wave = 3;
    int phoenix_from_wave = 3;
    int wraith_from_wave = 2;
    // 不死鸟数量。CLAUDE.md：「硬性数量上限，上限可随波数缓慢放开，
    // 但增速必须远低于预算增速」。
    //
    // **2026-09-05 第一次真的放开**：此前 `per_waves = 0`（恒 `base`）、
    // `cap = 1`，而实测它 58/60 波出场、只被击落 12 次（存活 79%），守方全程
    // 只有 2 座 `Flak` 对着 74 座 `Tower` ⇒ 那个「AA 的机会成本」两难
    // （`CLAUDE.md` 称「本作智斗最可读的载体」）从未成立过：1 只不死鸟不值
    // 一座防空塔，所以脚本不建、真人也不会建。
    //
    // **增速必须远低于预算增速**是结构约束、不是取值口味：兵力预算是
    // `w^1.25`，所以这里只能是「每 N 波 +1」这种线性且斜率很小的东西。
    // 取值仍是占位（进待标定清单）。
    int phoenix_base = 1;
    int phoenix_per_waves = 10;  // 0 = 恒 phoenix_base；否则每这么多波 +1
    int phoenix_cap = 5;

    // ——分兵——
    int main_permille = 700;   // 主攻拿几成（其余给佯攻）

    // **派几成的近战编队去打经济**（千分比，只算 `Ghoul` + `Knight`）。
    //
    // 攻方的战果此前全是「路过顺手」——flow 的唯一目标是 `keep`，60 波累计
    // 打掉 22 座采石场都是撞见了才打，而顺手已经把守方的采集建筑**钉在 6 座**。
    // 刻意打它就是掐守方唯一的成长引擎（石材 → K → 三个等级上限 + 塔），
    // 也就是 CLAUDE.md 的「攻其必救」。
    //
    // 占位值。它有真实的两头：太小则经济打击不成立，太大则主攻路人手不够、
    // 破不开墙（而破口才是拿下堡垒的前提）。
    int econ_raid_permille = 250;

    // 第 w 波的编成位与兵力预算。**唯一实现处**，`demo_driver.cpp` 与
    // `tools/balance/budget_curves.py` 都以它为准。
    int slots_at(int wave) const;
    double power_at(int wave) const;
};

// 攻方从**自己的迷雾**里读出来的东西。它是 `AttackerMacro` 唯一的情报入口，
// 单独成结构体是为了可测——测试可以直接断言「攻方以为守方长什么样」，
// 而不必从编成结果反推。
struct AttackerIntel {
    // 本波窥使拿到新情报了没有。false = 只有上一波（或更早）的记忆。
    bool fresh = false;
    // 记忆里城区内的防御工事。`towers` 含 `Tower`，`flaks` 单列——
    // 前者决定要不要多带攻城锤，后者决定要不要少派不死鸟，两条用途不同。
    int towers = 0;
    int flaks = 0;
    // 环上的格：记忆里还立着的墙/门，与**缺口**。
    //
    // 缺口的判据只能是 `fog.hpp` 写明的那条——`at() != Unseen &&
    // !remembered_bld()`。`remembered_bld()` 返回 false 有两种完全不同的含义
    // （「从未见过」与「见过、那里没有建筑」），只有后者是缺口，而这正是
    // 迷雾三态存在的理由。
    int walls = 0;
    int gaps = 0;
    // 记忆里的采集建筑位置，按地图文件里资源点的顺序（固定序，纪律 2）。
    std::vector<rts::GridPos> economy;
};

// 一族攻方宏观策略的参数。**全部是占位值。**
struct AttackerParams {
    // 关掉它 = 回到 2026-09-05 之前的行为（编成只看波数、不看守方），
    // 留着是为了 A/B：要证明「适应」真的改变了什么，得能关掉它跑一遍。
    bool adapt_composition = true;

    // 记忆里塔多到这个数以上，才开始加攻城锤。**不是「有塔就加」**：
    // 开局守方就有 2–3 座预置塔，那不构成「塔海」。
    int tower_ram_from = 8;
    // 每超出 `tower_ram_from` 这么多座塔，`ram_permille` 加 `ram_step_permille`。
    int tower_ram_per = 8;
    int ram_step_permille = 60;

    // **记忆里有缺口 ⇒ 大幅砍攻城锤**（千分比，600 = 砍掉六成）。
    //
    // 这一条对着 `CLAUDE.md` 的**头号评估指标**：「AI 是否发现并利用已有缺口」
    // ——它点名了对标产品 DiNaO 被差评的那一点（「敌人无视已有缺口、反复撞击
    // 加固过的城墙」），也是答辩要对照呈现的证据。墙上已经有洞的时候还带一堆
    // 攻城锤，正是那个差评描述的行为。
    int gap_ram_cut_permille = 600;

    // 三种非 `Ghoul` 战斗兵占编成位的比例上界（千分比）。`Ghoul` 是余量，
    // 而它是唯一的肉盾——不留够会让整波在塔的火力网里被点没。
    int specials_max_permille = 700;

    // 记忆里每有这么多座防空，就少派一只不死鸟。
    // **这是「AA 的机会成本」那个两难的攻方一半**：守方花在防空上的钱第一次
    // 真的买到了东西（此前不死鸟恒 1，建不建防空对攻方毫无影响）。
    int flak_per_phoenix_cut = 2;
};

// 一支编队能装几个同兵种单位。**逐兵种不同，理由各自不同**（2026-09-05 组长定）。
//
// 这个上限是**结构**而不是旋钮：`Knight` 的 2 来自冲锋助跑要跑道（挤在一起
// 互相挡住加速空间），`Ram`/`Phoenix` 的 1 来自它们各自的稀缺性与「单件威胁」
// 定位。改它们要回头看那三条机制，不是调个数。
constexpr int squad_cap_of(rts::UnitType t) noexcept {
    switch (t) {
        case rts::UnitType::Ghoul:   return 3;   // 肉盾，扎堆本来就是它的打法
        case rts::UnitType::Shade:   return 3;   // 中程压制，齐射式火力
        case rts::UnitType::Knight:  return 2;   // 冲锋助跑要跑道
        case rts::UnitType::Ram:     return 1;   // 单独成队
        case rts::UnitType::Phoenix: return 1;   // 单独成队
        case rts::UnitType::Wraith:  return 1;   // 本来就恒 1 只，天然单独
        default:                     return 1;   // 守方兵种：不成队（不上逐单位 RL）
    }
}

// 一波的编成计划。
//
// **2026-09-05 起这些数字是「编队数」而不是「单位数」。** 编成位（`slots_at`）
// 的语义随之改成编队数，因为它的上限来自 **RL 可训练的 agent 数**——而一支
// 编队就是一个 agent（动作复制给队里每个单位、观测取队长）。
//
// 这一步同时解决了「27 个单位硬顶」那个方案的副作用：单位数回到 ~70，于是
// 名义等级不会被摊到 165（那会把克制二部图稀释到 11%），实测 62 / 28%。
// 兑现战力 `Σ√B = √(单位数 × 预算)` 因此还涨了 1.61 倍。
//
// **编队一律同兵种**：一个动作要对整队有意义，而 `Ghoul`（近战 1.2）与
// `Shade`（中程）混编只能得到「肉盾被拖住」或「法师被推进近战」，后者正是
// CLAUDE.md 列为机制性克制的「被贴脸即废」。契约上也只能同兵种——
// `kObsSelfCount = kUnitTypeCount + 3`，自身向量里兵种是 one-hot，
// 混编队表达不了。
struct WavePlan {
    int ghouls = 0;      // 亡灵步兵**编队**数，每队 squad_cap_of() 个
    int shades = 0;
    int knights = 0;
    int rams = 0;
    int phoenixes = 0;
    int wraiths = 0;

    // 编队总数 = agent 数 = 受 `slots_cap` 约束的那个量。
    int squads() const noexcept {
        return ghouls + shades + knights + rams + phoenixes + wraiths;
    }

    // 场上单位总数。**它不等于编队数**，也不受 `slots_cap` 约束——
    // 约束 agent 数的理由（RL 可训练规模）不适用于单位数。
    int units() const noexcept {
        return ghouls * squad_cap_of(rts::UnitType::Ghoul) +
               shades * squad_cap_of(rts::UnitType::Shade) +
               knights * squad_cap_of(rts::UnitType::Knight) +
               rams * squad_cap_of(rts::UnitType::Ram) +
               phoenixes * squad_cap_of(rts::UnitType::Phoenix) +
               wraiths * squad_cap_of(rts::UnitType::Wraith);
    }
};

class AttackerMacro {
    friend struct SnapshotCodec;
public:
    explicit AttackerMacro(const MapData& map, const AttackerParams& params = {});

    // 从攻方视角读一次情报。**一波一次**（在 `spawn_wave()` 里），不是逐拍：
    // 它扫的是城区那 (2R+1)² 格 + 资源点，一波一次可以忽略不计，逐拍就不行了。
    //
    // `fresh` 由调用方给（`DemoBattle::wave_scouted()`）——本层不掺和
    // 「怎么算侦查到手」，那是 `Wraith` 行为那一侧的判据。
    AttackerIntel read_intel(const rts::WorldView& av, bool fresh) const;

    // 第 `wave` 波**预期的场上单位数**（不是编队数）。
    //
    // **它存在的唯一理由是「兵力预算按人头摊」那条反解**（`wave_level`）：
    // 预算要除以**单位数**，而 `slots_at()` 自编队制之后返回的是**编队数**。
    // 两者差约 2.58 倍，弄混就是把一支编队的预算发给队里每个成员——那正是
    // 2026-09-05 落地时踩的 bug（前期攻方强了 2.5 倍，第 4 波就陷落）。
    //
    // **刻意用空情报算**（`AttackerIntel{}`）：等级曲线应当是波数的纯函数，
    // 不该因为「这一波侦查到几座塔」而变——那会让难度取决于攻方的情报，
    // 而 `CLAUDE.md` 明令「适应只改风格，绝不改强度」。
    //
    // 不另写一份人数公式，直接走 `compose()`——同一件事写在两处必然漂移。
    int units_at(const WaveCurve& curve, int wave) const {
        return compose(curve, wave, AttackerIntel{}).units();
    }

    // 给定本波预算与情报，定编成。**纯函数**（不碰成员状态），好测。
    WavePlan compose(const WaveCurve& curve, int wave,
                     const AttackerIntel& intel) const;

    int ring_radius() const noexcept { return ring_r_; }
    const AttackerParams& params() const noexcept { return p_; }

private:
    AttackerParams p_;
    rts::GridPos keep_{};
    int ring_r_ = 0;

    // 城区内（含环）的所有格，行主序。构造时算好，此后不变（纪律 2）。
    std::vector<rts::GridPos> city_cells_;
    // 资源点位置，地图文件序。
    std::vector<rts::GridPos> resource_cells_;
};

}   // namespace game
