#include "game/demo_driver.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "game/world_builder.hpp"
#include "rts/action.hpp"
#include "rts/combat_math.hpp"
#include "rts/command.hpp"
#include "rts/roster.hpp"
#include "rts/world_view.hpp"

namespace game {
namespace {

// 等级缩放后的满血。与 `rts_core` 的伤害走同一对函数（combat_math），
// 不另立一套公式。
std::int64_t hp_at(const rts::StatsTable& t, rts::UnitType u, std::int32_t level) {
    return rts::apply_permille(
        t.of(u).max_hp, {rts::level_permille(level, t.global.hp_permille_per_level)});
}

bool has(std::uint16_t mask, rts::UnitAction a) noexcept {
    return (mask & static_cast<std::uint16_t>(1u << static_cast<unsigned>(a))) != 0;
}

constexpr float kInvSqrt2 = 0.70710678f;

// 防空塔摆在墙线内侧，避开城门内三格、宽三格的通行区。
//
// **不写固定偏移**，理由见调用点。判据分三步，与 `place_inner_content` 摆预置塔
// 的做法同构（贴着墙线、避开已占的格）：
//
//   1. 候选墙段按 (是不是门, 到 `keep` 的切比雪夫距离) 排序 ⇒ 门优先
//   2. 从它沿「指向堡垒」的方向往里走一格，得到候选格
//   3. 候选被占（墙/预置建筑/资源点/已进 init 的建筑）或出界就换下一段墙再试
//
// **门优先不是审美偏好，是因为「最近」在方环上是退化的**：城圈是切比雪夫方环，
// 环上每一格到堡垒的距离**完全相同**，只按距离排等于「取文件里第一个」
// （实测落在拐角上）。而门是结构上的既定薄弱点（木质、破坏速率更高），
// 也是攻方最可能压过来的一段——地图自己的预置塔也正是贴着城门/缺口摆的。
//
// 返回空 = 这张图上找不到位置（没有初始墙，或内侧全被占满）。**那时就不摆**，
// 不退化成「随便找个地方放」——一座位置错的防空塔比没有更误导人。
std::optional<rts::GridPos> flak_site(const MapData& map, const rts::WorldInit& init) {
    const rts::GridPos keep = init.keep;
    const auto cheb = [&](rts::GridPos p) {
        const int dx = p.i - keep.i;
        const int dy = p.j - keep.j;
        return std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
    };
    // 排下标而不排 `WallSegment` 本身，且用 `stable_sort`：键相同时保持地图里的
    // 原始顺序（生成器按环序产出），于是结果是确定的——demo 必须可复现。
    std::vector<std::size_t> order;
    order.reserve(map.walls().size());
    for (std::size_t k = 0; k < map.walls().size(); ++k) order.push_back(k);
    const auto key = [&](std::size_t k) {
        const WallSegment& w = map.walls()[k];
        return std::pair<int, int>{w.kind == WallKind::Gate ? 0 : 1, cheb(w.pos)};
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return key(a) < key(b); });

    const auto occupied = [&](rts::GridPos p) {
        if (!map.in_bounds(p.i, p.j)) return true;
        if(map.gate_approach(p) || map.no_build_at(p.i,p.j) || map.terrain_at(p.i,p.j)!=Terrain::Plain) return true;
        if (map.wall_at(p.i, p.j) != nullptr) return true;
        if (p.i == keep.i && p.j == keep.j) return true;
        for (const BuildingNode& b : map.buildings()) {
            if (b.pos.i == p.i && b.pos.j == p.j) return true;
        }
        for (const ResourceNode& r : map.resources()) {
            if (r.pos.i == p.i && r.pos.j == p.j) return true;
        }
        // 已经摆进 init 的（堡垒、全部墙段、地图预置建筑）也要避开——
        // 同一格摆两座建筑在 `World` 里是没有定义的。
        for (const rts::BldInit& b : init.buildings) {
            if (b.pos.i == p.i && b.pos.j == p.j) return true;
        }
        return false;
    };

    for (const std::size_t k : order) {
        const rts::GridPos w = map.walls()[k].pos;
        // 「往堡垒方向走一格」：两轴各取符号，于是角上的墙走对角、边上的走正向。
        const int sx = (keep.i > w.i) ? 1 : (keep.i < w.i ? -1 : 0);
        const int sy = (keep.j > w.j) ? 1 : (keep.j < w.j ? -1 : 0);
        const rts::GridPos cand{static_cast<std::int16_t>(w.i + sx),
                                static_cast<std::int16_t>(w.j + sy)};
        if (!occupied(cand)) return cand;
    }
    return std::nullopt;
}

// 开局兵力全部经 `WorldInit` 进场（不再是构造后逐个 spawn）：初始局面因此
// 完整地由建局参数表达（回放 = 建局参数 + 输入流，这正是 `WorldInit::units`
// 存在的理由）。**不带任何归属标记**——编队系统移除后，守方单兵开局即自主
// （弓手会自己找墙登墙，见 `game/defender_script.hpp`），不再需要「摆到
// 墙内侧一格 + 下 Garrison 令」那套配合。
rts::WorldInit demo_init(const MapData& map, const rts::StatsTable& stats,
                         std::uint64_t seed, const DefenderSetup& setup) {
    rts::WorldInit init = make_world_init(map, stats, seed, /*nominal_level=*/1);
    // 人口上限是**建局输入**（`rts/world.hpp`），不在数值表里 ⇒ 唯一的入口
    // 就是这里。`make_world_init` 用的是 `WorldInit` 的默认 8+2K，这一行把
    // 调用方给的值盖上去。
    init.pop_cap_base = setup.pop_cap_base;
    init.pop_cap_per_keep_level = setup.pop_cap_per_keep_level;
    const auto add = [&](rts::UnitType u, float x, float y, std::int32_t lv) {
        const std::int64_t hp = hp_at(stats, u, lv);
        init.units.push_back(rts::UnitInit{u, rts::Vec2{x, y}, lv, hp, hp});
    };
    // 守方：三名弓手、两名枪卫、一名游骑与一名工匠，散开摆在堡垒周围。
    const rts::Vec2 keep = rts::center_of(init.keep);
    add(rts::UnitType::Archer, keep.x - 2.0f, keep.y - 2.0f, 2);
    add(rts::UnitType::Archer, keep.x - 3.0f, keep.y, 2);
    add(rts::UnitType::Archer, keep.x - 2.0f, keep.y + 2.0f, 2);
    add(rts::UnitType::Spear, keep.x + 2.0f, keep.y - 2.0f, 1);
    add(rts::UnitType::Spear, keep.x + 2.0f, keep.y + 2.0f, 1);
    add(rts::UnitType::Ranger, keep.x + 1.0f, keep.y, 1);
    add(rts::UnitType::Mason, keep.x + 1.0f, keep.y + 1.0f, 1);
    // 防空一座（完工状态），**摆在墙线内侧**。
    //
    // 2026-09-01：此前这里用固定偏移摆了 `Tower`(keep+(1,−2)) 与
    // `Flak`(keep+(1,2))，那是**单张固定地图时期的写法**（与 #110 修掉的
    // 「驻守目标写成堡垒正东三格」同源）。换成随机地图池之后 `city_radius`
    // 是 12–15，于是这两座离墙线 10–13 格，而 `Tower.range = 7`、
    // `Flak.range = 5` ⇒ **它们永远打不到墙线**，敌人进城五格才开火，
    // 画面上就是「两座塔莫名其妙贴着堡垒」。
    //
    // 两处修法不同：
    //
    //   * **`Tower` 直接删掉**——地图自己的预置塔（`place_inner_content`：
    //     2–3 座，贴城门/缺口内侧）位置本来就是对的，demo 不该再摆一座。
    //   * **`Flak` 保留但摆位从墙线推**：池图不预置防空，删了玩家就见不到
    //     AA 这件事，而 `Phoenix` 从第 3 波起就来。
    //
    // 长远归宿是把 `Flak` 也放进地图的 `buildings` 预置（走校验器第 24 条的
    // 摆放冲突检查），那要重生成全部池图——**PR #122 正在重生成同一批文件**，
    // 所以这轮先留在这里。
    if (const auto flak_at = flak_site(map, init)) {
        const std::int64_t hp = stats.of(rts::BldType::Flak).max_hp;
        init.buildings.push_back(rts::BldInit{rts::BldType::Flak, *flak_at, hp, hp});
    }

    // 攻方不在这里：波次循环生效后每波在**建造阶段一开始**生成（集结期，
    // 见 update() 顶部那段），不是开局摆好。
    return init;
}

// ——波次循环的常量——
//
// 建造阶段时长仍是**占位**（待标定数值）；编成曲线自 2026-09-01 起不再是
// 「随波缓涨的占位混编」，而是双预算的 v1（见 `game::WaveCurve` 与 wave_level）——
// 正式形态里编成由攻方宏观层按同一对预算**决策**，这里是它的无决策退化：
// 比例固定、预算全花。
//
// 2026-08-31 试玩反馈：波次节奏太紧，且第 1 波来得太快、玩家还没看清初始
// 布局。原先两处（首波前的建造倒计时、清波后到下一波的建造倒计时）共用同
// 一个 160 tick（8s），现在拆成两个常量分别调：波次间隔只需要「至少 +5s」，
// 首波额外再退后 10s（不是「间隔 +5s 之后再退后 10s」，是各自独立地从原先
// 那个 8s 起算）。
// **这两个数已提成运行期参数**（`game::WaveTiming`，2026-09-03）：波间隔是
// 平衡的关键旋钮之一（它同时管「每波收入」与「能安全施工多久」，理由见
// 那个结构体的注释），配平搜索必须够得着它。默认值就是这里原来的两个数。

// ——攻方双预算（2026-09-01 试玩反馈「20 分钟后玩家显著强过敌人」的修复）——
//
// 旧占位曲线的编成在第 12 波彻底封顶（8 Ghoul + 4 Shade + 1 Knight + 4 Ram +
// 1 Phoenix = 18 个），等级每 3 波才 +1 ⇒ 攻方战力此后近乎停涨，而守方收入是
// 恒定流量、复利无对手——交叉点恰落在 20 分钟上下。换成
// 《波次预算曲线与堡垒等级曲线.md》§1 的双预算：
//
//   编成位(w)   = 6 + 1.2·(w−1)              线性；**硬顶暂不设**，见下
//   兵力预算(w) = 6 · w^1.25                  超线性，无上限
//
// α 取 1.25，不是文档最初写的 1.6——1.6 在低波段太陡（第 3 波人均战力已是
// 首波的 4.3 倍，玩家还没起经济就被数值碾过去），而结构要求只是 α > 1。
// 1.25 下第 10 波总战力约为旧占位曲线停涨值的 3 倍多、且此后持续增长，
// 对准的正是「20 分钟后玩家反超」那个病灶。
//
// **编成位的硬顶撤下了（2026-09-01 用户定版）**：40 是随手标的，RL 实际能
// 支持多少单位没人测过——CLAUDE.md 引的文献只说明「存在一个不大的可训练
// 区间」，没给出本项目的那个数，demo 不该钉一个假数。~~撤下它**不改难度曲线**：
// 名义等级按预算反解，总战力 ≡ 兵力预算，上限只改「人数 × 等级」的分配~~
// ——订正（2026-09-02，详见 攻守实力模型与平衡分析.md §8 第 2 条）：被划掉的
// 读法是旧货币（ΣB）下的；硬顶改变兑现率（Σ√B，该文档 §2）与 Ram 阈值（§3），
// **改**难度。等 `bindings/`+`train/` 实测出可训练规模后再把硬顶接回来，
// 接回时难度曲线要随兑现率重估。「必须有硬上限」这一结构主张本身没撤，
// 撤的是没有依据的取值。
//
// 系数与 `tools/balance/budget_curves.py` 的 PLACEHOLDER 同源（那边是模型与
// 结构检查，这边是唯一的消费者；调系数两处一起改）。两条是结构、不是旋钮：
//
//   * **兵力预算超线性（α > 1）**——「城内保底收入线性、波次强度超线性」
//     那个拐点的存在性靠它；守方收入率有上界（资源点有限），所以交叉必然
//     发生且不可逆，这正是「无尽模式必败、看中位存活波数」的形状
//   * **名义等级由造价曲线反解**（L = 1 + (预算/编成位 − 1)/k，k 读
//     数值表的等级系数），**不是「预算 ÷ 编成位」**——战力 = 1 + k(L−1)
//     是仿射而非过原点的线性，直接除会让攻方总战力随波数次比例增长。
//     订正（2026-09-02，详见 攻守实力模型与平衡分析.md §8 第 3 条）：
//     公式保留、理由改——原注「由 cost ∝ 战力 反解」是为了让
//     「总战力 ≡ 预算由构造成立」（budget_curves.py 文件头结构结论二），
//     那条已被 §8 第 1 条订正为不成立（场上兑现是 Σ√B 不是 ΣB）；
//     本式现在的定位是攻方「兵力预算 ≡ 战力预算」定义下的人均预算折算
//     （§12.6：不是真扣钱，反解不跟守方 c ∝ √B 改）。
// **这四个常量已提成 `game::WaveCurve` 的字段**（2026-09-03）：兵力曲线是
// 配平的主旋钮之一，而编译期常量搜索够不着；连**函数形式**也一起开放了
// （`WaveCurve::PowerForm`），理由见那个结构体的注释。默认值就是原来这四个数。

// `units` = 本波**预期的场上单位数**，由调用方给（`AttackerMacro::units_at`）。
//
// **它必须是单位数，不能是编队数。** 这条 2026-09-05 出过事：编队制把
// `slots_at()` 的语义从「单位数」改成「编队数」，而这里原封不动地继续除它
// ⇒ 一支编队的兵力预算被发给了队里**每一个**成员，前期攻方战力凭空 ×2.5
// （第 4 波就拆了堡垒，而我一度把那个陷落当成编队制的成果报了出去）。
// 组长一句「你是不是把原先一个士兵的兵力直接变成了一个编队的兵力」点掉它。
//
// 参数名从 `curve.slots_at()` 改成显式传入，就是为了让这个错误写不出来：
// 调用方必须自己说清「我给的是几个单位」。
std::int32_t wave_level(int wave, int units, const rts::StatsTable& stats,
                       const WaveCurve& curve) {
    // k 从数值表读（hp 与 dmg 两个系数相等由 StatsLoader 拦，取哪个都一样），
    // 不在这里抄一份 0.22——机制里不许藏数，那条纪律对 demo 曲线同样适用。
    //
    // **这条反解假设人均造价 = base·B(L)（p = 1），而守方
    // `World::train_cost_gold` 自 2026-09-02 起是 c ∝ √B(L)。不跟着改**：
    // 这里是波次预算的反推（「兵力预算 ≡ 总战力」是 PR #124 建立的构造性
    // 等式，攻方那条预算按定义就是战力预算），不是真扣钱——§12.6 明写
    // 「不能顺手改，要单独想清楚」，两侧的差异归标定时校准。
    const double k =
        static_cast<double>(stats.global.hp_permille_per_level) / 1000.0;
    if (k <= 0.0) return 1;
    const double budget = curve.power_at(wave);
    const double per_unit =
        budget / static_cast<double>(units > 0 ? units : 1);
    // 除以第 1 波的人均预算（budget_curves.py 的 `base`）：把「1 级单位值多少
    // 预算」锚定在第 1 波 ⇒ L(1) = 1 由构造成立。当前系数下 base 恰是 1，
    // 但省略它的话「同源」就是假的——改 power_base/slots_base 时这里会静默
    // 丢掉那个锚点，而 Python 那边不会。
    const double base = curve.power_base / curve.slots_base;
    const double level = 1.0 + (per_unit / base - 1.0) / k;
    if (level < 1.0) return 1;
    return static_cast<std::int32_t>(level + 0.5);
}

}  // namespace

// `WaveCurve::slots_at` / `power_at` 2026-09-05 随结构体搬到
// `game/src/attacker_macro.cpp`。

DemoBattle::DemoBattle(const MapData& map, const rts::StatsTable& stats,
                       std::uint64_t seed, WaveTiming timing, WaveCurve curve,
                       DefenderSetup setup)
    : w_(demo_init(map, stats, seed, setup)),
      script_(ScriptParams{}, seed ^ 0x9e3779b9u),
      macro_(map, AttackerParams{}),
      timing_(timing),
      curve_(curve),
      setup_(setup),
      // 与 `script_` 同源派生、但**另取一个常数**：两条流各走各的，
      // 于是给脚本加一次掷点不会连带改掉侦查的结果（反之亦然）。
      recon_rng_(seed ^ 0x517cc1b7u) {
    // 弓手登墙不再靠开局下命令：执行层脚本每个决策拍会自己找空墙段、
    // 发登墙意愿（`submit_garrison_wishes`），登墙由 `tick_garrison` 解算。
    // 开局资源（**占位数额**，无平衡含义）：交互层要能试建造，
    // 石木全零的话 Build 永远被解算拒绝，「建造放置」就没法演示。
    w_.set_stock(rts::Resource::Stone, 120);
    w_.set_stock(rts::Resource::Wood, 120);
    // 从建造阶段开始（World 的初始 phase 就是 Build）。**集结期**（2026-09-02，
    // 《地图生成器大改方案.md》§4 第一行）：首波在建造阶段**一开始**就在集结点
    // 生成、待命到开打——不再挂在进攻阶段的第一拍。三件事由此才成立：
    // 建造阶段有可侦查的对象（`Wraith` 的活就在这段时间干）、HUD「攻方 N」不再
    // 建造期恒 0、免费方向提示给得出**行军之前**的调兵窗口（实力模型 §4 的那条
    // 唯一大杠杆——弓手集中——没有这个窗口就跨不过半圈）。
    build_left_ = timing_.first_build_ticks;
    spawn_wave();
    issue_actions();
}

// 本波编成（双预算曲线，见 `game::WaveCurve`）：编成位定人数、
// 兵力预算经名义等级定质量，轮流摆在各集结点周围。
// 骑士走开阔走廊一路直线 = 满动量冲锋（第四批）——首击明显重于互殴，正是要看的。
void DemoBattle::spawn_wave() {
    const auto& spawns = w_.spawns();
    if (spawns.empty()) return;
    const int wave = w_.wave();
    const std::int32_t lv = w_.nominal_level();
    const rts::StatsTable& stats = w_.stats();

    // ——编成归**攻方宏观决策层**（`game::AttackerMacro`，2026-09-05）——
    //
    // 此前这里是一串写死的整除式：比例是常数、`Phoenix` 恒 1、谁都不看守方
    // 长什么样。那让攻方成了一个**坏陪练**（`配平工作交接.md` §2.9/§2.10），
    // 与 2026-09-04 那次守方脚本重写同型的问题。
    //
    // **情报只过攻方自己的迷雾。** `read_intel` 读的是 `FogLayer` 里的记忆
    // 建筑，而记忆是跨波持久的 ⇒ 「玩家这一波新建了什么」只有在窥使活着回来
    // 之后才进得了编成。这不需要任何新机制，正是 CLAUDE.md「波次结构使 AI 的
    // 记忆天然过时」那条设计。
    //
    // **编成在波次开始时定死，不等本波侦查回来再改**（`波次分段与侦查时序.md`
    // §1.1）：否则玩家侦查到的编成会在她建造之后变掉，「花钱知道编成」这件事
    // 直接失去意义。所以这里传的 `fresh` 是**上一波**的侦查结果——本波的
    // `wave_scouted_` 此刻刚被重置成 false，用它只会恒假。
    wave_intel_ = recon_intel_;
    wave_intel_.fresh=prev_wave_scouted_;
    const WavePlan plan = macro_.compose(curve_, wave, wave_intel_);
    wave_plan_=plan;baseline_plan_=macro_.compose(curve_,wave,AttackerIntel{});

    // ——不死鸟的跨波结算（2026-09-10，#170）。**必须在展开编成之前**——
    //
    // 上一波结束时还挂在 `phoenix_id_of_` 上、却没进花名册的那些 = 被击落的。
    // 它们进重生队列；花名册里的则原样回来（当波等级、满血，见头文件那段）。
    settle_phoenix_roster();
    // 曲线给的是「这一波场上该有几只」，花名册已经占掉一部分名额。
    const int phoenix_returning = static_cast<int>(phoenix_roster_.size());
    const int phoenix_new =
        std::max(0, plan.phoenixes - phoenix_returning);

    // ——展开成编队：一支编队 = 同兵种 `squad_cap_of()` 个（2026-09-05）——
    //
    // `plan` 里的数字是**编队数**（= agent 数，受 `slots_cap` 约束），
    // 展开之后才是场上单位数（约 2.58 倍）。理由见 `WavePlan` 的注释：
    // 约束 agent 数的是 RL 可训练规模，它不适用于单位数。
    //
    // **编队归属就在这里定下来**，记在 `squad_of_` 里（与 `roster` 同序）。
    // 它不进 `World`/`Command`/回放——动作提交仍是逐单位的
    // （`submit_actions` 按 `enumerate_units` 序），编队只是「同一个动作发给
    // 队里每个单位」。所以这一整套不需要动 `rts_core` 一行。
    // **编队记账逐波清空。** 槽位会被复用（`SlotPool`），上一波死掉的单位留下
    // 的编队号若不清掉，新单位会继承它——症状是「两支不该相关的编队莫名一起
    // 动」，而且只在高波次（槽位真的被复用之后）才发作。
    std::fill(squad_of_.begin(), squad_of_.end(), -1);
    std::fill(squad_goal_.begin(), squad_goal_.end(), 0);

    // ——两个目标集（编队的意图选项）——
    //
    // 「主力打 Keep + 分队打经济」。**攻方的战果此前全是「路过顺手」**：
    // flow 的唯一目标是 `keep`，而 60 波累计打掉 22 座采石场就是撞见了才打。
    // 顺手已经把守方的采集建筑钉在 6 座 ⇒ 刻意打它就是掐守方唯一的成长引擎
    // （石材 → K → 人口/建筑/兵种三个等级上限 + 塔）。这就是 CLAUDE.md 的
    // 「攻其必救」，奖励函数也早已设计好（摧毁建筑的即时奖励 = 重建成本）。
    keep_goals_.assign(1, w_.keep_pos());
    // 经济目标只用**攻方记忆里见过的**采集建筑（`read_intel` 已经按地图文件序
    // 扫出来了），不读 god 视角——没侦查过的矿，攻方不该知道。
    econ_goals_ = wave_intel_.economy;
    econ_squads_.clear();

    std::vector<rts::UnitType> roster;
    std::vector<int> squad_id;      // 与 roster 同序：这个单位属于第几支编队
    roster.reserve(static_cast<std::size_t>(plan.units()));
    squad_id.reserve(static_cast<std::size_t>(plan.units()));
    int next_squad = 0;
    const auto add_squads = [&](rts::UnitType t, int squads) {
        const int per = squad_cap_of(t);
        for (int q = 0; q < squads; ++q) {
            const int sid = next_squad++;
            for (int m = 0; m < per; ++m) {
                roster.push_back(t);
                squad_id.push_back(sid);
            }
        }
    };
    add_squads(rts::UnitType::Ghoul, plan.ghouls);
    add_squads(rts::UnitType::Shade, plan.shades);
    add_squads(rts::UnitType::Knight, plan.knights);
    add_squads(rts::UnitType::Ram, plan.rams);
    // 回来的 + 新生的 = 曲线给的上限（`phoenix_new` 已减去花名册，见上）。
    // 两批都在这里展开，因为「哪一只是老的」由下面的落位循环按顺序认领
    // ——不死鸟一队一只（`squad_cap_of`），所以第 n 只不死鸟就是第 n 支
    // 不死鸟编队，顺序是确定的。
    add_squads(rts::UnitType::Phoenix, phoenix_returning + phoenix_new);
    // 幽影窥使恒 1，第 2 波起。**这一只是整个侦查博弈里攻方那一半**：
    // 在它进编成之前，`Wraith` 在 `game/` 里只出现在中文展示名表里——攻方
    // 从来没有侦查过，于是「双向欺骗」只有守方那一向。
    // 从编成位里扣、不额外加人：它占的是攻方自己的预算，否则等于白送一只。
    add_squads(rts::UnitType::Wraith, plan.wraiths);

    // ——把「打经济」这个意图分给一部分编队——
    //
    // **只分 `Ghoul` 与 `Knight`**（近战、便宜、能破结构），两条理由：`Ram` 是
    // 破墙主力必须跟主攻（那条逻辑在下面的分路里也写着）、`Shade` 的活是压制
    // 墙头；而每多一个 (兵种 × 目标集) 组合就多一张每拍重算的 field，
    // 那直接是训练吞吐（见 `kGoalSetCount` 的注释）。
    //
    // 取值是**占位**：这是脚本的固定分派，RL 宏观层接管后它变成每波一次的
    // 离散动作。经济目标集为空（没侦查到任何采集建筑）时整段不发生——
    // 那时 `goal_cells()` 会退回打堡垒。
    if (!econ_goals_.empty()) {
        int raiders = std::max(1, (plan.ghouls + plan.knights) *
                                      curve_.econ_raid_permille / 1000);
        for (std::size_t n = 0; n < roster.size() && raiders > 0; ++n) {
            const rts::UnitType t = roster[n];
            if (t != rts::UnitType::Ghoul && t != rts::UnitType::Knight) continue;
            const int sid = squad_id[n];
            // 一队只数一次（队里第一个成员那一格）。
            if (n > 0 && squad_id[n - 1] == sid) continue;
            econ_squads_.push_back(sid);
            --raiders;
        }
    }

    // ——兵力集中：主攻一路 + 佯攻一路，**不再轮转平摊**——
    //
    // 2026-09-02 试玩反馈：「敌人从四面八方来，每个方位就一点点人，一点压迫感
    // 都没有」。原写法是 `spawns[n % spawns.size()]` **轮转**派点 ⇒ 3–4 个集结点
    // 各分到大致相等的兵力。实测第 5 波总共 10 人、每路 2.5 人，摊在 2R+1 = 27
    // 格的受攻面上是 **0.09 人/格**——每 11 格墙才站 1 个敌人，「散兵游勇」是
    // 准确的描述。而 CLAUDE.md §1 末行早就预言了这个形态：「一座周长 150 格的城
    // 对 40 个攻方单位而言太稀，攻防会退化成散兵游勇」。
    //
    // 集中之后同一波的受攻面密度 ×4（见下面的比例）。**这是那条反馈里占比
    // 最大的一项**——实测「集中兵力」贡献 ×4，而「把城缩小」只贡献 ×1.5。
    //
    // 顺带修好另外两件本来就该成立的事：
    //
    //   * **分兵佯攻此前结构上不可能**（轮转 ⇒ 各路恒等），而 CLAUDE.md
    //     「集结区」把它列为攻方的欺骗手段之一
    //   * **「免费方向提示 = 兵力最多的集结点」此前由整数取余决定**（2026-09-01
    //     加的 HUD 那一行）——有主攻之后它才真的指向主攻
    //
    // **主攻方向按波数轮换**（`wave % 集结点数`），不掷点：demo 的确定性不该
    // 依赖随机数，而轮换保证玩家不能靠「永远守北面」蒙混过关。
    //
    // **佯攻不比主攻多**：CLAUDE.md 说 AI 可以「把佯攻部队堆得比主攻更多」让
    // 提示变成诱饵，但那是**宏观层要学的决策**；这里是它的无决策退化
    // （比例固定），所以提示在 demo 里始终诚实。欺骗留给 RL。
    const std::size_t n_spawn = spawns.size();
    const auto main_spawn = static_cast<std::size_t>(wave) % n_spawn;
    const std::size_t feint_spawn = (main_spawn + 1) % n_spawn;
    const int kMainPermille = curve_.main_permille;   // 主攻拿几成（运行期可设）

    // 固定的落位偏移环（不掷点，同上）。7×7 由内向外——集中之后单个集结点要
    // 摆下几乎整波人，原来的 5×5（25 个）在高波次会让后来者与先来者叠在
    // 同一坐标上生成。
    std::vector<std::pair<float, float>> off_ring;
    off_ring.reserve(49);
    for (int r = 0; r <= 3; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy) != r) continue;
                off_ring.emplace_back(static_cast<float>(dx), static_cast<float>(dy));
            }
        }
    }

    // 逐兵种分配，而不是按 roster 下标切一刀——切一刀会把排在后面的 `Ram`
    // 与 `Phoenix` 整体推给佯攻那一路，而 `Ram` 是破墙的主力，它必须跟主攻走。
    int placed_at[2] = {0, 0};   // [0]=主攻路已摆几个，[1]=佯攻路
    int seen_of_type[rts::kUnitTypeCount] = {};
    // 逐兵种总数**先数一遍**，不在循环里重数（原写法是 O(n²)；单位数从 76
    // 涨到约 70 之后仍然不是瓶颈，但没有理由留着）。
    int count_of_type[rts::kUnitTypeCount] = {};
    for (const rts::UnitType q : roster) ++count_of_type[static_cast<std::size_t>(q)];

    // **下标循环，不是 range-for**：下面要用 `n` 去 `squad_id` 里取编队号，
    // 而 range-for 的 `t` 是副本，拿它的地址算不出下标。
    for (std::size_t n = 0; n < roster.size(); ++n) {
        const rts::UnitType t = roster[n];
        const auto ti = static_cast<std::size_t>(t);
        const int idx = seen_of_type[ti]++;
        const int total = count_of_type[ti];

        bool to_main = true;
        if (t == rts::UnitType::Wraith) {
            // 侦查单位走另一路：它的活是看清防线，跟着主攻挤在一起看不到别处。
            to_main = false;
        } else if (t != rts::UnitType::Ram && t != rts::UnitType::Phoenix) {
            // `Ram`（破墙主力）与 `Phoenix`（空中、自己选目标）恒随主攻。
            // 其余按七成切，且**向上取整给主攻**——只有 1 个时不会全跑去佯攻。
            to_main = idx < (total * kMainPermille + 999) / 1000;
        }
        const std::size_t si = to_main ? main_spawn : feint_spawn;
        const int slot = placed_at[to_main ? 0 : 1]++;
        const auto& off = off_ring[static_cast<std::size_t>(slot) % off_ring.size()];
        const rts::Vec2 c = rts::center_of(spawns[si].pos);
        const std::int64_t hp = hp_at(stats, t, lv);
        // **编队号一并传进 `World`**（2026-09-05，`World/13 → World/14`）。
        //
        // 它此前只活在 `game/` 里（下面那个 `squad_of_`），而 `BatchedEnv` 在
        // `rts_core` 里、看不见 `game/` ⇒ RL 的观测按单位摊行、`kMaxUnitsPerEnv`
        // 还停在过期的 40，于是场上 ~70 个单位里有 30 个**存在、会挨打、但完全
        // 不受控且不被观测**（超出的那些被补 `Stop`）。而 agent = 编队是
        // `CLAUDE.md` 定的，所以编队号必须在 `World` 里。
        const rts::UnitId id = w_.spawn_unit(
            t, rts::Vec2{c.x + off.first, c.y + off.second}, lv, hp, hp,
            static_cast<std::uint16_t>(squad_id[n]));
        // `game/` 侧仍记一份，因为战术层要按它查同队（`squad_ahead`）。
        // **按槽位下标记账**（`Handle::index()`）：槽位会被复用，所以每波开头
        // 要清空（见上），否则上一波死掉的单位留下的编号会被新单位继承。
        const std::size_t ui = id.index();
        if (ui >= squad_of_.size()) squad_of_.resize(ui + 1, -1);
        if (ui >= squad_goal_.size()) squad_goal_.resize(ui + 1, 0);
        squad_of_[ui] = squad_id[n];
        squad_goal_[ui] =
            std::find(econ_squads_.begin(), econ_squads_.end(), squad_id[n]) !=
                    econ_squads_.end()
                ? 1
                : 0;
        // 不死鸟认领跨波身份：花名册里的老鸟先来（按 `idx` 顺序，与展开顺序
        // 一致且确定），用完之后的新鸟发新号。**身份跟着「第几只」走**，
        // 而它们的等级与血量都是当波的——留存不带任何数值继承，见头文件。
        if (t == rts::UnitType::Phoenix) {
            if (ui >= phoenix_id_of_.size()) phoenix_id_of_.resize(ui + 1, -1);
            phoenix_id_of_[ui] =
                idx < static_cast<int>(phoenix_roster_.size())
                    ? phoenix_roster_[static_cast<std::size_t>(idx)].id
                    : next_phoenix_id_++;
        }
    }
}

// 换波时结算不死鸟的去留。**在展开编成之前跑**（`spawn_wave` 开头）。
//
// 三件事，顺序要紧：
//
//   1. 重生倒计时 −1，数到 0 的从队列里出来（下面第 3 步会把它算进花名册名额）
//   2. **上一波还挂在 `phoenix_id_of_` 上的 = 被击落的**——撤离成功的那些在
//      `retire_phoenix` 里已经把这个槽位清成 −1 了，所以剩下的就是没走成的。
//      它们进重生队列，连续存活波数清零（《一跃》要的是**连续**）
//   3. `phoenix_id_of_` 整表清空，等这一波的落位循环重新认领
void DemoBattle::settle_phoenix_roster() {
    for (auto& [pid, left] : phoenix_respawn_) if (left > 0) --left;
    std::vector<int> reborn;
    for (const auto& [pid, left] : phoenix_respawn_)
        if (left <= 0) reborn.push_back(pid);
    std::erase_if(phoenix_respawn_,
                  [](const std::pair<int,int>& e) { return e.second <= 0; });

    for (std::size_t ui = 0; ui < phoenix_id_of_.size(); ++ui) {
        const int pid = phoenix_id_of_[ui];
        if (pid < 0) continue;
        // 没进花名册 ⇒ 上一波被打下来了。等 N 波，满血回来。
        std::erase_if(phoenix_roster_,
                      [pid](const PhoenixRecord& r) { return r.id == pid; });
        phoenix_respawn_.emplace_back(pid, curve_.phoenix_respawn_waves);
    }
    std::fill(phoenix_id_of_.begin(), phoenix_id_of_.end(), -1);

    // 重生回来的重新进花名册，连续存活从 0 起算。
    for (const int pid : reborn) phoenix_roster_.push_back(PhoenixRecord{pid, 0});
}

// 本波「打完」的判据。
//
// **原来写的是「攻方存活数归零」，那是个潜伏 bug，CLAUDE.md 早就写下来了**：
// 「波次循环挂在『攻方存活数归零』上，而会撤退的侦查单位不会死——一只就能让
// 游戏永远停在这一波。加任何『不求战』的单位时都要回头看这个终止条件。」
//
// 它此前从未发作，只是因为**守方总是先死**：无渲染跑的时候没人替守方下宏观
// 命令（全仓只有 `player_input.cpp` 产出建造/征兵），于是堡垒总在第 1–2 波
// 就掉了，循环从「陷落」那一侧退出。`game::DefenderMacro` 一上来（守方真的
// 守得住）它立刻变成硬停：实测第 2 波跑了 56682 tick，波末只剩一只 `Wraith`。
//
// 改成「攻方**还能打的**归零」。判据用 `rts::is_combat()` 而不是列举兵种名——
// 「哪些单位不求战」是花名册的性质（CLAUDE.md：攻方 `Wraith`、守方
// `Scout`/`Mason` 不进克制矩阵），列举一遍就是第二份真相。
bool DemoBattle::attacker_can_fight() const {
    const rts::WorldView v = w_.view(rts::Side::Attacker);
    const auto ut = v.unit_type();
    const auto alive = v.unit_alive();
    for (std::size_t k = 0; k < ut.size(); ++k) {
        if (!alive[k]) continue;
        if (rts::side_of(ut[k]) != rts::Side::Attacker) continue;
        if (rts::is_combat(ut[k])) return true;
    }
    return false;
}

// 换波时把还活着的攻方**非战斗**单位撤出场。
//
// 少了这一步，上面那条判据会让侦查单位**跨波累积**：每波恒生一只 `Wraith`，
// 二十波之后场上有二十只，而「第 2 波起恒一只」那条既有测试会当场变红。
// 叙事上它也自洽——它的活是看清防线，看完就该回去复命。
void DemoBattle::withdraw_all_attackers() {
    std::vector<rts::UnitId> ids;
    w_.enumerate_units(rts::Side::Attacker, ids);
    // **撤不是死**：超时收场的不死鸟也算撤离成功，进花名册。走这条路正是
    // 「本波打不动了、残兵撤退」，把它算成击落等于让「超时」白白惩罚攻方
    // 的空军——而那一侧的惩罚该由被击落来给（`phoenix_respawn_waves`）。
    for (const rts::UnitId id : ids) {
        if (w_.unit_type(id) == rts::UnitType::Phoenix) retire_phoenix(id);
        else w_.kill_unit(id);
    }
}

// ——不死鸟的撤离（2026-09-10，#170）——

// 飞回最近的集结点。**`Wraith` 与 `Phoenix` 共用这一个**：两者都是「事情办完
// 或办不下去就回去」，而「最近的集结点在哪」写两遍必然漂移（本仓库通篇在防
// 的东西）。`Wraith` 那份原本是内联的，这里提出来。
rts::UnitAction DemoBattle::retreat_action(rts::UnitId id) const {
    const auto& spawns = w_.spawns();
    if (spawns.empty()) return rts::UnitAction::Stop;
    const rts::Vec2 p = w_.unit_pos(id);
    rts::Vec2 best = rts::center_of(spawns[0].pos);
    float best_d2 = -1.0f;
    for (const auto& s : spawns) {
        const rts::Vec2 c = rts::center_of(s.pos);
        const float dx = c.x - p.x;
        const float dy = c.y - p.y;
        const float d2 = dx * dx + dy * dy;
        if (best_d2 < 0.0f || d2 < best_d2) { best_d2 = d2; best = c; }
    }
    return greedy_move(id, best);
}

// 这只不死鸟该撤了吗。两个条件，理由完全不同：
//
//   * **血量低于阈值** —— 这是「手术刀」那条设计本身（点一座塔、掉血就走、
//     下一波再来）。它同时让 `Flak` 第一次有对象：会撤的不死鸟才会被
//     「视野否定半径 > 伤害半径」赶走
//   * **本波已无地面战斗单位** —— 防波次循环卡死，与 `Wraith` 那条
//     `!any_combat` 同源。`CLAUDE.md` 点过名：「加任何『不求战』的单位时都要
//     回头看这个终止条件」，而一只会撤的不死鸟正好变成了那种单位
bool DemoBattle::phoenix_should_withdraw(rts::UnitId id,
                                         bool any_ground_combat) const {
    if (!any_ground_combat) return true;
    // 满血从**数值表 + 等级**重算（`hp_at`，与生成时同一条路），不去 `World`
    // 要一个 `unit_max_hp` 访问器：`rts_core` 不为 `game/` 的记账加接口，
    // 而这个值本来就是确定性地由 (兵种, 等级) 决定的。
    const std::int64_t max_hp =
        hp_at(w_.stats(), rts::UnitType::Phoenix, w_.unit_level(id));
    if (max_hp <= 0) return false;
    return w_.unit_hp(id) * 1000 < max_hp * curve_.phoenix_withdraw_hp_permille;
}

// 撤离完成：记进花名册，移出世界。
//
// **刻意「到达即离场」，而不是留一个「活着但不算数」的状态。** 后者要给
// `attacker_can_fight()` 加一条「撤离完成不算能打」，那正是 `CLAUDE.md` 点名
// 的那个坑的形状（会撤退的单位不会死 ⇒ 一只就能让游戏停在这一波）。单位不在
// 世界里，那条判据**结构上不需要改**——坑就不存在，而不是绕过去了。
// 每拍查一次「有没有正在撤的不死鸟已经到了集结点」。
//
// 判据与 `Scout` 那条到达判据同款（切比雪夫 ≤ 1 格）——飞行单位不必踩正中心，
// 而集结点周围本来就摆着一圈生成偏移。
void DemoBattle::tick_phoenix_withdrawal() {
    const auto& spawns = w_.spawns();
    if (spawns.empty()) return;
    std::vector<rts::UnitId> ids;
    w_.enumerate_units(rts::Side::Attacker, ids);
    // 先数地面战斗单位：撤离判据要它，而这一层不在 `issue_actions` 里。
    bool any_ground_combat = false;
    for (const rts::UnitId id : ids) {
        const rts::UnitType t = w_.unit_type(id);
        if (rts::is_combat(t) && t != rts::UnitType::Phoenix) {
            any_ground_combat = true;
            break;
        }
    }
    for (const rts::UnitId id : ids) {
        if (w_.unit_type(id) != rts::UnitType::Phoenix) continue;
        if (!phoenix_should_withdraw(id, any_ground_combat)) continue;
        const rts::GridPos g = rts::grid_of(w_.unit_pos(id));
        for (const auto& s : spawns) {
            const int di = g.i > s.pos.i ? g.i - s.pos.i : s.pos.i - g.i;
            const int dj = g.j > s.pos.j ? g.j - s.pos.j : s.pos.j - g.j;
            if (di <= 1 && dj <= 1) { retire_phoenix(id); break; }
        }
    }
}

void DemoBattle::retire_phoenix(rts::UnitId id) {
    const std::size_t ui = id.index();
    const int pid = ui < phoenix_id_of_.size() ? phoenix_id_of_[ui] : -1;
    if (pid >= 0) {
        int alive = 0;
        for (const PhoenixRecord& r : phoenix_roster_)
            if (r.id == pid) alive = r.waves_alive;
        // 花名册按身份去重：同一只在同一波里只该进一次。
        std::erase_if(phoenix_roster_,
                      [pid](const PhoenixRecord& r) { return r.id == pid; });
        phoenix_roster_.push_back(PhoenixRecord{pid, alive + 1});
        phoenix_id_of_[ui] = -1;
    }
    w_.kill_unit(id);
}

// 斥候侦查：**到达集结点 → 掷一次死活 → 活着即侦查成功**。
//
// 与攻方 `Wraith` 的形状对称（组内 2026-09-04 定）：那一侧是「推进到看得见
// 防御布局 ⇒ 情报到手 ⇒ 掉头」，这一侧是「推进到集结点 ⇒ 掷点 ⇒ 活着就
// 拿到它**看见的那部分**编成（所见即报，分兵后每一路是独立情报）」。移动
// 那一半是现成的——`DefenderScript` 的默认分支本来就让
// `Scout` 走向**最近的当前不可见的集结点**。
//
// 三条形状上的取舍，都是有意的：
//
//   * **二元，没有渐进渗漏。** 一版早先的实现按帧累积「看见过什么」，
//     于是斥候刚出城门瞟一眼就在漏情报、**侦查从来不会失败**。那把整个
//     `Scout` / `Wraith` 博弈消掉了：猎杀斥候没有意义，因为它死之前已经
//     漏了一路。侦查必须是成功或失败。
//   * **每只各掷一次独立的点。** 「派几只」因此是玩家的冗余决策
//     （两只 ⇒ `1 − p²`），与提案里攻方那一侧逐字同构。概率**不随敌方数量
//     缩放**：那会让后期侦查趋近必然失败，等于在最需要情报的时候关掉它。
//   * **报告是快照。** 情报的价值在「提前知道」，到手之后就不该再随战场
//     变化——那是记忆不是视野，所以它在交战期仍然可读。
void DemoBattle::tick_scout_recon() {
    // **直接问 `World`，不绕一个临时 `WorldView`。** `WorldView::spawns()` 返回的
    // 是 `w_->spawns()`（所有者是 `World`，比那个临时 view 活得长），所以绕一圈
    // 也不真悬垂——但 **GCC 的 `-Wdangling-reference` 证明不了这一点，会报错**
    // （`-Werror` 开着 ⇒ 服务器侧构建直接失败，而 MSVC 一声不响放过了）。
    // 这是 CLAUDE.md「GCC 侧的编译问题只能在推到服务器后才暴露」的一个实例。
    const auto& spawns = w_.spawns();
    if (spawns.empty()) return;
    // 0 = 用斥候视野（自维护，见头文件）。
    const int arrive =
        setup_.scout_arrive_cells > 0
            ? setup_.scout_arrive_cells
            : static_cast<int>(w_.stats().of(rts::UnitType::Scout).vision);

    std::vector<rts::UnitId> ids;
    w_.enumerate_units(rts::Side::Defender, ids);
    for (const rts::UnitId id : ids) {
        if (w_.unit_type(id) != rts::UnitType::Scout) continue;
        const std::uint32_t raw = id.raw();
        // 这一只本波掷过了？（按只记账，不是按波——见头文件）
        bool rolled = false;
        for (const std::uint32_t r : scout_rolled_) {
            if (r == raw) {
                rolled = true;
                break;
            }
        }
        if (rolled) continue;

        // 到没到？任意一个集结点都算——它走的是「最近的当前不可见的那个」。
        const rts::GridPos g = rts::grid_of(w_.unit_pos(id));
        bool arrived = false;
        for (const rts::SpawnSite& sp : spawns) {
            const int dx = static_cast<int>(g.i) - static_cast<int>(sp.pos.i);
            const int dy = static_cast<int>(g.j) - static_cast<int>(sp.pos.j);
            const int d = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
            if (d <= arrive) {
                arrived = true;
                break;
            }
        }
        if (!arrived) continue;

        // **所见即报**：分兵（主攻一路 + 佯攻一路）之后，「站在集结点上 =
        // 整波都在」不再成立——斥候走到的那一路可能只有部分兵力，甚至空无
        // 一兵（2026-09-05 试玩反馈：斥候没看见敌人，面板却报出全波编成）。
        // 先数它实际看见的攻方单位，只报这一部分。
        //
        // 看见圈 = 斥候视野 **+ 3**：`spawn_wave` 把单位摆在集结点周围 7×7
        // 环上（离中心最远 3 格），而到达判定是「斥候离中心 ≤ 视野」——斥候
        // 停在远侧时，不加这 3 格整支驻军都会落在它视野外，侦察永远落空。
        // 加完之后语义干净：**抵达哪个集结点，就看见驻守在那里的部队**。
        const int vision =
            static_cast<int>(w_.stats().of(rts::UnitType::Scout).vision);
        int tally[rts::kUnitTypeCount] = {};
        int seen_n = 0;
        std::vector<rts::UnitId> foes;
        w_.enumerate_units(rts::Side::Attacker, foes);
        for (const rts::UnitId f : foes) {
            const rts::GridPos fg = rts::grid_of(w_.unit_pos(f));
            const int dx = static_cast<int>(g.i) - static_cast<int>(fg.i);
            const int dy = static_cast<int>(g.j) - static_cast<int>(fg.j);
            const int d = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
            if (d <= vision + 3) {
                ++tally[static_cast<std::size_t>(w_.unit_type(f))];
                ++seen_n;
            }
        }
        if (seen_n == 0) {
            // 空集结点（本波没往这一路分兵）⇒ 没人会发现斥候：**不掷点、
            // 不记账**，它继续走向下一路。代价是斥候生存率略升（空路白探）
            // ——有意取舍：空营里本来就没有能杀死它的驻军。
            continue;
        }

        scout_rolled_.push_back(raw);
        const std::int32_t roll = static_cast<std::int32_t>(recon_rng_.below(1000));
        if (roll < setup_.scout_death_permille) {
            // 死了 ⇒ **本轮这一只侦查失败，一个字都不给**。
            // 只有在还没有别的斥候成功过时才把结论记成 `Killed`——
            // 一只成功一只战死，玩家该看到的是那份成功的报告。
            w_.kill_unit(id);
            if (scout_outcome_ == ScoutOutcome::None) {
                scout_outcome_ = ScoutOutcome::Killed;
            }
            continue;
        }

        // 活下来 ⇒ 侦查成功。报告就是它看见的那部分——佯攻分兵因此有了
        // 独立的情报价值（看清一路，另一路仍是迷雾）。
        scout_outcome_ = ScoutOutcome::Success;
        scout_report_.clear();
        // 按花名册顺序（`unit_at`），顺序必须稳定——面板每帧重排读不了。
        for (int k = 0; k < rts::kUnitTypeCount; ++k) {
            const rts::UnitType t = rts::unit_at(k);
            const int n = tally[static_cast<std::size_t>(t)];
            if (n > 0) scout_report_.push_back(SightedType{t, n});
        }
    }
}

void DemoBattle::withdraw_noncombat_attackers() {
    std::vector<rts::UnitId> ids;
    w_.enumerate_units(rts::Side::Attacker, ids);
    for (const rts::UnitId id : ids) {
        if (!rts::is_combat(w_.unit_type(id))) w_.kill_unit(id);
    }
}

bool DemoBattle::keep_alive() const {
    const rts::WorldView v = w_.view(rts::Side::Defender);
    for (std::size_t k = 0; k < v.bld_type().size(); ++k) {
        if (v.bld_alive()[k] && v.bld_type()[k] == rts::BldType::Keep) return true;
    }
    return false;
}

rts::UnitAction DemoBattle::greedy_move(rts::UnitId id, rts::Vec2 target) const {
    const rts::Vec2 p = w_.unit_pos(id);
    const std::uint16_t mask = w_.action_mask(id);
    const float dx = target.x - p.x;
    const float dy = target.y - p.y;
    rts::UnitAction best = rts::UnitAction::Stop;
    float best_dot = 0.0f;   // 只接受朝目标的方向；全被挡住就停
    for (int d = 0; d < rts::kMoveDirCount; ++d) {
        const rts::UnitAction a = rts::move_of(d);
        if (!has(mask, a)) continue;
        const rts::GridDelta g = rts::move_delta(a);
        const float norm = rts::is_grid_diagonal(a) ? kInvSqrt2 : 1.0f;
        const float dot = (dx * static_cast<float>(g.di) +
                           dy * static_cast<float>(g.dj)) * norm;
        if (dot > best_dot) {
            best_dot = dot;
            best = a;
        }
    }
    return best;
}

rts::UnitAction DemoBattle::flow_step(rts::UnitId id) {
    const rts::UnitType t = w_.unit_type(id);
    const int tier = rts::flow_tier_of(w_.unit_level(id), tiering_);
    // **目标集是编队的意图**（2026-09-05）：同一队读同一张 field，而每个成员在
    // **自己那一格**采样方向。这正是 flow field 该有的用法（SupCom2 那篇：
    // 「每个 agent 永远在自己那一格采样」），也是 TStarBot2 / ROMA 那条
    // 「group action 是共享子目标、不是共享输出」的落点。
    const std::size_t goals = static_cast<std::size_t>(squad_goal_of(id));
    auto& slot = flow_[(static_cast<std::size_t>(t) *
                            static_cast<std::size_t>(rts::kFlowTierCount) +
                        static_cast<std::size_t>(tier)) *
                           kGoalSetCount +
                       goals];
    const rts::GridPos fallback = goal_anchor(static_cast<int>(goals));
    if (!slot) {
        const std::vector<rts::GridPos>& gs = goal_cells(static_cast<int>(goals));
        slot.emplace(rts::FlowField::compute(w_.view(rts::Side::Attacker), t, tier,
                                             gs, tiering_));
    }
    const rts::UnitAction a = slot->step_of(rts::grid_of(w_.unit_pos(id)));
    return a == rts::UnitAction::Stop ? greedy_move(id, rts::center_of(fallback))
                                      : a;
}

// 这一队该读哪张 field。**目前是脚本的固定分派**（生波时定，见 `spawn_wave`）；
// RL 宏观层接管时它变成一个每波一次的离散动作，而下面这一层一行都不用改
// ——那正是「编队动作 = 选目标集」这个形状的全部好处。
int DemoBattle::squad_goal_of(rts::UnitId id) const {
    const std::size_t ui = id.index();
    if (ui >= squad_goal_.size()) return 0;
    const int g = squad_goal_[ui];
    return (g >= 0 && g < static_cast<int>(kGoalSetCount)) ? g : 0;
}

const std::vector<rts::GridPos>& DemoBattle::goal_cells(int set) const {
    // 经济目标集为空（这张图上一个采集建筑都没侦查到）⇒ 退回打堡垒。
    // **不能把空集交给 `FlowField::compute`**：空 goals 会让整张 field 全是
    // +inf、`step_of` 处处返回 Stop，那一队原地不动到本波结束。
    if (set == 1 && !econ_goals_.empty()) return econ_goals_;
    return keep_goals_;
}

rts::GridPos DemoBattle::goal_anchor(int set) const {
    const std::vector<rts::GridPos>& gs = goal_cells(set);
    return gs.empty() ? w_.keep_pos() : gs.front();
}

void DemoBattle::issue_actions() {
    // field 每个决策拍作废重算：破坏代价读的是当前墙血，决策拍之间它在变。
    for (auto& f : flow_) f.reset();

    // 守方：执行层脚本（拉扯 / 堵口不追 / 避骑士摸攻城锤 / 自动驻墙找活）。
    // 动作与登墙意愿是同一拍的两份输出，分别进两条输入通道。
    w_.enumerate_units(rts::Side::Defender, ids_);
    script_.decide(w_.view(rts::Side::Defender), ids_, acts_, wishes_);
    w_.submit_actions(rts::Side::Defender, acts_.data(), acts_.size());
    w_.submit_garrison_wishes(rts::Side::Defender, wishes_.data(), wishes_.size());

    // 攻方：占位脚本（正式形态是逐单位 RL）——优先打得着的人，Ram 优先砸墙，
    // 否则按 flow field 向堡垒推进（「绕远走缺口 vs 就近砸墙」由代价模型
    // 自己比较，field 指进墙格的那一步会变成自动破坏）。
    w_.enumerate_units(rts::Side::Attacker, ids_);
    acts_.clear();
    acts_.reserve(ids_.size());
    const rts::WorldView av = w_.view(rts::Side::Attacker);

    // ——`Wraith` 的两个判据，逐拍算一次，不逐单位重算——
    //
    // 一、**本波侦查到手了没有**：攻方迷雾里已经看见过任意一座**防御布局建筑**
    //     （塔 / 防空 / 堡垒）。2026-09-02 起**墙与门不算**（《地图生成器大改方案.md》
    //     §4：城墙在哪是免费信息——地形不过迷雾，墙环开局就看得见，「看见任意
    //     建筑就撤」等于在城外瞟一眼就回家，从来没侦查过）；资源建筑也不算
    //     （那是经济情报，不是布局）。要看见塔/防空/堡垒就得压到城圈附近——
    //     2026-09-03 起它会飞（数值表 `_note_2026_09_03`，速度回调 0.20）：
    //     地面单位结构上够不着它，但压到城圈意味着进入 `Flak` 与墙上
    //     `Archer` 的射程，「能被追上」由「会挨打」接任。
    //     用建筑数组而不是逐格扫 170×170 的迷雾图；条件在一波之内基本是单调的
    //     （格子一旦看过就不回到 `Unseen`），所以不会在边界上来回抖。
    if (!wave_scouted_) {
        const auto b_alive = av.bld_alive();
        const auto b_pos = av.bld_pos();
        const auto b_type = av.bld_type();
        const rts::FogLayer& af = av.fog();
        for (std::size_t b = 0; b < b_alive.size(); ++b) {
            if (b_alive[b] == 0) continue;
            const rts::BldType t = b_type[b];
            if (t != rts::BldType::Tower && t != rts::BldType::Flak &&
                t != rts::BldType::Keep) {
                continue;
            }
            const rts::GridPos p = b_pos[b];
            if (af.in_bounds(p.i, p.j) && af.at(p.i, p.j) == rts::Vis::Visible) {
                bool witnessed=false;
                for(std::size_t u=0;u<av.unit_type().size();++u) {
                    if(!av.unit_alive()[u] || av.unit_type()[u]!=rts::UnitType::Wraith) continue;
                    const auto up=av.unit_pos()[u];const auto center=rts::center_of(p);
                    const float r=w_.stats().of(rts::UnitType::Wraith).vision;
                    const float dx=up.x-center.x,dy=up.y-center.y;
                    if(dx*dx+dy*dy<=r*r) {witnessed=true;break;}
                }
                if(!witnessed) continue;
                wave_scouted_ = true;
                recon_intel_=macro_.read_intel(av,true);
                break;
            }
        }
    }
    // 二、**本波还有没有战斗单位活着**。这一条不是为了行为好看，是为了**波次循环
    //     不会卡死**：下一波挂在「攻方存活数归零」上，而 `Wraith` 撤回集结点之后
    //     不会死，于是它一个人就能让游戏永远停在这一波。所以当它落单时改为前压
    //     ——侦查任务此时已经没有意义（没有部队可以用得上这份情报了）。
    bool any_combat = false;
    // 三、**地面战斗单位还有没有**。给不死鸟的撤离判据用（见下面那一支）：
    //     它自己是战斗单位，用 `any_combat` 会把「只剩我一只」判成「还有仗打」
    //     ——那正是上一条要防的卡死，只是换了个兵种。
    bool any_ground_combat = false;
    for (const rts::UnitId id : ids_) {
        const rts::UnitType t = w_.unit_type(id);
        if (!rts::is_combat(t)) continue;
        any_combat = true;
        if (t != rts::UnitType::Phoenix) { any_ground_combat = true; break; }
    }
    // ——集结期待命与编队步速线（2026-09-02，《地图生成器大改方案.md》§4）——
    //
    // **集结期**：建造阶段里战斗单位在集结点待命（Stop），一个都不动——
    // 「建造阶段在集结区集结」是 CLAUDE.md 写着的形状，也让免费方向提示在
    // 行军之前就可用。待命只罩「否则就该 flow 推进」的那一支：有人送到脸上
    // （AtkNear 掩码亮）照打，`Wraith` 的侦查也照跑（它的活就该在这段干）。
    const bool muster = (w_.phase() == rts::WavePhase::Build);
    //
    // **编队推进**：开打后按**最慢兵种**齐步——到达离散（`Knight` 18 s 到、
    // `Ram` 61 s 到、一波分三批）是「没有压迫感」那条反馈的成因之一，而
    // 「等最慢的」正是 RL 阶段期望涌现的行为，占位脚本先把它写出来。
    // 步速线 = 最慢兵种里**最靠前**的那个到堡垒的距离；比它超前超过
    // `kFormationSlack` 格的地面战斗单位止步等它（有攻击目标的不等——
    // 接战优先，那是上面几支的事）。`Phoenix` 单走（空中、自己选目标）、
    // `Wraith` 单走（上面那一支），都不进步速线。
    //
    // **必须是「最靠前」而不能是「最靠后」**（2026-09-02 实测踩过）：取殿后者
    // 当步速线时，前排停在原地等它，而停下的人**会堵住路**——1 格宽的桥上
    // 被止步的前排塞死，殿后者永远走不上来，步速线因此永远不前移，全军
    // 在原地被塔火磨死（败局定格那条测试就是这么红的）。取前锋则死锁解不开
    // 的那一环不存在：殿后的慢兵永远在走，被挡的快兵在它接近到 slack 以内时
    // 已经恢复移动、把路让开。
    constexpr float kFormationSlack = 3.0f;   // 占位：队形松紧，格
    float pace_dist = -1.0f;
    {
        const rts::StatsTable& st = w_.stats();
        const rts::Vec2 kc = rts::center_of(w_.keep_pos());
        float slowest = -1.0f;
        for (const rts::UnitId id : ids_) {
            const rts::UnitType t = w_.unit_type(id);
            if (!rts::is_combat(t) || t == rts::UnitType::Phoenix) continue;
            // 正在破墙的攻城锤不再充当步兵的行军步速线。
            if(t==rts::UnitType::Ram && has(w_.action_mask(id),rts::UnitAction::AtkWall)) continue;
            const float s = st.of(t).speed;
            if (slowest < 0.0f || s < slowest) slowest = s;
        }
        // 同一兵种速度相同（读同一张表），浮点等值比较是精确的。
        for (const rts::UnitId id : ids_) {
            const rts::UnitType t = w_.unit_type(id);
            if (!rts::is_combat(t) || t == rts::UnitType::Phoenix) continue;
            // 正在破墙的攻城锤不再充当步兵的行军步速线。
            if(t==rts::UnitType::Ram && has(w_.action_mask(id),rts::UnitAction::AtkWall)) continue;
            if (st.of(t).speed != slowest) continue;
            const rts::Vec2 p = w_.unit_pos(id);
            const float dx = p.x - kc.x;
            const float dy = p.y - kc.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (pace_dist < 0.0f || d < pace_dist) pace_dist = d;
        }
    }

    // ——**步速线按整队判，不是按单兵判**（2026-09-05，编队制的连带）——
    //
    // 编队的动作最终会统一成队长那一个（见函数末尾）。若步速判断还留在单兵
    // 层，队长「我没越线、走」会**盖掉**队员「我越线了、该停」——于是越线的
    // 队员跟着一起走，散布越拉越大。这一条是实测逼出来的：`[demo]` 的
    // 「编队推进：行军途中前锋不甩开最慢兵种」在编队一致性上线后立刻红
    // （散布 11.86 > 允许的 11.72），把编队一致性关掉就绿。
    //
    // 判据取**队里最前的那个**（最保守）：任一队员越线，整队止步。于是
    // 「没有任何单位超出步速线」这条不变量在编队制下由构造成立。
    std::vector<std::uint8_t> squad_ahead;
    if (pace_dist > 0.0f) {
        const rts::Vec2 kc2 = rts::center_of(w_.keep_pos());
        for (const rts::UnitId id : ids_) {
            const std::size_t ui = id.index();
            if (ui >= squad_of_.size()) continue;
            const int sid = squad_of_[ui];
            if (sid < 0) continue;
            if (!rts::is_combat(w_.unit_type(id))) continue;
            const auto sq = static_cast<std::size_t>(sid);
            if (sq >= squad_ahead.size()) squad_ahead.resize(sq + 1, 0);
            const rts::Vec2 p2 = w_.unit_pos(id);
            const float ddx = p2.x - kc2.x;
            const float ddy = p2.y - kc2.y;
            if (std::sqrt(ddx * ddx + ddy * ddy) < pace_dist - kFormationSlack) {
                squad_ahead[sq] = 1;
            }
        }
    }

    // Opportunistic raids use only currently visible outer collection buildings.
    // Scouts keep scouting and rams already breaching a wall keep their assignment.
    struct RaidRoute {rts::UnitType type;int tier;rts::GridPos goal;rts::FlowField field;};
    std::vector<RaidRoute> raid_routes;
    const auto raid_view=w_.view(rts::Side::Attacker);
    std::vector<rts::GridPos> raid_sites;
    for(std::size_t k=0;k<raid_view.bld_type().size();++k) {
        if(!raid_view.bld_alive()[k] || !rts::is_gatherer(raid_view.bld_type()[k])) continue;
        const auto pos=raid_view.bld_pos()[k];
        for(const auto& site:w_.resources()) if(site.pos==pos && site.tier==rts::ResourceTier::Outer) {raid_sites.push_back(pos);break;}
    }
    for (const rts::UnitId id : ids_) {
        const std::uint16_t mask = w_.action_mask(id);
        rts::UnitAction a = rts::UnitAction::Stop;
        const auto type=w_.unit_type(id);
        const auto pos=w_.unit_pos(id);
        std::optional<rts::GridPos> raid;
        float raid_distance=w_.stats().of(type).vision*w_.stats().of(type).vision;
        if(rts::is_combat(type) && !(type==rts::UnitType::Ram && has(mask,rts::UnitAction::AtkWall))) {
            for(auto target:raid_sites) {const auto c=rts::center_of(target);const float dx=c.x-pos.x,dy=c.y-pos.y;const float d=dx*dx+dy*dy;if(d<=raid_distance) {raid_distance=d;raid=target;}}
        }
        if(raid) {
            const float range=w_.stats().of(type).range;
            if(raid_distance<=range*range && has(mask,rts::UnitAction::AtkBld)) a=rts::UnitAction::AtkBld;
            else if(!muster) {
                const int tier=rts::flow_tier_of(w_.unit_level(id),tiering_);
                auto route=std::find_if(raid_routes.begin(),raid_routes.end(),[&](const auto& r){return r.type==type && r.tier==tier && r.goal==*raid;});
                if(route==raid_routes.end()) {
                    raid_routes.push_back({type,tier,*raid,rts::FlowField::compute(raid_view,type,tier,std::vector<rts::GridPos>{*raid},tiering_)});
                    route=raid_routes.end()-1;
                }
                a=route->field.step_of(rts::grid_of(pos));
                if(a==rts::UnitAction::Stop) a=has(mask,rts::UnitAction::AtkNear)?rts::UnitAction::AtkNear:flow_step(id);
            }
        } else if (w_.unit_type(id) == rts::UnitType::Phoenix) {
            // Phoenix 单走一档（2026-09-01 试玩反馈：它原先与步兵同一套
            // 「AtkNear 否则奔堡垒」，于是径直飞进墙上弓手的火网、到了堡垒
            // 又因为射程内没有单位而干悬着——「手术刀」全程没切过一刀）：
            // 优先点杀射程内最脆的单位（AtkWeak，工匠/斥候先遭殃），其次
            // 俯冲最近的非墙建筑（AtkBld，点杀防御塔是设计明写的用途）。
            // 机制层排除不可伤害的堡垒；脚本层在无近身目标时追逐有效目标。
            //
            // ——**撤离优先于一切攻击**（2026-09-10，#170）——
            //
            // `CLAUDE.md` 给不死鸟定的是「手术刀，不是胜利条件」+ 跨波留存。
            // 掉血就走、下一波再来，这才是手术刀的形状；打到死是消耗品的形状，
            // 而消耗品换不来「AA 的机会成本」那个两难（守方 2 座 `Flak` 对
            // 74 座 `Tower` 就是实测结果）。
            if (phoenix_should_withdraw(id, any_ground_combat)) {
                a = retreat_action(id);
            } else if (has(mask, rts::UnitAction::AtkWeak)) {
                a = rts::UnitAction::AtkWeak;
            } else if (has(mask, rts::UnitAction::AtkBld)) {
                a = rts::UnitAction::AtkBld;
            } else {
                // 无法伤害堡垒；追逐可攻击的单位或非核心建筑，不能以堡垒为落点。
                const auto p=w_.unit_pos(id);rts::Vec2 goal=p;float nearest=-1.0f;
                const auto consider=[&](rts::Vec2 q) {const float dx=q.x-p.x,dy=q.y-p.y,d=dx*dx+dy*dy;if(nearest<0 || d<nearest){nearest=d;goal=q;}};
                for(std::size_t k=0;k<av.unit_alive().size();++k)
                    if(av.unit_alive()[k] && rts::side_of(av.unit_type()[k])==rts::Side::Defender) consider(av.unit_pos()[k]);
                for(std::size_t k=0;k<av.bld_alive().size();++k)
                    if(av.bld_alive()[k] && av.bld_type()[k]!=rts::BldType::Keep && av.bld_type()[k]!=rts::BldType::Wall && av.bld_type()[k]!=rts::BldType::Gate) consider(rts::center_of(av.bld_pos()[k]));
                if(nearest<0) for(const auto& spawn:w_.spawns()) consider(rts::center_of(spawn.pos));
                a=nearest>=0?greedy_move(id,goal):rts::UnitAction::Stop;
            }
        } else if (w_.unit_type(id) == rts::UnitType::Wraith) {
            // 幽影窥使：**无战力**（`is_combat()` 为假 ⇒ 攻击掩码永远不亮），
            // 所以不能照步兵那套「打得着就打、否则奔堡垒」走——那会让它一路
            // 走到墙下被射死，侦查一次都没成功过。
            //
            // 行为是「看到了就撤」：还没侦查到手就按 flow 前压，到手之后掉头
            // 回最近的集结点。撤回去正是设计要的形状——玩家由此有机会猎杀它
            // （`CLAUDE.md`「双向欺骗」：杀掉 `Wraith` 让 AI 带着错误情报开打），
            // 而它活着走掉则意味着 AI 这一波真的看清了防御布局。
            //
            // `!any_combat` 那一支见上面，是防波次循环卡死的，不是行为设计。
            if (!wave_scouted_ || !any_combat) {
                a = flow_step(id);
            } else {
                a = retreat_action(id);
            }
        } else if (has(mask, rts::UnitAction::AtkNear)) {
            a = rts::UnitAction::AtkNear;
        } else if (w_.unit_type(id) == rts::UnitType::Ram &&
                   has(mask, rts::UnitAction::AtkWall)) {
            a = rts::UnitAction::AtkWall;
        } else if (muster) {
            // 集结期：待命。能走到这里说明没有任何攻击目标在脸上（上面几支
            // 先判过了），所以 Stop 是「列队等开打」，不是「挨打不还手」。
            a = rts::UnitAction::Stop;
        } else if (pace_dist > 0.0f &&
                   rts::is_combat(w_.unit_type(id))) {
            // 编队推进：比步速线超前超过 slack 就止步等后排（理由见上面那段）。
            // **成队的按整队判**（`squad_ahead`，队里最前那个说话）；散兵
            // 按自己判。
            const std::size_t ui = id.index();
            const int sid = ui < squad_of_.size() ? squad_of_[ui] : -1;
            bool ahead = false;
            if (sid >= 0 && static_cast<std::size_t>(sid) < squad_ahead.size()) {
                ahead = squad_ahead[static_cast<std::size_t>(sid)] != 0;
            } else {
                const rts::Vec2 p = w_.unit_pos(id);
                const rts::Vec2 kc = rts::center_of(w_.keep_pos());
                const float dx = p.x - kc.x;
                const float dy = p.y - kc.y;
                ahead = std::sqrt(dx * dx + dy * dy) < pace_dist - kFormationSlack;
            }
            a = ahead ? rts::UnitAction::Stop : flow_step(id);
        } else {
            a = flow_step(id);
        }
        acts_.push_back(a);
    }

    // ——**编队怎么落到单位上：共享意图，不共享方向**（2026-09-05）——
    //
    // 我先写错了一版，值得留着当反面教材：**把队长选的那个动作原样抄给队员**。
    // 它当场把 `[demo]` 的队形测试打红（散布 11.86 > 允许的 11.72），
    // 而组长一句「你做编队就要考虑这些啊，去搜索一下开源代码对编队的实现」
    // 让我去查了文献 —— 结论是那个做法**有明确记录是错的**，不是没人试过：
    //
    //   * **TStarBot2**（全局 SC2 bot）把它写进自己的 ablation：给单个控制器
    //     共享的 group macro-action 会失败，因为「macro actions don't have
    //     control over individual units, which is inflexible」。它的解法是两层
    //     ——上层发**意图/目标**，下层**以那个意图为条件**各自算逐单位动作。
    //   * **ROMA / RODE**（role-based MARL）同形：role 是一个**条件输入的隐
    //     向量**，每个 agent 仍从自己的局部状态解码出自己的动作。
    //   * **Supreme Commander 2 的 flow field 原文**（Emerson, Game AI Pro
    //     Ch.23）在寻路侧给出同一个答案：**每个 agent 永远在自己那一格采样**，
    //     整条流水线里没有任何广播步骤。
    //   * **OpenRA 根本没有编队系统**（各自下令各自寻路）；**0 A.D.** 的
    //     `FormationController` 是一个独立实体供成员跟随；**Spring/BAR** 给
    //     **每个单位算各自的目标点**、各自寻路。三个引擎都不广播方向。
    //
    // 一句话：**group action 从不是单位的字面输出**，它是一个共享的子目标 /
    // 条件变量，喂进每个成员自己的动作头。
    //
    // 所以这里**什么都不做**。编队体现在两处，都已经在别处了：
    //
    //   1. **共享目标集**（意图）：同一队读同一张 flow field，而每个成员在
    //      **自己那一格**采样方向。落点是 `flow_step()` 的缓存键——那一维
    //      正是「打 Keep / 打经济」要加的那个，编队动作就是「这一队读哪张」。
    //   2. **向最慢的成员限速**（凝聚）：`squad_ahead`，任一队员越线则整队
    //      止步。这是 BAR 的 locked-formation 那条规则的离散时间版本，
    //      也是**唯一**与「各自采样」兼容且有出货先例的凝聚手段。
    //
    // ⚠️ **刻意没做「虚拟队心 + 成员偏移」**：那是唯一被明确报告在 flow field
    // 上失败的方案（有团队试过 formation forces 叠加在 flowfield 之上，
    // 最后整个放弃了 flow field、退回 waypoint A*）。我们已经在 flow field
    // 上，所以那条路排除。代价要认下来：画面上不会出现整齐的三人小队，
    // 编队是「共享意图、各自走」。那种视觉编队要换回 waypoint A*。

    w_.submit_actions(rts::Side::Attacker, acts_.data(), acts_.size());
}

void DemoBattle::enable_developer() {
    w_.enable_developer();
    for(int r=0;r<rts::kResourceCount;++r) w_.set_stock(static_cast<rts::Resource>(r),1000000000);
}
void DemoBattle::developer_wave(int wave) {
    if(!developer() || wave<1 || wave>9999) return;
    withdraw_all_attackers();
    w_.developer_wave(wave,wave_level(wave,macro_.units_at(curve_,wave),w_.stats(),curve_));
    build_left_=timing_.build_ticks;build_start_=w_.now();assault_ticks_=0;
    wave_scouted_=false;prev_wave_scouted_=false;scout_outcome_=ScoutOutcome::None;
    scout_report_.clear();scout_rolled_.clear();spawn_wave();issue_actions();since_decision_=0;
}

void DemoBattle::update(int ticks) {
    for (int k = 0; k < ticks; ++k) {
        if (defeated_) return;   // 败局定格：世界停在最后一帧
        if (w_.phase() == rts::WavePhase::Build && build_left_ > 0) {
            --build_left_;
        } else if (w_.phase() == rts::WavePhase::Build) {
            w_.begin_assault();
            // 开打这一拍重新下令：集结期待命的全队从 Stop 换成推进。
            // （玩家的 `Summon` 也会把 phase 掰到 Assault——那条路不经过这里，
            // 但待命中的单位最多再呆一个决策周期（8 tick）就会拿到新命令，
            // 不另开一条通道。）
            issue_actions();
            since_decision_ = 0;
        }
        if (w_.phase() == rts::WavePhase::Assault) ++assault_ticks_;
        // 撤离到位的不死鸟离场（2026-09-10，#170）。**必须在下面那条终止判据
        // 之前**：它自己是战斗单位，不先离场就永远等不到「攻方还能打的归零」。
        if (w_.phase() == rts::WavePhase::Assault) tick_phoenix_withdrawal();
        const bool timed_out = timing_.assault_max_ticks > 0 &&
                               assault_ticks_ >= timing_.assault_max_ticks;
        if (w_.phase() == rts::WavePhase::Assault &&
            (!attacker_can_fight() || timed_out)) {
            // 超时 ⇒ 残兵（含还能打的）一起撤；正常清波 ⇒ 只撤不求战的那些。
            if (timed_out) withdraw_all_attackers();
            assault_ticks_ = 0;
            // 本波打完（消耗殆尽也算，突破与否不改变循环）：进下一波建造，
            // 且**新一波动即生成**——集结期（见构造函数那段）：建造阶段一开始
            // 他们就在集结点待命，侦查与方向提示因此有一整个建造阶段可用。
            withdraw_noncombat_attackers();
            const int nw = w_.wave() + 1;
            w_.begin_next_wave(
                wave_level(nw, macro_.units_at(curve_, nw), w_.stats(), curve_));
            build_left_ = timing_.build_ticks;
            build_start_ = w_.now();   // Summon 护栏的计时起点（见头文件）
            // **先结转再清零**：下一波的编成要读上一波的侦查结果
            // （`spawn_wave()` 里那条 `prev_wave_scouted_`），而重置之后
            // `wave_scouted_` 恒假。
            prev_wave_scouted_ = wave_scouted_;
            wave_scouted_ = false;   // 新的一波要重新侦查（记忆天然过时）
            // 守方一侧同理：上一波的编成不算情报（CLAUDE.md「波次结构使
            // AI 的记忆天然过时」，那条对玩家一样成立）。
            scout_outcome_ = ScoutOutcome::None;
            scout_report_.clear();
            scout_rolled_.clear();
            spawn_wave();
            issue_actions();   // 新生成的单位当拍拿到动作，不呆等一个决策周期
            since_decision_ = 0;
        }
        if (since_decision_ >= rts::kDecisionPeriodMax) {
            issue_actions();
            since_decision_ = 0;
        }
        if(developer()) enable_developer();
        w_.advance(1);
        // 斥候的到达判定：**每拍查一次**。它必须在 `advance` 之后——单位是
        // 在那里面移动的，判前查等于永远慢一拍。
        tick_scout_recon();
        ++since_decision_;
        if (!keep_alive()) defeated_ = true;   // 丢堡即败（设计，不是演示便宜）
    }
}

}  // namespace game
