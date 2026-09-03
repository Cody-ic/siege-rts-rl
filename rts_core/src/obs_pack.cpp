#include "rts/obs_pack.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "rts/action.hpp"
#include "rts/fog.hpp"
#include "rts/roster.hpp"
#include "rts/world.hpp"

namespace rts {
namespace {

// 通道在最后一维：cells[(dy * K + dx) * C + ch]。
inline std::size_t at(int dx, int dy, ObsChannel ch) noexcept {
    return (static_cast<std::size_t>(dy) * static_cast<std::size_t>(kObsK) +
            static_cast<std::size_t>(dx)) *
               static_cast<std::size_t>(kObsChannelCount) +
           static_cast<std::size_t>(ch);
}

inline float frac(std::int64_t hp, std::int64_t max_hp) noexcept {
    if (max_hp <= 0) return 0.0f;
    const float f = static_cast<float>(hp) / static_cast<float>(max_hp);
    return std::clamp(f, 0.0f, 1.0f);
}

// 墙 / 门 / 其余建筑各占一条通道（`obs.hpp`：门单独一条，因为它是结构上的
// 既定薄弱点，混进 `wall_hp` 里 AI 就分不出「这里更好打」）。
inline ObsChannel channel_of_bld(BldType t) noexcept {
    if (t == BldType::Wall) return ObsChannel::WallHp;
    if (t == BldType::Gate) return ObsChannel::GateHp;
    return ObsChannel::BldHp;
}

}  // namespace

void pack_globals(const WorldView& view, const ObsNorms& norms,
                  std::span<float> globals) {
    if (globals.size() != static_cast<std::size_t>(kObsGlobalFloats)) {
        throw ContractError("pack_globals: globals 长度必须恰好是 kObsGlobalFloats");
    }
    // `log2(1 + wave)`，不是 `wave`——理由在 `obs.hpp` 的 `WaveLog` 那段：
    // 线性波数在第 100 波是第 10 波的 10 倍，输入尺度漂移会毁掉课程迁移，
    // 而任何形如 `wave / 截断波数` 的写法又把观测绑在一个待定数上。
    const float w = static_cast<float>(view.wave());
    globals[static_cast<std::size_t>(ObsGlobal::WaveLog)] =
        std::log2(1.0f + std::max(0.0f, w));

    // 存活空军 ÷ 本波上限。2026-09-03 起 `Wraith` 算空军（第 2 波起恒一进
    // demo 编成），这条通道随它取值；见 `ObsNorms::aerial_cap` 那段注释。
    int aerial = 0;
    const auto alive = view.unit_alive();
    const auto type = view.unit_type();
    for (std::size_t k = 0; k < alive.size(); ++k) {
        if (alive[k] != 0 && is_aerial(type[k])) ++aerial;
    }
    const float cap = norms.aerial_cap > 0.0f ? norms.aerial_cap : 1.0f;
    globals[static_cast<std::size_t>(ObsGlobal::AerialAliveFrac)] =
        std::clamp(static_cast<float>(aerial) / cap, 0.0f, 1.0f);
}

void pack_unit_obs(const WorldView& view, UnitId self, const FlowField* flow,
                   const ObsNorms& norms, std::span<float> cells,
                   std::span<float> self_vec, std::span<float> globals) {
    if (cells.size() != static_cast<std::size_t>(kObsCellFloats) ||
        self_vec.size() != static_cast<std::size_t>(kObsSelfFloats)) {
        throw ContractError("pack_unit_obs: 缓冲区长度必须恰好等于 kObs*Floats");
    }
    const auto alive = view.unit_alive();
    const std::size_t si = self.index();
    if (si >= alive.size() || alive[si] == 0) {
        throw ContractError("pack_unit_obs: self 不是一个活着的单位");
    }
    const auto u_type = view.unit_type();
    if (side_of(u_type[si]) != view.side()) {
        // 拿别人的视角打包 = 把 god 视角换了个形状，正是硬要求 1 禁的事。
        throw ContractError("pack_unit_obs: self 不属于本视图那一侧");
    }

    std::fill(cells.begin(), cells.end(), 0.0f);
    std::fill(self_vec.begin(), self_vec.end(), 0.0f);
    pack_globals(view, norms, globals);

    const auto u_pos = view.unit_pos();
    const auto u_hp = view.unit_hp();
    const auto u_max = view.unit_max_hp();
    const auto u_lvl = view.unit_level();
    const GridPos me = grid_of(u_pos[si]);
    const int half = kObsK / 2;
    const int w = view.width();
    const int h = view.height();

    // **迷雾是本侧的**（`WorldView::fog()`），god 视角要走 `World::fog(Side)`。
    // 整个打包器只经这一个来源读可见性——多一个来源就多一条绕过硬要求 1 的路。
    const FogLayer& fog = view.fog();

    // 视野窗口 → 世界格。`dx/dy` 是窗口下标，`(x, y)` 是世界格。
    // 越界格的所有通道保持 0（含 `passable`）：地图外不可通行，
    // 而 `visible` 为 0 正好读作「从未见过」——两者一致，不需要特判。
    const auto vis_of = [&](int x, int y) {
        return fog.in_bounds(x, y) ? fog.at(x, y) : Vis::Unseen;
    };

    // ——地形与可见性：逐格，不看实体——
    const TerrainMasks& terr = view.terrain();
    for (int dy = 0; dy < kObsK; ++dy) {
        for (int dx = 0; dx < kObsK; ++dx) {
            const int x = me.i - half + dx;
            const int y = me.j - half + dy;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            const Vis v = vis_of(x, y);
            // 三档一律经 `vis_value()`。**不要在这里写 `v != Unseen`**——
            // 那就把三态压回两态，而「从未侦查过」与「记忆里那段墙是破的」
            // 会再次撞成同一串字节（`rts/fog.hpp` 文件头）。
            cells[at(dx, dy, ObsChannel::Visible)] = vis_value(v);
            // 地形**不过迷雾**：地形不会变，而「地图长什么样」对玩家与 AI
            // 同样是免费信息（`CLAUDE.md` 的情报划分里它不在「需要侦查」那一列）。
            // 走地面的可通行性——空军飞过一切，那一层由 `self` 的兵种决定，
            // 不该混进这条给所有单位共用的通道。
            cells[at(dx, dy, ObsChannel::Passable)] =
                terr.passable(x, y, Mobility::Ground) ? 1.0f : 0.0f;
        }
    }

    // ——建筑：可见格用实况，记忆格用记忆——
    //
    // 顺序上先写记忆再用实况覆盖，是因为一格只可能是三态之一，两者不重叠；
    // 分两遍写只为把「记忆从哪来」这件事留在一处。
    for (int dy = 0; dy < kObsK; ++dy) {
        for (int dx = 0; dx < kObsK; ++dx) {
            const int x = me.i - half + dx;
            const int y = me.j - half + dy;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            if (vis_of(x, y) != Vis::Remembered) continue;
            RememberedBld rb;
            if (!fog.remembered_bld(x, y, &rb)) continue;   // 记忆里这里是缺口
            cells[at(dx, dy, channel_of_bld(rb.type))] =
                static_cast<float>(rb.hp_permille) / static_cast<float>(kFullPermille);
        }
    }
    const auto b_alive = view.bld_alive();
    const auto b_type = view.bld_type();
    const auto b_pos = view.bld_pos();
    const auto b_hp = view.bld_hp();
    const auto b_max = view.bld_max_hp();
    for (std::size_t k = 0; k < b_alive.size(); ++k) {
        if (b_alive[k] == 0) continue;
        const int dx = b_pos[k].i - me.i + half;
        const int dy = b_pos[k].j - me.j + half;
        if (dx < 0 || dy < 0 || dx >= kObsK || dy >= kObsK) continue;
        if (vis_of(b_pos[k].i, b_pos[k].j) != Vis::Visible) continue;
        cells[at(dx, dy, channel_of_bld(b_type[k]))] = frac(b_hp[k], b_max[k]);
    }

    // ——中立可破坏障碍：与建筑同一条门（可见才写）——
    //
    // 它**不进迷雾记忆**（`FogLayer` 只记建筑），所以记忆格上它是 0。
    // 这是有意的：清野是玩家级决策、拿的是一份有限存量，
    // 记住一个可能已经被清掉的木桩没有价值。
    const auto o_alive = view.obstacle_alive();
    const auto o_pos = view.obstacle_pos();
    const auto o_hp = view.obstacle_hp();
    const auto o_max = view.obstacle_max_hp();
    for (std::size_t k = 0; k < o_alive.size(); ++k) {
        if (o_alive[k] == 0) continue;
        const int dx = o_pos[k].i - me.i + half;
        const int dy = o_pos[k].j - me.j + half;
        if (dx < 0 || dy < 0 || dx >= kObsK || dy >= kObsK) continue;
        if (vis_of(o_pos[k].i, o_pos[k].j) != Vis::Visible) continue;
        cells[at(dx, dy, ObsChannel::ObstacleHp)] = frac(o_hp[k], o_max[k]);
    }

    // ——单位：己方一律可见，敌方**必须**当前可见——
    //
    // 这是硬要求 1 最要紧的一处。两条不对称都是设计：
    //
    //   * 己方不过迷雾——自己的部队自己知道在哪，过滤它只会让策略瞎
    //   * 敌方**只认 `Visible`，不认 `Remembered`**——`CLAUDE.md`
    //     「迷雾记建筑、不记单位」：建筑不动，记住的单位位置会主动误导，
    //     而那份误导正是玩家佯攻的手段。写进记忆等于替 AI 作弊，
    //     且方向是让它变笨（拿着过期位置去包夹）
    const float cap = norms.cell_capacity > 0.0f ? norms.cell_capacity : 1.0f;
    const float ldiv = view.nominal_level() > 0
                           ? static_cast<float>(view.nominal_level())
                           : 1.0f;
    for (std::size_t k = 0; k < alive.size(); ++k) {
        if (alive[k] == 0) continue;
        const GridPos g = grid_of(u_pos[k]);
        const int dx = g.i - me.i + half;
        const int dy = g.j - me.j + half;
        if (dx < 0 || dy < 0 || dx >= kObsK || dy >= kObsK) continue;
        const bool ally = side_of(u_type[k]) == view.side();
        if (!ally && vis_of(g.i, g.j) != Vis::Visible) continue;
        const ObsChannel den = ally ? ObsChannel::AllyDensity : ObsChannel::EnemyDensity;
        const ObsChannel hpc = ally ? ObsChannel::AllyHp : ObsChannel::EnemyHp;
        const ObsChannel lvc = ally ? ObsChannel::AllyLevel : ObsChannel::EnemyLevel;
        cells[at(dx, dy, den)] += 1.0f / cap;
        // 血量存**比例之和**而不是绝对值之和（`obs.hpp`）：绝对血量随等级涨，
        // 又是一条会随波数漂移两个数量级的尺度。
        cells[at(dx, dy, hpc)] += frac(u_hp[k], u_max[k]);
        // 等级存**和 ÷ L(w)**（不是平均）：与密度同量纲，两条通道因此可以
        // 相加相减、共享卷积核。两侧共用同一个分母。
        cells[at(dx, dy, lvc)] += static_cast<float>(u_lvl[k]) / ldiv;
    }

    // ——方向场：两条 [-1, 1] 分量——
    //
    // `flow == nullptr` 时留 0（见头文件）。`step_of` 给的是 `UnitAction`，
    // 斜向的两个分量都是 ±1——**刻意不做单位化**：观测要的是「往哪走」这个
    // 离散选择，而 8 个动作里斜向本来就同时动两轴，归一化会引入一个
    // 与动作空间无关的 0.707。
    if (flow != nullptr) {
        for (int dy = 0; dy < kObsK; ++dy) {
            for (int dx = 0; dx < kObsK; ++dx) {
                const int x = me.i - half + dx;
                const int y = me.j - half + dy;
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                const UnitAction a = flow->step_of(
                    GridPos{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)});
                if (!is_move(a)) continue;
                const GridDelta d = move_delta(a);
                cells[at(dx, dy, ObsChannel::FlowDi)] = static_cast<float>(d.di);
                cells[at(dx, dy, ObsChannel::FlowDj)] = static_cast<float>(d.dj);
            }
        }
    }

    // ——自身向量——
    self_vec[static_cast<std::size_t>(u_type[si])] = 1.0f;   // 兵种 one-hot
    self_vec[static_cast<std::size_t>(ObsSelfField::LevelNorm)] =
        static_cast<float>(u_lvl[si]) / ldiv;
    self_vec[static_cast<std::size_t>(ObsSelfField::HpFrac)] =
        frac(u_hp[si], u_max[si]);
    // 前摇**剩余比例**（1 = 刚开始摇，0 = 可出手）。分母取该兵种的前摇总长，
    // 从数值表来——它是待标定的，所以这里一个数字都不写死。
    const std::int32_t windup_total = view.stats().of(u_type[si]).windup_ticks;
    const std::int32_t left = view.unit_windup()[si];
    self_vec[static_cast<std::size_t>(ObsSelfField::WindupLeft)] =
        windup_total > 0 ? std::clamp(static_cast<float>(left) /
                                          static_cast<float>(windup_total),
                                      0.0f, 1.0f)
                         : 0.0f;
}

}  // namespace rts
