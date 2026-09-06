// 批量环境：一次并行 step N 局，直接产出打包好的观测张量。
//
// `CLAUDE.md`「rts_core 实现约定」最后一条：
//
//   > 提供 `BatchedEnv`：用线程池一次并行 step N 局游戏，直接向 Python 返回
//   > 打包好的 observation 张量。避免 Python 侧循环开销。
//
// ## 它是纯 C++，刻意**不**放在 `bindings/` 里
//
// 一件纯 C++ 的东西放在 Python 绑定层，代价是它从此只能在装了 Python 开发头的
// 机器上被编译与测试。而**训练服务器现在就没有 `Python.h`**（也没有 pip / torch，
// 见 `训练服务器环境.md`），于是那一层短期内根本编不了。
//
// 放在 `rts_core` 则相反：它进默认构建、被 GCC 验、有 ctest。这与本项目把前端
// 切成 `game/`（默认构建、可测）+ `render/`（默认不配置）是**同一条判据**——
// 按「能不能在默认构建里被测」切，而不是按「属于哪个模块」切。
//
// `bindings/` 那一层因此只剩一件事：把下面这三个函数的指针递给 numpy。
//
// ## 并行的难点不是并行，是**装配顺序必须确定**
//
// 每一局各自持有 `World`，互不共享状态，所以「单局确定性」不受线程影响。
// 真正会坏的是**汇总**：若结果按完成顺序写进输出缓冲，那么同一批同一种子跑两遍
// 会得到两份不同排列的张量——而它**不会报错**，只是训练时每个样本对应到了
// 别的环境。所以：
//
//   * 输出一律按**环境下标**写（`env_index * stride`），不按完成顺序 append
//   * 线程只读自己那一局，不写任何共享状态（除各自那段输出）
//   * 不用任何跨线程归约（`std::reduce` 之类会重排浮点加法顺序）
//
// 于是 `step()` 的结果与线程数、调度、机器核数**全部无关**。这一条由
// `tests/batched_env_test.cpp` 用「1 线程 vs N 线程逐字节相同」钉住。

#ifndef RTS_BATCHED_ENV_HPP
#define RTS_BATCHED_ENV_HPP

#include <array>
#include <cstdint>
#include <string_view>
#include <memory>
#include <span>
#include <vector>

#include "rts/obs_pack.hpp"
#include "rts/types.hpp"
#include "rts/world.hpp"

namespace rts {

// 一批环境的构型。
struct BatchedEnvInit {
    // 每一局的初始局面。**长度即批大小**，且各局可以不同——按波次分层采样
    // （`CLAUDE.md`「波数即难度轴」）要的正是「同一批里混着不同波数的局面」。
    std::vector<WorldInit> worlds;
    // 观测是给哪一侧打包的。攻方 RL 控制每一个单位，所以训练攻方时取
    // `Attacker`；守方决策层不吃这套 K×K 通道（`obs.hpp` 文件头），所以
    // 这一版只有逐单位那一侧会用到它。
    Side side = Side::Attacker;
    // 决策频率：一次 `step()` 推进多少 tick。
    //
    // `CLAUDE.md`「每 4–8 tick 决策一次，不是每 tick」——**这个数是占位值**，
    // 落在那个区间的中点。它同时是不变量 1 的落地处：Python 一次调用推进 N 个
    // tick，而不是每 tick 回调一次上层。
    int ticks_per_step = 6;
    // **episode 的时间上界（tick）。0 = 不设。**
    //
    // 没有它，`done` 只在「Keep 被拆」时置位 ⇒ 打不动的策略会把一局无限拖
    // 下去。实测：随机策略在 170×170 图上推 **18000 tick 仍未终局**，于是
    // 一个 rollout（384 tick）里一次奖励都收不到，PPO 学不动。
    //
    // 而 `CLAUDE.md` 要的正好相反：「**一波 = 一个 RL episode**」、
    // 「**短 episode** 让 credit assignment 链条足够短，是训练可行的关键」。
    // 地图校验器第 5 条也承诺 episode ∈ [1200, 2400] tick。
    //
    // 默认取 **2400**，与 `game::WaveTiming::assault_max_ticks` 同值——
    // demo 侧早就有这条上界，训练侧此前漏了。**两处刻意不共享一个常量**：
    // 那个是波次节奏的旋钮（配平要调它），这个是 episode 的定义（RL 的
    // credit assignment 依赖它），改动的理由不同。
    int max_ticks_per_episode = 2400;
    // 线程数。0 = 由实现挑（硬件并发数，上限批大小）。
    // **它不影响结果**，只影响墙钟时间——见文件头。
    int threads = 0;
    // 归一化分母，逐批共用（见 `rts/obs_pack.hpp` 的 `ObsNorms`）。
    ObsNorms norms;
};

// 一批环境。**不可拷贝**：它持有 N 个 `World`，拷贝一批环境几乎总是笔误
// （想要的是再造一批，而不是复制当前状态）。
class BatchedEnv {
public:
    explicit BatchedEnv(BatchedEnvInit init);
    ~BatchedEnv();

    BatchedEnv(const BatchedEnv&) = delete;
    BatchedEnv& operator=(const BatchedEnv&) = delete;

    int batch_size() const noexcept;
    Side side() const noexcept;

    // 本批**当前**每一局各有多少个活着的、属于 `side()` 的单位。
    //
    // 它是变长的，而张量必须定长——所以观测缓冲区按 `max_units_per_env`
    // 分段，多出来的位置留 0，并由 `unit_counts()` 告诉 Python 哪几段有效。
    // **不做「把活单位压实成连续一段」**：那会让同一个单位在相邻两步落在
    // 不同下标上，而策略网络是逐单位独立前向的，压实只会让日志与调试对不上号。
    std::span<const int> unit_counts() const noexcept;

    // 打包当前观测。三块缓冲区的长度必须**恰好**是：
    //
    //   cells    : batch × max_units × kObsCellFloats
    //   self_vec : batch × max_units × kObsSelfFloats
    //   globals  : batch × kObsGlobalFloats
    //
    // 长度不符即抛（同 `pack_unit_obs` 的理由：Python 侧 `reshape` 会照样成功）。
    //
    // **本版一律不喂方向场，于是 `FlowDi` / `FlowDj` 两条通道恒为 0。**
    // 如实记在这里，因为它与 `AerialAliveFrac` 那条恒 0 **不是同一种**：那条是
    // 世界里还没有空军（接线是对的，放一只进去就有值），这条是**接线还不存在**。
    //
    // 不顺手接上是因为它不是一行的事：field 按 `(mover, tier)` 缓存与失效
    // （`rts/flow.hpp` 把这条明确划给调用方），而一批里每局的墙血各自在变，
    // 所以要么每局一套缓存、要么每步重算 N × 兵种 × 档 张——前者是这个类要
    // 增加的一整块状态，后者恰好是 `flow.hpp` 让调用方缓存的那件事。
    // 该由谁决定重算节律，得等 `train/` 那侧的形状定了才知道。
    void observe(std::span<float> cells, std::span<float> self_vec,
                 std::span<float> globals);

    // 推进一批。`actions` 的形状是 batch × max_units，**按 `enumerate_units()`
    // 的规范顺序**逐单位给动作；超出该局单位数的位置被忽略（不是报错——
    // 定长张量里那些位置本来就没有对应单位）。
    //
    // 返回值是每一局这一步的「是否终局」。终局的局**不自动重置**：
    // 重置时机归 `train/`（要按波次分层采样，重置成哪一波是它的决定）。
    void step(std::span<const UnitAction> actions, std::span<std::uint8_t> done);

    // ——战果，给 `train/` 折奖励用（2026-09-06）——
    //
    // 每局一行、`kTallyFields` 列，读走即清（见 `World::take_tally`）。
    // **权重不在这一层**：那是训练侧的超参，而这里只给「发生了什么」。
    //
    // 列的顺序就是下面 `kTallyNames` 的顺序，`train/` 侧照它解包——
    // 同 `kObsChannels` 那条纪律（两侧不一致时不会有任何东西报错，
    // 张量照样 reshape 成功、网络照样收敛，收敛到一个把「击杀数」当
    // 「自身损失」的表示上）。
    void take_tally(std::span<float> out);

    // 动作掩码：每局每 agent 一个 16 位位图（第 k 位 = `UnitAction(k)` 合法）。
    //
    // **没有它策略学不动**（`CLAUDE.md`「并做动作掩码」）：非法动作会被
    // `submit_actions` 静默拒成 `Stop`，于是策略反复输出一个「看起来有效果
    // 但实际什么都没发生」的动作，梯度里全是噪声。
    //
    // 掩码取**队长**的（agent = 编队）。队员的掩码可能不同（站位不一样），
    // 那是「编队 = 一个 agent」这个抽象自带的代价，与观测取队长同源。
    void action_masks(std::span<std::uint16_t> out) const;

    static constexpr int kTallyFields = 7;
    static constexpr std::array<std::string_view, kTallyFields> kTallyNames{
        {"dmg_to_units", "dmg_to_blds", "units_killed", "blds_destroyed",
         "bld_value", "scouts_killed", "losses"}};

    // 把第 `i` 局换成一个新局面。终局之后由 `train/` 调。
    void reset_one(int i, WorldInit init);

    // 每一局最多打包多少个 **agent**。
    //
    // **agent = 编队，不是单位**（2026-09-05，`CLAUDE.md`「攻方 RL 控制的是
    // 每一支编队」）。所以这个数取的是**编队数**硬顶：
    //
    //   * 取 **32**，比攻方硬顶 `slots_cap = 27` 留一点余量——那个 27 是
    //     待标定的**数值**（依据见 `game::WaveCurve::slots_cap`：SMAC 最大的
    //     官方图恰是 27 个 agent、我们每 agent 的观测是它的 18–21 倍），
    //     而本常量是**张量的形状**。让形状恰好等于一个会调的数，等于每次调
    //     那个数都要改契约。
    //   * 守方那一侧是散兵（每人一支），上界随堡垒等级涨（`8 + 2K`，K 到 15
    //     就是 38）⇒ **给守方打包时 32 会不够**，那一侧要另算。这一版只有
    //     攻方走这条路，同原注释。
    //
    // **它此前是 40，而那个 40 已经过期两次**：注释说「取攻方硬上限 40」，
    // 可 2026-09-01 撤下过那个硬顶（变成不封顶）、09-05 又改成 27 且语义
    // 从单位数变成编队数。中间那段时间攻方场上有 ~70 个单位，于是
    // `observe` 只打包前 40 个、`step` 给后 30 个补 `Stop` ——**它们存在、
    // 会挨打、但完全不受控且不被观测**，而这不会让任何测试变红
    // （`smoke.py` 手摆 3–5 个单位，从来没碰到过上限）。
    static constexpr int kMaxUnitsPerEnv = 32;

    // 只读地拿第 `i` 局的世界，给测试与调试用（**不给训练热路径用**：
    // 逐局取视图再自己循环，正好绕开了这个类存在的理由）。
    const World& world_at(int i) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace rts

#endif  // RTS_BATCHED_ENV_HPP
