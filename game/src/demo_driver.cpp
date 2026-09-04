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

// 防空塔摆哪：**城门内侧**那一格（没有门则退回离堡垒最近的墙段内侧）。
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

std::int32_t wave_level(int wave, const rts::StatsTable& stats,
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
    const double per_unit = budget / static_cast<double>(curve.slots_at(wave));
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

int WaveCurve::slots_at(int wave) const {
    const double s = slots_base + slots_per_wave * static_cast<double>(wave - 1);
    int n = static_cast<int>(s);
    if (slots_cap > 0 && n > slots_cap) n = slots_cap;
    return n < 1 ? 1 : n;
}

double WaveCurve::power_at(int wave) const {
    const double w = static_cast<double>(wave);
    switch (power_form) {
        case PowerForm::Linear:
            return power_base * (1.0 + power_alpha * (w - 1.0));
        case PowerForm::Log:
            return power_base * (1.0 + power_alpha * std::log(w));
        case PowerForm::Saturating:
            // 后期趋平：w 小时近似 base·(1+alpha·w)，w >> half 时趋于常数。
            // 它存在的理由是「无尽模式必败」只要求**攻方不停涨**，不要求
            // 涨得比守方的饱和收入快无穷多——那条曲线的形状是待定的。
            return power_base *
                   (1.0 + power_alpha * w / (1.0 + w / (power_half > 0.0 ? power_half : 1.0)));
        case PowerForm::Power:
        default:
            return power_base * std::pow(w, power_alpha);
    }
}


DemoBattle::DemoBattle(const MapData& map, const rts::StatsTable& stats,
                       std::uint64_t seed, WaveTiming timing, WaveCurve curve,
                       DefenderSetup setup)
    : w_(demo_init(map, stats, seed, setup)),
      script_(ScriptParams{}, seed ^ 0x9e3779b9u),
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

    // 编成：把本波编成位**填满**（旧曲线各兵种各自封顶、总数钉死在 18，
    // 正是「攻方战力躺平」的另一半根因）。比例是占位（Shade 2 成 / Knight
    // 1.5 成 / Ram 1 成 / 余量全是 Ghoul），出场门槛沿用「由易到难」那条
    // 试玩结论：第 1 波只有 Ghoul（有测试钉着），Shade/Knight 第 2 波起、
    // Ram/Phoenix 第 3 波起。`Phoenix` 恒 1——「硬性数量上限」是结构
    // （CLAUDE.md「空中单位」），上限随波缓慢放开是后话，这里先取最小值；
    // 注意它与编成位那个撤下的硬顶不是一回事，这条不随之松动。
    const int slots = curve_.slots_at(wave);
    // 比例与门槛现在都从 `WaveCurve` 来（2026-09-03 起可在运行期设）；
    // 默认值与原来那几个整除式逐波相同（200‰ = /5、150‰ = ·3/20、100‰ = /10）。
    const auto share = [&](int permille) {
        return std::max(1, slots * permille / 1000);
    };
    const int shades = wave >= curve_.shade_from_wave ? share(curve_.shade_permille) : 0;
    const int knights =
        wave >= curve_.knight_from_wave ? share(curve_.knight_permille) : 0;
    const int rams = wave >= curve_.ram_from_wave ? share(curve_.ram_permille) : 0;
    // 不死鸟：`phoenix_per_waves == 0` ⇒ 恒 `phoenix_base`（现行行为）。
    const int phoenixes =
        wave >= curve_.phoenix_from_wave
            ? std::min(curve_.phoenix_cap,
                       curve_.phoenix_base +
                           (curve_.phoenix_per_waves > 0
                                ? (wave - curve_.phoenix_from_wave) /
                                      curve_.phoenix_per_waves
                                : 0))
            : 0;
    // 幽影窥使恒 1，第 2 波起。**这一只是整个侦查博弈里攻方那一半**：
    // 在它进编成之前，`Wraith` 在 `game/` 里只出现在中文展示名表里——攻方
    // 从来没有侦查过，于是「双向欺骗」只有守方那一向，而 CLAUDE.md 那三条
    // （AI 先侦查再定主攻方向、玩家猎杀 `Wraith` 让 AI 带错情报开打、
    // 玩家等 `Wraith` 走了再造防空）一条都无从发生。
    //
    // 从编成位里扣、不额外加人：它占的是攻方自己的预算，否则等于白送一只。
    const int wraiths = wave >= curve_.wraith_from_wave ? 1 : 0;
    const int ghouls =
        std::max(1, slots - shades - knights - rams - phoenixes - wraiths);
    std::vector<rts::UnitType> roster;
    roster.reserve(static_cast<std::size_t>(ghouls + shades + knights + rams +
                                            phoenixes + wraiths));
    for (int k = 0; k < ghouls; ++k) roster.push_back(rts::UnitType::Ghoul);
    for (int k = 0; k < shades; ++k) roster.push_back(rts::UnitType::Shade);
    for (int k = 0; k < knights; ++k) roster.push_back(rts::UnitType::Knight);
    for (int k = 0; k < rams; ++k) roster.push_back(rts::UnitType::Ram);
    for (int k = 0; k < phoenixes; ++k) roster.push_back(rts::UnitType::Phoenix);
    for (int k = 0; k < wraiths; ++k) roster.push_back(rts::UnitType::Wraith);

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
    for (const rts::UnitType t : roster) {
        const auto ti = static_cast<std::size_t>(t);
        const int idx = seen_of_type[ti]++;
        int total = 0;
        for (const rts::UnitType q : roster) total += (q == t) ? 1 : 0;

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
        w_.spawn_unit(t, rts::Vec2{c.x + off.first, c.y + off.second}, lv, hp, hp);
    }
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
    for (const rts::UnitId id : ids) w_.kill_unit(id);
}

// 斥候侦查：**到达集结点 → 掷一次死活 → 活着即侦查成功**。
//
// 与攻方 `Wraith` 的形状对称（组内 2026-09-04 定）：那一侧是「推进到看得见
// 防御布局 ⇒ 情报到手 ⇒ 掉头」，这一侧是「推进到集结点 ⇒ 掷点 ⇒ 活着就
// 拿到本波编成」。移动那一半是现成的——`DefenderScript` 的默认分支本来就让
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
    const auto& spawns = w_.view(rts::Side::Defender).spawns();
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

        // 活下来 ⇒ 侦查成功。**快照取全波编成**：它此刻就站在集结点上，
        // 那里正是本波部队待命的地方（集结期，`spawn_wave` 在建造期一开始
        // 就把整波生成好了）。
        scout_outcome_ = ScoutOutcome::Success;
        scout_report_.clear();
        int tally[rts::kUnitTypeCount] = {};
        std::vector<rts::UnitId> foes;
        w_.enumerate_units(rts::Side::Attacker, foes);
        for (const rts::UnitId f : foes) {
            ++tally[static_cast<std::size_t>(w_.unit_type(f))];
        }
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
    auto& slot = flow_[static_cast<std::size_t>(t) *
                           static_cast<std::size_t>(rts::kFlowTierCount) +
                       static_cast<std::size_t>(tier)];
    if (!slot) {
        const rts::GridPos goal[1] = {w_.keep_pos()};
        slot.emplace(rts::FlowField::compute(w_.view(rts::Side::Attacker), t, tier,
                                             goal, tiering_));
    }
    const rts::UnitAction a = slot->step_of(rts::grid_of(w_.unit_pos(id)));
    return a == rts::UnitAction::Stop
               ? greedy_move(id, rts::center_of(w_.keep_pos()))
               : a;
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
                wave_scouted_ = true;
                break;
            }
        }
    }
    // 二、**本波还有没有战斗单位活着**。这一条不是为了行为好看，是为了**波次循环
    //     不会卡死**：下一波挂在「攻方存活数归零」上，而 `Wraith` 撤回集结点之后
    //     不会死，于是它一个人就能让游戏永远停在这一波。所以当它落单时改为前压
    //     ——侦查任务此时已经没有意义（没有部队可以用得上这份情报了）。
    bool any_combat = false;
    for (const rts::UnitId id : ids_) {
        if (rts::is_combat(w_.unit_type(id))) {
            any_combat = true;
            break;
        }
    }
    // 最近的非墙建筑是不是 Keep——镜像 mechanics 的 AtkBld 选择（全场最近者；
    // 掩码位亮着就保证它在射程内，因为「有一座在射程内」蕴含「最近那座在
    // 射程内」）。Phoenix 用它绕开 Keep，理由见下。
    const auto nearest_bld_is_keep = [&](rts::Vec2 p) {
        bool found = false;
        bool keep = false;
        float best = 0.0f;
        for (std::size_t b = 0; b < av.bld_type().size(); ++b) {
            if (!av.bld_alive()[b]) continue;
            const rts::BldType t = av.bld_type()[b];
            if (t == rts::BldType::Wall || t == rts::BldType::Gate) continue;
            const rts::Vec2 c = rts::center_of(av.bld_pos()[b]);
            const float dx = c.x - p.x;
            const float dy = c.y - p.y;
            const float d2 = dx * dx + dy * dy;
            if (!found || d2 < best) {
                found = true;
                best = d2;
                keep = (t == rts::BldType::Keep);
            }
        }
        return found && keep;
    };

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
            const float s = st.of(t).speed;
            if (slowest < 0.0f || s < slowest) slowest = s;
        }
        // 同一兵种速度相同（读同一张表），浮点等值比较是精确的。
        for (const rts::UnitId id : ids_) {
            const rts::UnitType t = w_.unit_type(id);
            if (!rts::is_combat(t) || t == rts::UnitType::Phoenix) continue;
            if (st.of(t).speed != slowest) continue;
            const rts::Vec2 p = w_.unit_pos(id);
            const float dx = p.x - kc.x;
            const float dy = p.y - kc.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (pace_dist < 0.0f || d < pace_dist) pace_dist = d;
        }
    }
    for (const rts::UnitId id : ids_) {
        const std::uint16_t mask = w_.action_mask(id);
        rts::UnitAction a = rts::UnitAction::Stop;
        if (w_.unit_type(id) == rts::UnitType::Phoenix) {
            // Phoenix 单走一档（2026-09-01 试玩反馈：它原先与步兵同一套
            // 「AtkNear 否则奔堡垒」，于是径直飞进墙上弓手的火网、到了堡垒
            // 又因为射程内没有单位而干悬着——「手术刀」全程没切过一刀）：
            // 优先点杀射程内最脆的单位（AtkWeak，工匠/斥候先遭殃），其次
            // 俯冲最近的非墙建筑（AtkBld，点杀防御塔是设计明写的用途）。
            // **最近那座是 Keep 就不发**：CLAUDE.md「空中单位」明写它无法
            // 攻击核心建筑，而机制侧的 AtkBld 目前不区分 Keep（demo 从前
            // 不发 AtkBld，这条差异一直休眠）——demo 不该示范一个违反设计
            // 契约的行为，先在脚本侧绕开；机制侧要不要把 Keep 排除出
            // AtkBld，留给团队定。
            if (has(mask, rts::UnitAction::AtkWeak)) {
                a = rts::UnitAction::AtkWeak;
            } else if (has(mask, rts::UnitAction::AtkBld) &&
                       !nearest_bld_is_keep(w_.unit_pos(id))) {
                a = rts::UnitAction::AtkBld;
            } else {
                a = flow_step(id);
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
                const auto& spawns = w_.spawns();
                if (spawns.empty()) {
                    a = rts::UnitAction::Stop;
                } else {
                    const rts::Vec2 p = w_.unit_pos(id);
                    rts::Vec2 best = rts::center_of(spawns[0].pos);
                    float best_d2 = -1.0f;
                    for (const auto& s : spawns) {
                        const rts::Vec2 c = rts::center_of(s.pos);
                        const float dx = c.x - p.x;
                        const float dy = c.y - p.y;
                        const float d2 = dx * dx + dy * dy;
                        if (best_d2 < 0.0f || d2 < best_d2) {
                            best_d2 = d2;
                            best = c;
                        }
                    }
                    a = greedy_move(id, best);
                }
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
            const rts::Vec2 p = w_.unit_pos(id);
            const rts::Vec2 kc = rts::center_of(w_.keep_pos());
            const float dx = p.x - kc.x;
            const float dy = p.y - kc.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            a = (d < pace_dist - kFormationSlack) ? rts::UnitAction::Stop
                                                  : flow_step(id);
        } else {
            a = flow_step(id);
        }
        acts_.push_back(a);
    }
    w_.submit_actions(rts::Side::Attacker, acts_.data(), acts_.size());
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
            w_.begin_next_wave(wave_level(w_.wave() + 1, w_.stats(), curve_));
            build_left_ = timing_.build_ticks;
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
        w_.advance(1);
        // 斥候的到达判定：**每拍查一次**。它必须在 `advance` 之后——单位是
        // 在那里面移动的，判前查等于永远慢一拍。
        tick_scout_recon();
        ++since_decision_;
        if (!keep_alive()) defeated_ = true;   // 丢堡即败（设计，不是演示便宜）
    }
}

}  // namespace game
