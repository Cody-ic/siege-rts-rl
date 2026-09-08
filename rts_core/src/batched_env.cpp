#include "rts/batched_env.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <thread>

#include "rts/flow.hpp"
#include "rts/roster.hpp"
#include "rts/world_view.hpp"

namespace rts {

struct BatchedEnv::Impl {
    Side side = Side::Attacker;
    int ticks_per_step = 6;
    int threads = 1;
    ObsNorms norms;

    // 逐局的状态。**三个 vector 同长、同下标**——环境 `i` 的一切都在下标 `i`，
    // 这是「装配顺序确定」那条不变量在数据布局上的落地：没有任何一处按完成
    // 顺序 append 的容器，所以线程调度没有地方可以泄漏进结果。
    std::vector<std::unique_ptr<World>> worlds;
    std::vector<int> counts;
    // 逐局的单位句柄缓存。**每局一个**而不是共用一个：`enumerate_units` 要一个
    // 输出 vector，而多线程共用一个就是数据竞争。预分配在这里，`step` 里不再分配
    // （热路径不分配，同 `obs_pack` 那条理由）。
    std::vector<std::vector<UnitId>> ids;
    // 逐局的**编队队长**句柄（`enumerate_squads`）。观测按它摊行、动作按它取，
    // 因为 **agent = 编队**（`kMaxUnitsPerEnv` 那段注释）。
    std::vector<std::vector<UnitId>> leaders;
    // 逐局的逐单位动作切片。`submit_actions` 要「每局恰好等于活单位数」的一段，
    // 而策略给的是**逐编队**的动作 ⇒ 要在这里摊开。预分配同上：热路径不分配。
    std::vector<std::vector<UnitAction>> acts;
    // 逐局已跑了多少 tick（`max_ticks_per_episode` 用）。
    std::vector<int> elapsed;
    std::vector<EpisodeEnd> ends;
    int max_ticks = 0;
    // 对侧驱动钩子（`BatchedEnvInit::opponent_hook`）。**在工作线程里调**，
    // 所以实现只许碰第 i 局自己的状态——那条纪律写在声明处。
    std::function<void(World&, int)> opponent;
    std::function<std::unique_ptr<World>(WorldInit, int)> world_factory;
    std::function<BatchedGoals(const WorldView&, std::span<const UnitId>, int)> goal_hook;

    std::unique_ptr<World> make_world(WorldInit init, int index) {
        auto result = world_factory ? world_factory(std::move(init), index)
                                    : std::make_unique<World>(std::move(init));
        if (!result) throw ContractError("BatchedEnv: world factory returned null");
        return result;
    }
    // 逐局的 flow field 缓存：`(兵种 × 等级档 × 目标组)`，每次 observe 重算。
    //
    // **`observe` 里必须喂方向场，否则策略没有任何东西指向目标。**
    // 此前这里传 `nullptr`（`obs_pack.hpp` 说「训练早期没有宏观目标时是正常
    // 形态」），而实测那让 `FlowDi`/`FlowDj` 恒 0 ⇒ 14 条通道里只有 4 条非零，
    // 攻方在 50 格外、视野半径只有 7 格、`enemy_*` 全 0 —— **观测里没有任何
    // 东西告诉策略该往哪走**。40 万步训练回报恒 0.00 就是这么来的，
    // 而同一个局面用「一路朝 keep 走」的定向策略能打出 8515 点建筑伤害。
    //
    // 重算节律取「每次 `observe`」而不是缓存跨步：破坏代价读实时墙血
    // （`flow.hpp` 明写「重算节律归调用方」），而 `game::DemoBattle` 取的
    // 也是每个决策拍。
    std::vector<std::vector<std::optional<FlowField>>> flows;
    FlowTiering tiering{};

    // ——`progress` 那一列（见 `kTallyNames`）——
    //
    // 逐局的距离快照：`step` 推进**之前**记下每个活单位到堡垒的距离，
    // 推进之后再算一遍，只对**两端都在**的单位求差。
    // 预分配、只 `assign` 不 `push_back`——热路径不分配。
    std::vector<std::vector<std::pair<UnitId, int>>> dist_before;
    // 逐局本步缩短了几格。`take_tally` 读走即清（与前 7 列同款语义）。
    std::vector<double> progress;

    // 到堡垒的切比雪夫距离。**取格坐标而不是浮点距离**：格是移动的单位，
    // 而浮点差在两套工具链之间会飘（本项目明写回放不跨平台，但没必要
    // 在一个纯训练信号上再引入一处平台差异）。
    static int dist_to_keep(const World& w, UnitId id) {
        const GridPos g = grid_of(w.unit_pos(id));
        const GridPos k = w.keep_pos();
        const int dx = g.i > k.i ? g.i - k.i : k.i - g.i;
        const int dy = g.j > k.j ? g.j - k.j : k.j - g.j;
        return dx > dy ? dx : dy;
    }

    BatchedGoals intent_for(std::size_t i) const {
        const auto& w=*worlds[i];
        auto intent=goal_hook ? goal_hook(w.view(side),leaders[i],static_cast<int>(i)) : BatchedGoals{};
        if(!intent.groups.empty() && intent.groups.size()!=leaders[i].size())
            throw ContractError("BatchedEnv: goal groups must match squad leaders");
        for(auto group:intent.groups)
            if(group>1) throw ContractError("BatchedEnv: invalid goal group");
        for(auto cell:intent.economy)
            if(cell.i<0 || cell.j<0 || cell.i>=w.width() || cell.j>=w.height())
                throw ContractError("BatchedEnv: economy goal outside map");
        return intent;
    }

    // 把 `[lo, hi)` 这段环境分给若干线程跑同一个函数体。
    //
    // **刻意不用任何「任务队列 / 工作窃取」**：那类调度让「哪个线程处理哪一局」
    // 随运行变化，虽然本实现里每局只写自己那段、结果仍然确定，但它把
    // 「为什么结果确定」的论证从「结构上不可能」降级为「碰巧没有共享写」。
    // 固定分段则一眼看得出每个线程碰哪几局。
    // **工作线程里抛出的异常必须捎回调用线程。** 不捎的话它会走到线程函数外，
    // 于是 `std::terminate` —— **整个进程 SIGABRT**。从 Python 调的时候那是最糟的
    // 失败方式：训练器直接死掉，没有 traceback、没有 `RuntimeError`、
    // 什么都问不出来。
    //
    // 这条不是假想的：做「输出按完成顺序装配」那次破坏性验证时，
    // 期待的是一条干净的断言失败，实际拿到的是退出码 134 —— 那次崩溃
    // 顺带查出了这里缺一层捕获。
    //
    // 捎回的是**下标最小的那一个**异常，不是最先抛的那一个：后者取决于线程
    // 调度，于是同一个错误每次报出来的可能是不同的一条，而那让复现变难。
    template <typename F>
    void for_each_env(int n, F&& f) {
        const int t = std::clamp(threads, 1, n > 0 ? n : 1);
        if (t <= 1) {
            for (int i = 0; i < n; ++i) f(i);
            return;
        }
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(t));
        // 逐环境一格，所以线程之间不写同一个位置——不需要锁。
        std::vector<std::exception_ptr> errs(static_cast<std::size_t>(n));
        const int chunk = (n + t - 1) / t;
        for (int k = 0; k < t; ++k) {
            const int lo = k * chunk;
            const int hi = std::min(n, lo + chunk);
            if (lo >= hi) break;
            pool.emplace_back([lo, hi, &f, &errs]() {
                for (int i = lo; i < hi; ++i) {
                    try {
                        f(i);
                    } catch (...) {
                        errs[static_cast<std::size_t>(i)] = std::current_exception();
                    }
                }
            });
        }
        for (std::thread& th : pool) th.join();
        for (const std::exception_ptr& e : errs) {
            if (e) std::rethrow_exception(e);
        }
    }

    // 「堡垒还在吗」。**刻意不往 `World` 加一个 `keep_alive()`**：
    // `game::DemoBattle` 已经有一份同样的扫法（`demo_driver.cpp:139`），
    // 往 `World` 加第三处只会让「哪个才是权威」多一个候选。
    // 真要收敛，该做的是把 `game/` 那份也换成同一个来源，而那是另一件事
    // ——本批不顺手改别人正在动的文件。
    static bool keep_alive(const World& w) {
        const WorldView v = w.view(Side::Defender);
        const auto alive = v.bld_alive();
        const auto type = v.bld_type();
        for (std::size_t k = 0; k < alive.size(); ++k) {
            if (alive[k] != 0 && type[k] == BldType::Keep) return true;
        }
        return false;
    }

    void refresh_counts() {
        const int n = static_cast<int>(worlds.size());
        for (int i = 0; i < n; ++i) {
            const std::size_t ui = static_cast<std::size_t>(i);
            worlds[ui]->enumerate_units(side, ids[ui]);
            worlds[ui]->enumerate_squads(side, leaders[ui]);
            // `counts` 记的是**编队数**（= agent 数），因为张量的第二维是 agent。
            // 逐单位那一侧的长度由 `ids[ui].size()` 给，两者不是一回事。
            counts[ui] = static_cast<int>(leaders[ui].size());
        }
    }
};

BatchedEnv::BatchedEnv(BatchedEnvInit init) : p_(std::make_unique<Impl>()) {
    if (init.worlds.empty()) {
        throw ContractError("BatchedEnv: 批大小不能是 0");
    }
    if (init.ticks_per_step <= 0) {
        throw ContractError("BatchedEnv: ticks_per_step 必须为正");
    }
    p_->side = init.side;
    p_->ticks_per_step = init.ticks_per_step;
    p_->norms = init.norms;
    const int n = static_cast<int>(init.worlds.size());
    // 线程数 0 = 由实现挑。**上限是批大小**：线程比局多没有意义，
    // 而且会让 `for_each_env` 造出一堆空分段。
    p_->threads = init.threads > 0
                      ? init.threads
                      : std::clamp(static_cast<int>(std::thread::hardware_concurrency()),
                                   1, n);
    p_->worlds.reserve(static_cast<std::size_t>(n));
    p_->world_factory = std::move(init.world_factory);
    p_->goal_hook = std::move(init.goal_hook);
    for (WorldInit& wi : init.worlds) {
        p_->worlds.push_back(p_->make_world(std::move(wi), static_cast<int>(p_->worlds.size())));
    }
    p_->counts.assign(static_cast<std::size_t>(n), 0);
    p_->ids.resize(static_cast<std::size_t>(n));
    p_->leaders.resize(static_cast<std::size_t>(n));
    p_->acts.resize(static_cast<std::size_t>(n));
    p_->elapsed.assign(static_cast<std::size_t>(n), 0);
    p_->ends.assign(static_cast<std::size_t>(n), EpisodeEnd::Running);
    p_->flows.resize(static_cast<std::size_t>(n));
    for (auto& f : p_->flows) {
        f.resize(static_cast<std::size_t>(kUnitTypeCount) *
                 static_cast<std::size_t>(kFlowTierCount) * 2);
    }
    p_->max_ticks = init.max_ticks_per_episode;
    p_->opponent = std::move(init.opponent_hook);
    p_->dist_before.resize(static_cast<std::size_t>(n));
    p_->progress.assign(static_cast<std::size_t>(n), 0.0);
    p_->refresh_counts();
}

BatchedEnv::~BatchedEnv() = default;

int BatchedEnv::batch_size() const noexcept {
    return static_cast<int>(p_->worlds.size());
}

int BatchedEnv::max_ticks_per_episode() const noexcept { return p_->max_ticks; }

Side BatchedEnv::side() const noexcept { return p_->side; }

std::span<const BatchedEnv::EpisodeEnd> BatchedEnv::episode_ends() const noexcept {
    return p_->ends;
}

std::span<const int> BatchedEnv::unit_counts() const noexcept {
    return std::span<const int>(p_->counts.data(), p_->counts.size());
}

const World& BatchedEnv::world_at(int i) const {
    if (i < 0 || i >= batch_size()) throw ContractError("BatchedEnv: 环境下标越界");
    return *p_->worlds[static_cast<std::size_t>(i)];
}

void BatchedEnv::observe(std::span<float> cells, std::span<float> self_vec,
                         std::span<float> globals) {
    const int n = batch_size();
    const std::size_t per_cells =
        static_cast<std::size_t>(kMaxUnitsPerEnv) * static_cast<std::size_t>(kObsCellFloats);
    const std::size_t per_self =
        static_cast<std::size_t>(kMaxUnitsPerEnv) * static_cast<std::size_t>(kObsSelfFloats);
    if (cells.size() != static_cast<std::size_t>(n) * per_cells ||
        self_vec.size() != static_cast<std::size_t>(n) * per_self ||
        globals.size() !=
            static_cast<std::size_t>(n) * static_cast<std::size_t>(kObsGlobalFloats)) {
        throw ContractError("BatchedEnv::observe: 缓冲区长度必须恰好等于 batch × 每局的量");
    }
    // 先整块清零，再逐单位写。**这样「多出来的位置留 0」不依赖任何一个分支**
    // ——依赖分支的话，某局单位数从 5 掉到 3 时，第 4、5 段会留着上一步的值，
    // 而那是**过期观测**：不报错、不崩，只是策略读到了两步前的战场。
    std::fill(cells.begin(), cells.end(), 0.0f);
    std::fill(self_vec.begin(), self_vec.end(), 0.0f);
    std::fill(globals.begin(), globals.end(), 0.0f);

    p_->for_each_env(n, [&](int i) {
        const std::size_t ui = static_cast<std::size_t>(i);
        const World& w = *p_->worlds[ui];
        const WorldView v = w.view(p_->side);
        pack_globals(v, p_->norms,
                     globals.subspan(ui * static_cast<std::size_t>(kObsGlobalFloats),
                                     static_cast<std::size_t>(kObsGlobalFloats)));
        // **按编队队长摊行**（agent = 编队）。队长的观测代表整队——这正是
        // 「group action 是共享子目标」那条形状要求的（见 `kMaxUnitsPerEnv`）。
        const std::vector<UnitId>& ids = p_->leaders[ui];
        const int m = std::min(static_cast<int>(ids.size()), kMaxUnitsPerEnv);
        const auto intent = p_->intent_for(ui);
        // 本局的 field 全部作废重算（墙血变了破坏代价就变）。
        for (auto& f : p_->flows[ui]) f.reset();
        const GridPos goal[1] = {w.keep_pos()};
        for (int u = 0; u < m; ++u) {
            // **输出位置只由 (i, u) 决定**，与哪个线程、什么时候跑完无关。
            const std::size_t co = ui * per_cells +
                                   static_cast<std::size_t>(u) *
                                       static_cast<std::size_t>(kObsCellFloats);
            const std::size_t so = ui * per_self +
                                   static_cast<std::size_t>(u) *
                                       static_cast<std::size_t>(kObsSelfFloats);
            // 全局标量已经写过一份，这里给 `pack_unit_obs` 一段临时的：它会再写一遍
            // 同样的值（幂等），换来的是「一个单位一次调完」这个更难用错的接口。
            float scratch[kObsGlobalFloats] = {};
            // **喂方向场**（2026-09-06；此前是 `nullptr`）。按 `(兵种, 档)`
            // 缓存，所以一局里同兵种同档的 agent 共用一张，不是每个都重算。
            const UnitId self = ids[static_cast<std::size_t>(u)];
            const UnitType mover = w.unit_type(self);
            const int tier = flow_tier_of(w.unit_level(self), p_->tiering);
            const std::size_t group = intent.groups.empty() || intent.economy.empty()
                ? std::size_t{0} : static_cast<std::size_t>(intent.groups[static_cast<std::size_t>(u)]);
            const std::span<const GridPos> targets = group == 1
                ? std::span<const GridPos>(intent.economy) : std::span<const GridPos>(goal);
            const std::size_t fi = (static_cast<std::size_t>(mover) *
                                       static_cast<std::size_t>(kFlowTierCount) +
                                   static_cast<std::size_t>(tier)) * 2 + group;
            std::optional<FlowField>& fld = p_->flows[ui][fi];
            if (!fld) {
                fld.emplace(FlowField::compute(v, mover, tier, targets, p_->tiering));
            }
            pack_unit_obs(v, self, &*fld, p_->norms,
                          cells.subspan(co, static_cast<std::size_t>(kObsCellFloats)),
                          self_vec.subspan(so, static_cast<std::size_t>(kObsSelfFloats)),
                          std::span<float>(scratch, kObsGlobalFloats));
        }
    });
}

void BatchedEnv::step(std::span<const UnitAction> actions,
                      std::span<std::uint8_t> done) {
    const int n = batch_size();
    if (actions.size() !=
            static_cast<std::size_t>(n) * static_cast<std::size_t>(kMaxUnitsPerEnv) ||
        done.size() != static_cast<std::size_t>(n)) {
        throw ContractError("BatchedEnv::step: actions 必须是 batch × kMaxUnitsPerEnv，"
                            "done 必须是 batch");
    }
    // 提交与推进。**张量的第二维是编队（agent），而 `submit_actions` 要的是
    // 逐单位**（长度恰好等于该局活单位数，它自己有这条检查）⇒ 这里要把
    // 编队的动作**摊给队里每个成员**。
    //
    // ⚠️ **摊开的是「动作」，而这与 `game/` 那一侧刻意不广播方向是两回事。**
    // 那边的结论（TStarBot2 / SupCom2 / OpenRA…，见 `demo_driver.cpp` 里那段）
    // 是「脚本的战术层该逐单位算」；而这里是 **RL 的动作面**：策略每支编队
    // 只输出一个动作，那是「27 个 agent」这个设计的定义本身。将来若要让 RL
    // 也发「子目标」而不是「动作」，改的是这一层的语义（动作枚举 → 目标集
    // 下标），不是这段摊开的代码。
    p_->for_each_env(n, [&](int i) {
        const std::size_t ui = static_cast<std::size_t>(i);
        World& w = *p_->worlds[ui];
        if (p_->ends[ui] != EpisodeEnd::Running) {
            done[ui] = 1;
            return;  // A terminal environment cannot accrue further battle rewards.
        }
        const std::vector<UnitId>& units = p_->ids[ui];
        const int nsq = std::min(p_->counts[ui], kMaxUnitsPerEnv);
        std::vector<UnitAction>& slice = p_->acts[ui];
        // 默认 `Stop`：**它永远合法**（`action.hpp`），所以「编队数超出张量、
        // 尾部编队没有动作可给」不会造出非法输入。攻方编队硬顶 27 <
        // `kMaxUnitsPerEnv` 32，所以那条分支在攻方一侧不该发生；守方是散兵、
        // 上限随堡垒等级涨（`8+2K`），将来会走到。
        // **推进之前**记下每个活单位到堡垒的距离（`progress` 那一列）。
        std::vector<std::pair<UnitId, int>>& before = p_->dist_before[ui];
        before.clear();
        before.reserve(units.size());
        for (const UnitId id : units) {
            before.emplace_back(id, Impl::dist_to_keep(w, id));
        }

        slice.assign(units.size(), UnitAction::Stop);
        for (std::size_t k = 0; k < units.size(); ++k) {
            const std::uint16_t sq = w.unit_squad(units[k]);
            // 找这个单位所属编队在 `leaders` 里的行号。**编队号不等于行号**
            // ——`enumerate_squads` 跳过了空编队、散兵还排在后面，所以只能查。
            // 编队数 ≤ 32，线性查足够；换成映射就要引入一个容器，而那正是
            // 确定性守卫盯的东西。
            for (int q = 0; q < nsq; ++q) {
                const UnitId lead = p_->leaders[ui][static_cast<std::size_t>(q)];
                const bool same = (sq == kNoSquad) ? (lead == units[k])
                                                   : (w.unit_squad(lead) == sq);
                if (!same) continue;
                slice[k] = actions[static_cast<std::size_t>(ui) * kMaxUnitsPerEnv +
                                   static_cast<std::size_t>(q)];
                break;
            }
        }
        w.submit_actions(p_->side, slice.data(), slice.size());
        // **替对侧下命令与动作**（`BatchedEnvInit::opponent_hook`）。
        //
        // 顺序必须夹在这里：在我方 `submit_actions` **之后**、`advance()`
        // **之前**。两侧的提交都只是入队，`advance` 才真的推演；反过来写
        // 不会报错，只会让对侧永远晚我方一拍。
        if (p_->opponent) p_->opponent(w, i);
        const int ticks = p_->max_ticks > 0
            ? std::min(p_->ticks_per_step, p_->max_ticks - p_->elapsed[ui])
            : p_->ticks_per_step;
        w.advance(ticks);
        p_->elapsed[ui] += ticks;

        // 距离缩短了几格。**只算两端都活着的单位**（见 `kTallyNames` 那条
        // 警告：按「死了就距离归零」计会把送死变成正收益）。
        //
        // `enumerate_units` 后于 `advance`，所以这里不能用它——重新枚举一遍
        // 的那份里已经没有这一步死掉的单位了，而我要的正是「谁两端都在」。
        // 改用句柄各自查：`unit_pos` 对失效句柄会抛，所以先问活没活。
        {
            double gained = 0.0;
            for (const auto& [id, d0] : before) {
                if (!w.alive(id)) continue;        // 这一步死了：不计
                gained += static_cast<double>(d0 - Impl::dist_to_keep(w, id));
            }
            p_->progress[ui] += gained;
        }
        // 终局：Keep 掉了**或**超时。超时那一半是 episode 的定义
        // （见 `max_ticks_per_episode`）——没有它，打不动的策略会把一局
        // 无限拖下去，而 PPO 收不到任何奖励信号。
        const bool timed_out =
            p_->max_ticks > 0 && p_->elapsed[ui] >= p_->max_ticks;
        w.enumerate_units(Side::Attacker, p_->ids[ui]);
        bool pending_attack = false;
        if (p_->ids[ui].empty()) {
            // An arrow already in flight may still destroy the keep after its
            // shooter dies. Let those attacks settle before declaring elimination.
            const auto view = w.view(Side::Attacker);
            const auto sides = view.proj_side();
            for (const auto side : sides) if (side == Side::Attacker) pending_attack = true;
        }
        p_->ends[ui] = !Impl::keep_alive(w) ? EpisodeEnd::KeepDestroyed
            : p_->ids[ui].empty() && !pending_attack ? EpisodeEnd::AttackersEliminated
            : timed_out ? EpisodeEnd::Timeout : EpisodeEnd::Running;
        done[ui] = p_->ends[ui] == EpisodeEnd::Running ? 0u : 1u;
    });
    p_->refresh_counts();
}

void BatchedEnv::agent_keys(std::span<std::int64_t> out) const {
    if (out.size() != static_cast<std::size_t>(batch_size()) * kMaxUnitsPerEnv) {
        throw ContractError("BatchedEnv::agent_keys: out must be batch * max_agents");
    }
    std::fill(out.begin(), out.end(), 0);
    for (int i = 0; i < batch_size(); ++i) {
        const std::size_t ui = static_cast<std::size_t>(i);
        const World& w = *p_->worlds[ui];
        const auto& leaders = p_->leaders[ui];
        if (leaders.size() > static_cast<std::size_t>(kMaxUnitsPerEnv)) {
            throw ContractError("BatchedEnv::agent_keys: too many agents, refusing truncation");
        }
        for (std::size_t q = 0; q < leaders.size(); ++q) {
            const auto sq = w.unit_squad(leaders[q]);
            out[ui * kMaxUnitsPerEnv + q] = sq != kNoSquad
                ? static_cast<std::int64_t>(sq) + 1
                : (std::int64_t{1} << 32) + leaders[q].raw();
        }
    }
}

void BatchedEnv::action_masks(std::span<std::uint16_t> out) const {
    const int n = batch_size();
    if (out.size() != static_cast<std::size_t>(n) *
                          static_cast<std::size_t>(kMaxUnitsPerEnv)) {
        throw ContractError(
            "BatchedEnv::action_masks: out 必须是 batch × kMaxUnitsPerEnv");
    }
    // 尾部（没有 agent 的行）必须清成「只允许 Stop」而不是 0：全 0 掩码会让
    // 策略侧的 softmax 得到全 -inf ⇒ NaN。`Stop` 永远合法（`action.hpp`）。
    const std::uint16_t only_stop =
        static_cast<std::uint16_t>(1u << static_cast<unsigned>(UnitAction::Stop));
    std::fill(out.begin(), out.end(), only_stop);
    p_->for_each_env(n, [&](int i) {
        const std::size_t ui = static_cast<std::size_t>(i);
        const World& w = *p_->worlds[ui];
        const std::vector<UnitId>& leaders = p_->leaders[ui];
        const int m = std::min(static_cast<int>(leaders.size()), kMaxUnitsPerEnv);
        for (int q = 0; q < m; ++q) {
            out[ui * static_cast<std::size_t>(kMaxUnitsPerEnv) +
                static_cast<std::size_t>(q)] =
                w.action_mask(leaders[static_cast<std::size_t>(q)]);
        }
    });
}

void BatchedEnv::take_tally(std::span<float> out) {
    const int n = batch_size();
    if (out.size() != static_cast<std::size_t>(n) *
                          static_cast<std::size_t>(kTallyFields)) {
        throw ContractError("BatchedEnv::take_tally: out 必须是 batch × kTallyFields");
    }
    // **不并行**：它是每步一次的 O(batch × 7) 拷贝，起线程的开销比它自己大。
    // 而且 `take_tally` 会清零 ⇒ 它是写操作，放进 `for_each_env` 只会让
    // 「为什么结果确定」的论证变复杂（那一段的注释专门讲这个）。
    for (int i = 0; i < n; ++i) {
        const std::size_t ui = static_cast<std::size_t>(i);
        const World::Tally t = p_->worlds[ui]->take_tally(p_->side);
        const std::size_t o = ui * static_cast<std::size_t>(kTallyFields);
        // 顺序 = `kTallyNames`。**float 装 int64 会在很大的数上丢精度**，
        // 但这些量的量级是伤害/造价（几千到几万），float 的 24 位有效位
        // 装得下——而 `train/` 那侧本来就要转 float32 喂网络。
        out[o + 0] = static_cast<float>(t.dmg_to_units);
        out[o + 1] = static_cast<float>(t.dmg_to_blds);
        out[o + 2] = static_cast<float>(t.units_killed);
        out[o + 3] = static_cast<float>(t.blds_destroyed);
        out[o + 4] = static_cast<float>(t.bld_value);
        out[o + 5] = static_cast<float>(t.scouts_killed);
        out[o + 6] = static_cast<float>(t.losses);
        // 第 8 列由这一层给（不是 `World::Tally` 的字段），同样读走即清。
        out[o + 7] = static_cast<float>(p_->progress[ui]);
        p_->progress[ui] = 0.0;
    }
}

std::vector<double> BatchedEnv::potentials() const {
    std::vector<double> result(p_->worlds.size(), 0.0);
    for (std::size_t i = 0; i < p_->worlds.size(); ++i) {
        const auto& w=*p_->worlds[i];
        const auto intent=p_->intent_for(i);
        for (const UnitId id : p_->ids[i]) {
            bool economy=false;
            if(!intent.groups.empty() && !intent.economy.empty()) {
                const auto squad=w.unit_squad(id);
                for(std::size_t q=0;q<p_->leaders[i].size();++q) {
                    const auto leader=p_->leaders[i][q];
                    if(id==leader || (squad!=kNoSquad && squad==w.unit_squad(leader))) {
                        economy=intent.groups[q]==1;break;
                    }
                }
            }
            int distance=Impl::dist_to_keep(w,id);
            if(economy) {
                distance=std::max(w.width(),w.height());
                const auto pos=grid_of(w.unit_pos(id));
                for(auto goal:intent.economy)
                    distance=std::min(distance,std::max(std::abs(int(pos.i)-goal.i),std::abs(int(pos.j)-goal.j)));
            }
            result[i] -= distance;
        }
    }
    return result;
}

void BatchedEnv::reset_one(int i, WorldInit init) {
    if (i < 0 || i >= batch_size()) throw ContractError("BatchedEnv: 环境下标越界");
    const std::size_t ui = static_cast<std::size_t>(i);
    p_->worlds[ui] = p_->make_world(std::move(init), i);
    p_->worlds[ui]->enumerate_units(p_->side, p_->ids[ui]);
    p_->worlds[ui]->enumerate_squads(p_->side, p_->leaders[ui]);
    p_->counts[ui] = static_cast<int>(p_->leaders[ui].size());
    p_->elapsed[ui] = 0;   // 每个任务具有相同的完整时限
    // **`progress` 必须一起清**：不清的话上一局最后一步的位移会算进新局的
    // 第一步，而新局的单位在集结点、距离是满的 ⇒ 那是一笔凭空的大额奖励。
    p_->progress[ui] = 0.0;
    p_->ends[ui] = EpisodeEnd::Running;
    p_->dist_before[ui].clear();
}

}  // namespace rts
