#include "rts/batched_env.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <thread>

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
            worlds[static_cast<std::size_t>(i)]->enumerate_units(
                side, ids[static_cast<std::size_t>(i)]);
            counts[static_cast<std::size_t>(i)] =
                static_cast<int>(ids[static_cast<std::size_t>(i)].size());
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
    for (WorldInit& wi : init.worlds) {
        p_->worlds.push_back(std::make_unique<World>(std::move(wi)));
    }
    p_->counts.assign(static_cast<std::size_t>(n), 0);
    p_->ids.resize(static_cast<std::size_t>(n));
    p_->refresh_counts();
}

BatchedEnv::~BatchedEnv() = default;

int BatchedEnv::batch_size() const noexcept {
    return static_cast<int>(p_->worlds.size());
}

Side BatchedEnv::side() const noexcept { return p_->side; }

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
        const std::vector<UnitId>& ids = p_->ids[ui];
        const int m = std::min(static_cast<int>(ids.size()), kMaxUnitsPerEnv);
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
            // `flow` 传 `nullptr` ⇒ `FlowDi` / `FlowDj` 恒 0，见头文件 `observe`
            // 那段（那两条通道本身有测试钉着，在 `tests/obs_pack_test.cpp`）。
            pack_unit_obs(v, ids[static_cast<std::size_t>(u)], nullptr, p_->norms,
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
    // 提交与推进。**每局一份动作切片，长度必须恰好等于该局的活单位数**
    // （`submit_actions` 自己有这条检查，见 `world.hpp:541`）——所以定长张量里
    // 多出来的位置在这里被裁掉，而不是喂进去。
    p_->for_each_env(n, [&](int i) {
        const std::size_t ui = static_cast<std::size_t>(i);
        World& w = *p_->worlds[ui];
        const int m = std::min(p_->counts[ui], kMaxUnitsPerEnv);
        std::vector<UnitAction> slice(
            actions.begin() + static_cast<std::ptrdiff_t>(ui) * kMaxUnitsPerEnv,
            actions.begin() + static_cast<std::ptrdiff_t>(ui) * kMaxUnitsPerEnv + m);
        // 该局单位数超过 `kMaxUnitsPerEnv` 时，尾部那些没有动作可给——补 `Stop`。
        // **`Stop` 永远合法**（`action.hpp`），所以这条补齐不会造出非法输入。
        // 攻方编成位硬性封顶 40 = `kMaxUnitsPerEnv`，所以这条分支在攻方一侧
        // 本不该发生；留着是因为守方那侧上限随堡垒等级涨，将来会走到。
        slice.resize(static_cast<std::size_t>(p_->counts[ui]), UnitAction::Stop);
        w.submit_actions(p_->side, slice.data(), slice.size());
        w.advance(p_->ticks_per_step);
        done[ui] = Impl::keep_alive(w) ? 0u : 1u;
    });
    p_->refresh_counts();
}

void BatchedEnv::reset_one(int i, WorldInit init) {
    if (i < 0 || i >= batch_size()) throw ContractError("BatchedEnv: 环境下标越界");
    const std::size_t ui = static_cast<std::size_t>(i);
    p_->worlds[ui] = std::make_unique<World>(std::move(init));
    p_->worlds[ui]->enumerate_units(p_->side, p_->ids[ui]);
    p_->counts[ui] = static_cast<int>(p_->ids[ui].size());
}

}  // namespace rts
