#include "game/demo_driver.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

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

// 开局兵力全部经 `WorldInit` 进场（不再是构造后逐个 spawn）：初始局面因此
// 完整地由建局参数表达（回放 = 建局参数 + 输入流，这正是 `WorldInit::units`
// 存在的理由）。**不带任何归属标记**——编队系统移除后，守方单兵开局即自主
// （弓手会自己找墙登墙，见 `game/defender_script.hpp`），不再需要「摆到
// 墙内侧一格 + 下 Garrison 令」那套配合。
rts::WorldInit demo_init(const MapData& map, const rts::StatsTable& stats,
                         std::uint64_t seed) {
    rts::WorldInit init = make_world_init(map, stats, seed, /*nominal_level=*/1);
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
    // 箭楼与防空各一座（完工状态）。位置：堡垒斜后方两格，覆盖墙线。
    const auto bld = [&](rts::BldType b, int dx, int dy) {
        const std::int64_t hp = stats.of(b).max_hp;
        init.buildings.push_back(rts::BldInit{
            b,
            rts::GridPos{static_cast<std::int16_t>(init.keep.i + dx),
                         static_cast<std::int16_t>(init.keep.j + dy)},
            hp, hp});
    };
    bld(rts::BldType::Tower, 1, -2);
    bld(rts::BldType::Flak, 1, 2);

    // 攻方不在这里：波次循环生效后每波在建造阶段结束时生成（spawn_wave）。
    return init;
}

// ——波次循环的常量——
//
// 建造阶段时长仍是**占位**（待标定数值）；编成曲线自 2026-09-01 起不再是
// 「随波缓涨的占位混编」，而是双预算的 v1（见下面 wave_slots/wave_level）——
// 正式形态里编成由攻方宏观层按同一对预算**决策**，这里是它的无决策退化：
// 比例固定、预算全花。
//
// 2026-08-31 试玩反馈：波次节奏太紧，且第 1 波来得太快、玩家还没看清初始
// 布局。原先两处（首波前的建造倒计时、清波后到下一波的建造倒计时）共用同
// 一个 160 tick（8s），现在拆成两个常量分别调：波次间隔只需要「至少 +5s」，
// 首波额外再退后 10s（不是「间隔 +5s 之后再退后 10s」，是各自独立地从原先
// 那个 8s 起算）。
constexpr int kBuildTicksPlaceholder = 260;        // 13 秒 @ 20 Hz（原 8s，波次间隔 +5s）
constexpr int kFirstBuildTicksPlaceholder = 360;   // 18 秒 @ 20 Hz（原 8s，首波延后 10s）

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
// 区间」，没给出本项目的那个数，demo 不该钉一个假数。撤下它**不改难度曲线**：
// 名义等级按预算反解，总战力 ≡ 兵力预算，上限只改「人数 × 等级」的分配；
// 等 `bindings/`+`train/` 实测出可训练规模后再把硬顶接回来（接回时也只改
// 分配）。「必须有硬上限」这一结构主张本身没撤，撤的是没有依据的取值。
//
// 系数与 `tools/balance/budget_curves.py` 的 PLACEHOLDER 同源（那边是模型与
// 结构检查，这边是唯一的消费者；调系数两处一起改）。两条是结构、不是旋钮：
//
//   * **兵力预算超线性（α > 1）**——「城内保底收入线性、波次强度超线性」
//     那个拐点的存在性靠它；守方收入率有上界（资源点有限），所以交叉必然
//     发生且不可逆，这正是「无尽模式必败、看中位存活波数」的形状
//   * **名义等级由 cost ∝ 战力 反解**（L = 1 + (预算/编成位 − 1)/k，k 读
//     数值表的等级系数），**不是「预算 ÷ 编成位」**——战力 = 1 + k(L−1)
//     是仿射而非过原点的线性，直接除会让攻方总战力随波数次比例增长，
//     推导见 budget_curves.py 文件头「结构结论一/二」。
constexpr double kSlotsBase = 6.0;
constexpr double kSlotsPerWave = 1.2;
constexpr double kPowerBase = 6.0;
constexpr double kPowerAlpha = 1.25;

int wave_slots(int wave) {
    const double s = kSlotsBase + kSlotsPerWave * static_cast<double>(wave - 1);
    return static_cast<int>(s);
}

std::int32_t wave_level(int wave, const rts::StatsTable& stats) {
    // k 从数值表读（hp 与 dmg 两个系数相等由 StatsLoader 拦，取哪个都一样），
    // 不在这里抄一份 0.22——机制里不许藏数，那条纪律对 demo 曲线同样适用。
    const double k =
        static_cast<double>(stats.global.hp_permille_per_level) / 1000.0;
    if (k <= 0.0) return 1;
    const double budget =
        kPowerBase * std::pow(static_cast<double>(wave), kPowerAlpha);
    const double per_unit = budget / static_cast<double>(wave_slots(wave));
    // 除以第 1 波的人均预算（budget_curves.py 的 `base`）：把「1 级单位值多少
    // 预算」锚定在第 1 波 ⇒ L(1) = 1 由构造成立。当前系数下 base 恰是 1，
    // 但省略它的话「同源」就是假的——改 kPowerBase/kSlotsBase 时这里会静默
    // 丢掉那个锚点，而 Python 那边不会。
    const double base = kPowerBase / kSlotsBase;
    const double level = 1.0 + (per_unit / base - 1.0) / k;
    if (level < 1.0) return 1;
    return static_cast<std::int32_t>(level + 0.5);
}

}  // namespace

DemoBattle::DemoBattle(const MapData& map, const rts::StatsTable& stats,
                       std::uint64_t seed)
    : w_(demo_init(map, stats, seed)), script_(ScriptParams{}, seed ^ 0x9e3779b9u) {
    // 弓手登墙不再靠开局下命令：执行层脚本每个决策拍会自己找空墙段、
    // 发登墙意愿（`submit_garrison_wishes`），登墙由 `tick_garrison` 解算。
    // 开局资源（**占位数额**，无平衡含义）：交互层要能试建造，
    // 石木全零的话 Build 永远被解算拒绝，「建造放置」就没法演示。
    w_.set_stock(rts::Resource::Stone, 120);
    w_.set_stock(rts::Resource::Wood, 120);
    // 从建造阶段开始（World 的初始 phase 就是 Build）：倒计时走完才生波。
    build_left_ = kFirstBuildTicksPlaceholder;
    issue_actions();
}

// 本波编成（双预算曲线，见上面 wave_slots/wave_level）：编成位定人数、
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
    const int slots = wave_slots(wave);
    const int shades = wave >= 2 ? std::max(1, slots / 5) : 0;
    const int knights = wave >= 2 ? std::max(1, slots * 3 / 20) : 0;
    const int rams = wave >= 3 ? std::max(1, slots / 10) : 0;
    const int phoenixes = wave >= 3 ? 1 : 0;
    const int ghouls = std::max(1, slots - shades - knights - rams - phoenixes);
    std::vector<rts::UnitType> roster;
    roster.reserve(static_cast<std::size_t>(ghouls + shades + knights + rams + phoenixes));
    for (int k = 0; k < ghouls; ++k) roster.push_back(rts::UnitType::Ghoul);
    for (int k = 0; k < shades; ++k) roster.push_back(rts::UnitType::Shade);
    for (int k = 0; k < knights; ++k) roster.push_back(rts::UnitType::Knight);
    for (int k = 0; k < rams; ++k) roster.push_back(rts::UnitType::Ram);
    for (int k = 0; k < phoenixes; ++k) roster.push_back(rts::UnitType::Phoenix);

    // 固定的落位偏移环（不掷点：demo 的确定性不该依赖「生成时的随机散布」）。
    // 5×5、由内向外——编成位撤掉硬顶后单波会超过「集结点数 × 9」，原来的
    // 3×3 环在高波次会让后来者与先来者叠在同一坐标上生成。
    constexpr float kOff[][2] = {
        {0.0f, 0.0f},   {1.0f, 0.0f},   {-1.0f, 0.0f},  {0.0f, 1.0f},
        {0.0f, -1.0f},  {1.0f, 1.0f},   {-1.0f, -1.0f}, {1.0f, -1.0f},
        {-1.0f, 1.0f},  {2.0f, 0.0f},   {-2.0f, 0.0f},  {0.0f, 2.0f},
        {0.0f, -2.0f},  {2.0f, 1.0f},   {-2.0f, -1.0f}, {2.0f, -1.0f},
        {-2.0f, 1.0f},  {1.0f, 2.0f},   {-1.0f, -2.0f}, {1.0f, -2.0f},
        {-1.0f, 2.0f},  {2.0f, 2.0f},   {-2.0f, -2.0f}, {2.0f, -2.0f},
        {-2.0f, 2.0f}};
    constexpr std::size_t kOffCount = sizeof(kOff) / sizeof(kOff[0]);
    for (std::size_t n = 0; n < roster.size(); ++n) {
        const rts::Vec2 c = rts::center_of(spawns[n % spawns.size()].pos);
        const float* off = kOff[(n / spawns.size()) % kOffCount];
        const std::int64_t hp = hp_at(stats, roster[n], lv);
        w_.spawn_unit(roster[n], rts::Vec2{c.x + off[0], c.y + off[1]}, lv, hp, hp);
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
        } else if (has(mask, rts::UnitAction::AtkNear)) {
            a = rts::UnitAction::AtkNear;
        } else if (w_.unit_type(id) == rts::UnitType::Ram &&
                   has(mask, rts::UnitAction::AtkWall)) {
            a = rts::UnitAction::AtkWall;
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
        }
        // 生波挂在「进攻阶段且本波还没生」上，而不是「倒计时走完」上——
        // 玩家的 `Summon`（提前召唤）也会把 phase 掰到 Assault，两条路
        // 在这里汇合，不需要各生各的波。
        if (w_.phase() == rts::WavePhase::Assault) {
            if (!wave_spawned_) {
                spawn_wave();
                wave_spawned_ = true;
                issue_actions();   // 新生成的单位当拍拿到动作，不呆等一个决策周期
                since_decision_ = 0;
            } else if (w_.live_unit_count(rts::Side::Attacker) == 0) {
                // 本波打完（消耗殆尽也算，突破与否不改变循环）：进下一波建造。
                w_.begin_next_wave(wave_level(w_.wave() + 1, w_.stats()));
                build_left_ = kBuildTicksPlaceholder;
                wave_spawned_ = false;
            }
        }
        if (since_decision_ >= rts::kDecisionPeriodMax) {
            issue_actions();
            since_decision_ = 0;
        }
        w_.advance(1);
        ++since_decision_;
        if (!keep_alive()) defeated_ = true;   // 丢堡即败（设计，不是演示便宜）
    }
}

}  // namespace game
