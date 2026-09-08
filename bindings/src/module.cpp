// `rts_native`：Python 侧唯一的入口。
//
// 不变量 3（`CLAUDE.md`「架构：三层隔离」）：**Python 经 pybind11 in-process
// 直接调 C++，不走 IPC、不做状态序列化。** 这个文件就是那一条的全部实现。
//
// ## 它刻意很薄
//
// 热路径（并行 step N 局、打包张量）在 `rts::BatchedEnv` 里，那是纯 C++、
// 进默认构建、被 GCC 验、有 ctest。**本文件不含任何算法**，只做三件事：
//
//   1. 把 `obs.hpp` 那张注册表原样递出去（名字、归一化方式、版本、指纹）
//   2. 把 `BatchedEnv` 的三个缓冲区包成 numpy 数组（**零拷贝**）
//   3. 把「地图 JSON + 数值表 JSON → 建局参数」这条路暴露成一个函数
//
// 第 3 条尤其刻意：`WorldInit` 有地形 vector、建筑列表、单位列表、
// 一整张 `StatsTable`（几十个字段）。把它逐字段搬到 Python 是几百行绑定 +
// 一份必然漂移的副本。而 `game::MapLoader` / `StatsLoader` / `make_world_init`
// 已经是**游戏自己在用**的那条路——绑定层复用它，于是「Python 造的局面」
// 与「双击 exe 玩的局面」是同一个来源。
//
// ## 零拷贝，且**由 Python 持有缓冲区**
//
// `observe()` 接收 numpy 数组、往里写；不返回新数组。理由是训练回路每步都要
// 这三块，让 Python 侧预分配一次、反复复用，比每步 new 一块再让 GC 回收便宜
// ——而后者在 40 单位 × 15×15×14 float 的规模上每步是 1.2 MB。
//
// 代价是调用方要自己保证形状。所以 `BatchedEnv::observe` 那道长度检查在这里
// 是**唯一的护栏**：形状错了必须抛，因为 numpy 的 `reshape` 会照样成功
// （`obs.hpp` 文件头列的那个「最防的失败形态」）。

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "game/world_builder.hpp"
#include "game/attacker_macro.hpp"
#include "scripted_defender.hpp"
#include "rts/action.hpp"
#include "rts/batched_env.hpp"
#include "rts/obs.hpp"
#include "rts/roster.hpp"
#include "rts/combat_math.hpp"

namespace py = pybind11;

namespace {

struct WorldFactory {
    game::MapData map;
    rts::StatsTable stats;
    WorldFactory(const std::string& map_path, const std::string& stats_path)
        : map(game::MapLoader::from_file(map_path)),
          stats(game::StatsLoader::from_file(stats_path)) {}

    rts::WorldInit make(std::uint64_t seed, std::int32_t level,
        const std::vector<std::tuple<int, float, float, int, int>>& attackers) const {
        auto init = game::make_world_init(map, stats, seed, level);
        for (const auto& [t, x, y, lvl, sq] : attackers) {
            if (t < 0 || t >= rts::kUnitTypeCount || lvl < 1 || sq < -1 || sq >= rts::kNoSquad) {
                throw rts::ContractError("WorldFactory: invalid attacker roster");
            }
            const auto ut = static_cast<rts::UnitType>(t);
            if (rts::side_of(ut) != rts::Side::Attacker) {
                throw rts::ContractError("WorldFactory: attacker side required");
            }
            const auto hp = rts::apply_permille(stats.of(ut).max_hp,
                {rts::level_permille(lvl,stats.global.hp_permille_per_level)});
            const auto squad = sq < 0 ? rts::kNoSquad : static_cast<std::uint16_t>(sq);
            init.units.push_back(rts::UnitInit{ut, rts::Vec2{x, y}, lvl, hp, hp, squad});
        }
        return init;
    }
};

// numpy 数组 → `std::span`。**要求 C 连续且 dtype 恰好对**：
// 不检查的话 pybind11 会悄悄做一次转换拷贝，于是我们写进去的是那份**副本**，
// 训练侧读到的永远是上一步的值——不报错、不崩，只是观测恒定不变。
// **模板参数写 `py::array_t<T, Flags>` 时 Flags 是类型的一部分。**
// 初版把辅助函数写成 `c_style | forcecast` 而 lambda 形参写成 `c_style`，
// 那是**两个不同的类型**，于是 5 处 `no matching function`。
// 这里改成对 `array_t` 的任意实例化都成立，免得两处 Flags 必须逐字一致。
//
// **刻意不带 `forcecast`。** 带了的话 dtype 或步长不对时 pybind11 会**悄悄做一次
// 转换拷贝**，我们于是写进那份副本、训练侧读到的永远是上一步的值
// ——不报错、不崩，只是观测恒定不变。不带它则 dtype 不符直接抛。
template <typename Arr>
auto as_span(Arr& a) {
    using T = typename Arr::value_type;
    py::buffer_info bi = a.request(true);
    return std::span<T>(static_cast<T*>(bi.ptr), static_cast<std::size_t>(bi.size));
}

template <typename Arr>
auto as_cspan(const Arr& a) {
    using T = typename Arr::value_type;
    py::buffer_info bi = a.request(false);
    return std::span<const T>(static_cast<const T*>(bi.ptr),
                              static_cast<std::size_t>(bi.size));
}

}  // namespace

PYBIND11_MODULE(rts_native, m) {
    m.def("squad_cap",[](int type) {
        if(type<0 || type>=rts::kUnitTypeCount) throw rts::ContractError("Invalid unit type");
        return game::squad_cap_of(static_cast<rts::UnitType>(type));
    });
    m.attr("SIMULATION_FINGERPRINT") = RTS_SIMULATION_FINGERPRINT;
    m.attr("BUILD_MODE") = RTS_NATIVE_BUILD_MODE;
    m.doc() = "siege-rts-rl 的原生仿真（不变量 3：in-process，不走 IPC）";

    // ——观测布局：Python 侧一律从这里读，**不硬编码**——
    //
    // `obs.hpp` 文件头写了为什么：两侧不一致时**不会有任何东西报错**，
    // `reshape` 照样成功、网络照样收敛（收敛到把「墙」当成「敌方血量」的表示上）。
    py::module_ obs = m.def_submodule("obs", "观测布局注册表（rts/obs.hpp）");
    obs.attr("VERSION") = rts::kObsVersion;
    obs.attr("LAYOUT_FINGERPRINT") = rts::kObsLayoutFingerprint;
    obs.attr("K") = rts::kObsK;
    obs.attr("CHANNEL_COUNT") = rts::kObsChannelCount;
    obs.attr("PAIRED_COUNT") = rts::kObsPairedCount;
    obs.attr("SELF_COUNT") = rts::kObsSelfCount;
    obs.attr("GLOBAL_COUNT") = rts::kObsGlobalCount;
    obs.attr("CELL_FLOATS") = rts::kObsCellFloats;
    obs.attr("SELF_FLOATS") = rts::kObsSelfFloats;
    obs.attr("GLOBAL_FLOATS") = rts::kObsGlobalFloats;
    // **它是 agent 数（编队数）的上限，不是单位数。** 名字保留是为了不破
    // 下游，但语义 2026-09-05 变了 —— `train/` 侧一律从这里读，别抄数。
    obs.attr("MAX_UNITS_PER_ENV") = rts::BatchedEnv::kMaxUnitsPerEnv;
    obs.attr("NO_SQUAD") = rts::kNoSquad;
    obs.attr("TALLY_FIELDS") = rts::BatchedEnv::kTallyFields;
    {
        py::list tn;
        for (const std::string_view n : rts::BatchedEnv::kTallyNames) {
            tn.append(std::string(n));
        }
        obs.attr("TALLY_NAMES") = tn;
    }
    obs.attr("UNIT_TYPE_COUNT") = rts::kUnitTypeCount;
    obs.attr("ACTION_COUNT") = rts::kUnitActionCount;

    {
        // 通道表：(名字, 归一化方式, 是否成对)。**顺序即张量的通道顺序。**
        py::list ch;
        for (const rts::ObsChannelSpec& c : rts::kObsChannels) {
            ch.append(py::make_tuple(std::string(c.name),
                                     std::string(rts::ident_of(c.norm)), c.paired));
        }
        obs.attr("CHANNELS") = ch;

        py::list sn;
        for (const std::string_view n : rts::kObsSelfNames) sn.append(std::string(n));
        obs.attr("SELF_NAMES") = sn;

        py::list gn;
        for (const std::string_view n : rts::kObsGlobalNames) gn.append(std::string(n));
        obs.attr("GLOBAL_NAMES") = gn;

        // 兵种名与动作名：one-hot 与动作头的下标含义。
        py::list un;
        for (int i = 0; i < rts::kUnitTypeCount; ++i) {
            un.append(std::string(rts::ident_of(static_cast<rts::UnitType>(i))));
        }
        obs.attr("UNIT_TYPE_NAMES") = un;

        py::list an;
        for (int i = 0; i < rts::kUnitActionCount; ++i) {
            an.append(std::string(rts::ident_of(static_cast<rts::UnitAction>(i))));
        }
        obs.attr("ACTION_NAMES") = an;
    }

    py::enum_<rts::Side>(m, "Side")
        .value("Defender", rts::Side::Defender)
        .value("Attacker", rts::Side::Attacker);

    py::class_<rts::ObsNorms>(m, "ObsNorms")
        .def(py::init<>())
        .def_readwrite("cell_capacity", &rts::ObsNorms::cell_capacity)
        .def_readwrite("aerial_cap", &rts::ObsNorms::aerial_cap);

    // ——建局：只传路径，不暴露 `WorldInit`——
    //
    // 返回的是一个不透明句柄（`WorldInit` 本身），Python 只把它递给
    // `BatchedEnv`，不读它的字段。这样绑定面不随 `WorldInit` 的字段增减而变，
    // 而那个结构还在长（机制每落一批就可能加一个初始状态）。
    py::class_<rts::WorldInit>(m, "WorldInit", "不透明的建局参数，只用来递给 BatchedEnv");

    py::class_<WorldFactory>(m, "WorldFactory")
        .def(py::init<const std::string&, const std::string&>(),
             py::arg("map_path"), py::arg("stats_path"))
        .def("make", &WorldFactory::make, py::arg("seed") = 0,
             py::arg("nominal_level") = 1,
             py::arg("attackers") = std::vector<std::tuple<int, float, float, int, int>>{});

    m.def(
        "make_world_init",
        [](const std::string& map_path, const std::string& stats_path,
           std::uint64_t seed, std::int32_t nominal_level,
           const std::vector<std::tuple<int, float, float, int, int>>& attackers) {
            const game::MapData map = game::MapLoader::from_file(map_path);
            const rts::StatsTable stats = game::StatsLoader::from_file(stats_path);
            rts::WorldInit init =
                game::make_world_init(map, stats, seed, nominal_level);
            // 攻方编成由**调用方**给。
            //
            // **这一条不是接口偷懒，是设计。** `CLAUDE.md`：「一波 = 一个 RL
            // episode」，而本波的编成是**宏观层**（bandit 尺度、一波决策一次）
            // 的产物——那一层住在 `train/`。`World` 自己不生波：生波逻辑在
            // `game::DemoBattle` 里，那是**演示**的波次循环，不该被训练回路复用
            // （它按占位曲线生兵，而训练要的是策略给出的编成）。
            //
            // 不给这个参数的话 `BatchedEnv` 会拿到一个**永远没有攻方单位**的局面
            // ——实测：推 2400 tick 仍然是 0。那不报错，只是每一步都在打包空张量。
            // 第五个字段是**编队号**（`squad`，-1 = `kNoSquad` 散兵）。
            // **agent = 编队**（`CLAUDE.md`「攻方 RL 控制的是每一支编队」），
            // 所以张量的第二维是编队而不是单位 ⇒ 编成必须能表达分队，
            // 否则 `BatchedEnv` 只能把每个单位当一支、`kMaxUnitsPerEnv`
            // 立刻不够（攻方 ~70 个单位 vs 32 行）。
            for (const auto& [t, x, y, lvl, sq] : attackers) {
                const auto ut = static_cast<rts::UnitType>(t);
                const std::int64_t hp = stats.of(ut).max_hp;
                const auto squad = sq < 0 ? rts::kNoSquad
                                          : static_cast<std::uint16_t>(sq);
                init.units.push_back(rts::UnitInit{ut, rts::Vec2{x, y},
                                                   lvl, hp, hp, squad});
            }
            return init;
        },
        py::arg("map_path"), py::arg("stats_path"), py::arg("seed") = 0,
        py::arg("nominal_level") = 1,
        py::arg("attackers") = std::vector<std::tuple<int, float, float, int, int>>{},
        "从地图 JSON + 数值表 JSON 装配建局参数。走的是游戏自己那条路"
        "（game::MapLoader / StatsLoader / make_world_init），"
        "所以 Python 造的局面与双击 exe 玩的局面是同一个来源。"
        "attackers 是 [(unit_type, x, y, level, squad), ...]：**本波编成由调用方给**，"
        "因为它是宏观层的产物，而 World 自己不生波。squad = 编队号，-1 = 散兵；"
        "**agent = 编队**，所以观测/动作张量的第二维是编队数而不是单位数。");

    // 地图上的**集结点与堡垒位置**。
    //
    // `train/` 必须知道兵该摆哪：`make_world_init(attackers=...)` 要坐标，
    // 而**摆错地方不报错**——我第一次跑 PPO 就把兵摆在图的空角落
    // （离 keep 65 格），于是 12000 tick 里战果全零、掩码里连一个攻击位都
    // 没亮过，而训练照样「跑得很顺」。所以这个查询不是便利函数，
    // 是防那一类静默失败的。
    m.def(
        "map_sites",
        [](const std::string& map_path) {
            const game::MapData map = game::MapLoader::from_file(map_path);
            py::dict d;
            py::list sp;
            for (const game::SpawnPoint& s : map.spawns()) {
                sp.append(py::make_tuple(s.pos.i, s.pos.j));
            }
            d["spawns"] = sp;
            d["keep"] = py::make_tuple(map.keep().i, map.keep().j);
            d["size"] = py::make_tuple(map.width(), map.height());
            return d;
        },
        py::arg("map_path"),
        "读地图的集结点 / 堡垒 / 图幅。攻方编成该摆在集结点上——摆在别处"
        "不会报错，只会让 episode 永不终局、战果恒零。");

    py::class_<rts::BatchedEnv>(m, "BatchedEnv")
        .def(py::init([](std::vector<rts::WorldInit> worlds, rts::Side side,
                         int ticks_per_step, int threads, int max_ticks_per_episode,
                         const std::string& defender_map, int defender_seed,
                         int defender_macro_period, rts::ObsNorms norms,
                         const std::vector<std::string>& defender_maps,
                         int defender_prepare_ticks) {
                 if (defender_prepare_ticks < 0 || defender_prepare_ticks > 2400)
                     throw rts::ContractError("defender_prepare_ticks must be in [0,2400]");
                 if (defender_prepare_ticks && (ticks_per_step <= 0 ||
                     (defender_map.empty() && defender_maps.empty())))
                     throw rts::ContractError("Preparation requires a scripted defender and positive decision period");
                 rts::BatchedEnvInit bi;
                 bi.worlds = std::move(worlds);
                 bi.side = side;
                 bi.ticks_per_step = ticks_per_step;
                 bi.threads = threads;
                 bi.max_ticks_per_episode = max_ticks_per_episode;
                 bi.norms = norms;
                 // **接上真正的守方**（`scripted_defender.hpp`）。
                 //
                 // 给了地图路径就接、不给就不接（默认不接 = 此前的行为，
                 // 对侧一动不动）。**它必须是 `shared_ptr` 捕获进
                 // `std::function`**：`BatchedEnv` 只存那个 function，
                 // 而大脑得活到最后一次 `step()`。
                 //
                 // ⚠️ 这里**再读一次地图**（`MapLoader::from_file`）。
                 // 不复用 `make_world_init` 那次是因为那一层只返回
                 // `WorldInit`、不返回 `MapData`，而 `DefenderMacro` 要的
                 // 是后者（它构造时要算环半径与候选塔位）。一次性开销。
                 if (!defender_map.empty() && !defender_maps.empty())
                     throw rts::ContractError("Choose defender_map or defender_maps, not both");
                 if (!defender_map.empty() || !defender_maps.empty()) {
                     std::vector<game::MapData> maps;
                     if(!defender_map.empty()) maps.push_back(game::MapLoader::from_file(defender_map));
                     for(const auto& path:defender_maps) maps.push_back(game::MapLoader::from_file(path));
                     auto brain = std::make_shared<bindings::ScriptedDefender>(
                         maps, static_cast<int>(bi.worlds.size()),
                         static_cast<std::uint64_t>(defender_seed),
                         game::MacroParams{}, game::ScriptParams{},
                         defender_macro_period);
                     bi.opponent_hook = [brain](rts::World& w, int i) {
                         (*brain)(w, i);
                     };
                     if (defender_prepare_ticks) {
                         bi.world_factory = [brain, defender_prepare_ticks, ticks_per_step](rts::WorldInit init, int i) {
                             return brain->prepare(std::move(init), i, defender_prepare_ticks, ticks_per_step);
                         };
                     }
                 }
                 return std::make_unique<rts::BatchedEnv>(std::move(bi));
             }),
             py::arg("worlds"), py::arg("side") = rts::Side::Attacker,
             py::arg("ticks_per_step") = 6, py::arg("threads") = 0,
             py::arg("max_ticks_per_episode") = 2400,
             py::arg("defender_map") = std::string{},
             py::arg("defender_seed") = 20260907,
             py::arg("defender_macro_period") = 1,
             py::arg("norms") = rts::ObsNorms{},
             py::arg("defender_maps") = std::vector<std::string>{},
             py::arg("defender_prepare_ticks") = 0,
             "defender_map 给了就**接上真正的守方**（game::DefenderScript + "
             "DefenderMacro，逐局各一份）。**不给 = 对侧一动不动**——那是 "
             "2026-09-07 之前的行为，而它让 enemy_* 那几条观测通道十万局零"
             "梯度。注意：光把守方单位摆进 World 是不够的，单位出生动作是 "
             "Stop、攻击阶段只处理攻击类动作 ⇒ 没人替它们下令就一枪不放。")
        .def("agent_keys", [](const rts::BatchedEnv& e,
                              py::array_t<std::int64_t, py::array::c_style> out) {
            const auto target = as_span(out);
            py::gil_scoped_release nogil;
            e.agent_keys(target);
        }, py::arg("out"))
        .def_property_readonly("batch_size", &rts::BatchedEnv::batch_size)
        .def_property_readonly("max_ticks_per_episode",
                               &rts::BatchedEnv::max_ticks_per_episode)
        .def_property_readonly("side", &rts::BatchedEnv::side)
        .def_property_readonly(
            "unit_counts",
            [](const rts::BatchedEnv& e) {
                const std::span<const int> c = e.unit_counts();
                // 这一份**是拷贝**（它很小：batch 个 int），因为返回 span 的视图
                // 会在 `step()` 之后失效，而 Python 侧留着一个旧视图不会有任何提示。
                return std::vector<int>(c.begin(), c.end());
            },
            "每一局当前有多少个属于 side 的活单位（张量里前几段有效）")
        .def(
            "observe",
            [](rts::BatchedEnv& e, py::array_t<float, py::array::c_style> cells,
               py::array_t<float, py::array::c_style> self_vec,
               py::array_t<float, py::array::c_style> globals) {
                // **顺序不可颠倒：先取指针，再放 GIL。**
                //
                // `as_span()` 里的 `a.request()` 走的是 Python 缓冲协议——
                // 那是在**碰 Python 对象**。放了 GIL 再调它是未定义行为，
                // 实测就是 SIGSEGV（退出码 139），而且**崩在 `observe` 上**，
                // 看起来像 `BatchedEnv` 有问题，与真因隔着一层。
                //
                // 取到裸指针之后就与 Python 无关了，那时才能放 GIL。
                const auto cs = as_span(cells);
                const auto ss = as_span(self_vec);
                const auto gs = as_span(globals);
                // 放 GIL 的理由：`observe` 里面自己开线程跑一段长耗时的纯 C++，
                // 持着锁会挡住 Python 主线程上的别的工作（数据搬运、日志）。
                py::gil_scoped_release nogil;
                e.observe(cs, ss, gs);
            },
            py::arg("cells"), py::arg("self_vec"), py::arg("globals"),
            "往调用方给的三块 numpy 缓冲区里写观测（零拷贝）。形状必须恰好，"
            "否则抛——numpy 的 reshape 会照样成功，那是最难查的一类错。")
        .def(
            "step",
            [](rts::BatchedEnv& e,
               const py::array_t<std::uint8_t, py::array::c_style> actions,
               py::array_t<std::uint8_t, py::array::c_style> done) {
                // 动作以 uint8 传（`UnitAction` 是 `std::uint8_t` 底层类型），
                // 在这里重解释。**不做范围检查**：`World::submit_actions`
                // 自己有掩码校验，两处做等于两处要维护同一份规则。
                const std::span<const std::uint8_t> raw = as_cspan(actions);
                const std::span<const rts::UnitAction> acts(
                    reinterpret_cast<const rts::UnitAction*>(raw.data()), raw.size());
                // 同 `observe`：`as_span(done)` 也必须在放 GIL **之前**取。
                const auto ds = as_span(done);
                py::gil_scoped_release nogil;
                e.step(acts, ds);
            },
            py::arg("actions"), py::arg("done"),
            "推进一批。actions 是 uint8 的 batch × MAX_UNITS_PER_ENV，"
            "done 是 uint8 的 batch。终局的局不自动重置——重置时机归 train/。")
        .def(
            "action_masks",
            [](const rts::BatchedEnv& e,
               py::array_t<std::uint16_t, py::array::c_style> out) {
                const auto os = as_span(out);
                py::gil_scoped_release nogil;
                e.action_masks(os);
            },
            py::arg("out"),
            "写入动作掩码（batch × MAX_UNITS_PER_ENV，uint16）。第 k 位 = "
            "ACTION_NAMES[k] 合法。**没有它策略学不动**：非法动作会被静默"
            "拒成 Stop，梯度里全是噪声。没有 agent 的行填「只允许 Stop」"
            "而不是 0 —— 全 0 掩码会让 softmax 得到 NaN。")
        .def(
            "take_tally",
            [](rts::BatchedEnv& e, py::array_t<float, py::array::c_style> out) {
                const auto os = as_span(out);
                py::gil_scoped_release nogil;
                e.take_tally(os);
            },
            py::arg("out"),
            "读走这一步的战果（batch × TALLY_FIELDS，float32），**读走即清**。"
            "列的顺序 = obs.TALLY_NAMES。权重不在 C++ 侧——那是训练超参，"
            "这一层只给「发生了什么」。")
        .def_property_readonly("potentials", &rts::BatchedEnv::potentials)
        .def_property_readonly("episode_ends", [](const rts::BatchedEnv& e) {
            const auto ends = e.episode_ends();
            return std::vector<rts::BatchedEnv::EpisodeEnd>(ends.begin(), ends.end());
        })
        .def("reset_one", &rts::BatchedEnv::reset_one, py::arg("i"), py::arg("init"),
             "把第 i 局换成具有完整时限的新局面")
        .def(
            "state_hash",
            [](const rts::BatchedEnv& e, int i) {
                return e.world_at(i).state_hash();
            },
            py::arg("i"), "第 i 局的状态哈希（调试与确定性核对用，不在热路径上）");

    py::enum_<rts::BatchedEnv::EpisodeEnd>(m, "EpisodeEnd")
        .value("Running", rts::BatchedEnv::EpisodeEnd::Running)
        .value("KeepDestroyed", rts::BatchedEnv::EpisodeEnd::KeepDestroyed)
        .value("Timeout", rts::BatchedEnv::EpisodeEnd::Timeout)
        .value("AttackersEliminated", rts::BatchedEnv::EpisodeEnd::AttackersEliminated);

    // `UnitAction` 的枚举值：Python 侧构造动作数组要用它，而硬编码 0..12
    // 与硬编码通道顺序是同一类错。
    py::module_ act = m.def_submodule("action", "战术动作枚举（rts/action.hpp）");
    for (int i = 0; i < rts::kUnitActionCount; ++i) {
        const auto a = static_cast<rts::UnitAction>(i);
        act.attr(std::string(rts::ident_of(a)).c_str()) = static_cast<std::uint8_t>(i);
    }
}
