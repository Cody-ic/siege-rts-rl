// 观测打包：把 `WorldView` 变成 `rts/obs.hpp` 那张注册表描述的张量。
//
// `obs.hpp` 定的是**布局**（有哪些通道、怎么归一化、指纹是多少），本文件是
// 唯一按那份布局真的写字节的地方。两者刻意分开：布局要被 Python 侧读，
// 打包要被 C++ 侧调，而把它们塞进一个头会让 `bindings/` 顺带拖进打包实现。
//
// ## 硬要求 1 落在这里，而且现在是可执行的
//
// `CLAUDE.md`「RL 侧两条硬要求」第 1 条：
//
//   > **AI 的观测必须是它自己的迷雾状态，不得是 ground truth。**
//
// `WorldView` 是 god 视角（它文件头明说了，那是不变量 2 的落点），所以这一条
// **只能由打包器保证**。此前 `rts_core 接口契约.md` §5.5 把它标成「要靠评审而非
// 编译器把关」，理由是「那一层还没有代码」——那句话到本文件为止成立。
//
// 现在它由三条断言钉住（`tests/obs_pack_test.cpp`，`[obspack]`）：
//
//   1. 迷雾之外的敌人：`enemy_*` 三条全 0 且 `visible` 为 0
//   2. 只在记忆里的建筑：`wall_hp` 有值，而同格的**单位**通道仍为 0
//      （`CLAUDE.md`「迷雾记建筑、不记单位」——记住的单位位置会主动误导）
//   3. 己方单位不受迷雾影响（自己的部队自己总是知道在哪）
//
// 破坏性验证在那份测试的注释里：摘掉 `vis` 那道门 ⇒ 第 1、2 条红。
//
// ## 不分配、不缓存、不算 field
//
// 调用方给缓冲区（`std::span<float>`），本文件一个字节都不 new——热路径里
// 「Python 一次调用推进 N 个 tick」（不变量 1），而 N × 单位数次分配会吃掉吞吐。
//
// `FlowField` 也**由调用方传入**而不是在里面算：`rts/flow.hpp` 自己定了
// 「调用方按 (mover, tier) 缓存与失效」，在打包器里现算等于每个单位重算一张。
// 传 `nullptr` 表示「这一批不喂方向场」，那两条通道写 0——训练早期没有宏观
// 目标时是正常形态，不是缺陷。

#ifndef RTS_OBS_PACK_HPP
#define RTS_OBS_PACK_HPP

#include <cstdint>
#include <span>

#include "rts/flow.hpp"
#include "rts/obs.hpp"
#include "rts/types.hpp"
#include "rts/world_view.hpp"

namespace rts {

// ——三块缓冲区的元素数——
//
// **通道在最后一维**（`[k][k][C]`，行优先）。理由是 PyTorch 的
// `permute(0,3,1,2)` 是零拷贝视图，而按 `[C][k][k]` 打包要在 C++ 侧
// 每通道跳 K² 步写——同样的字节数，cache 行为差一个量级。
inline constexpr int kObsCellFloats = kObsK * kObsK * kObsChannelCount;
inline constexpr int kObsSelfFloats = kObsSelfCount;
inline constexpr int kObsGlobalFloats = kObsGlobalCount;

// 归一化用到的两个分母。**都是占位值，且刻意不在 `StatsTable` 里。**
//
// 判据是「仿真读不读它」：`StatsTable` 那条纪律（`rts/stats.hpp`：待标定数值
// 只能从 `WorldInit::stats` 进仿真）管的是**影响解算**的数字，而这两个只影响
// 张量的尺度、一个 tick 都不推进。放进表里会让 `kStatsShapeTag` 进格、
// 旧回放报 `StatsMismatch`，而那次重录换不来任何可标定性。
//
// **`cell_capacity` 在仿真里没有对应规则**：单位互不阻挡（`mechanics.cpp` 的
// `cell_open` 只判地形 / 障碍 / 建筑），所以一格能站多少人是无界的。
// 它纯粹是「多少算挤」这个观测尺度的选择。
//
// **移进 `StatsTable` 的判据**：等这两个数需要与平衡数值**一起**标定时
// （空军上限曲线定了、或成本产出矩阵要求扫这个尺度），那时它们才有理由
// 进数据文件。现在进去只是多一次重录。
struct ObsNorms {
    // `ObsNorm::Count` 的分母：一格里「算挤」的单位数。
    float cell_capacity = 4.0f;
    // `ObsGlobal::AerialAliveFrac` 的分母：本波空军上限。
    //
    // 2026-09-03 起这条通道**不再恒为 0**：`Wraith` 改判空中单位，而它第 2 波起
    // 恒一进 demo 编成（`DemoBattle`），于是通道值 = 存活窥使数 ÷ `aerial_cap`。
    // `Phoenix` 仍不进 demo 编成。
    float aerial_cap = 4.0f;
};

// 打包一个单位的观测。
//
// `self` 必须是**活着**且属于 `view.side()` 那一侧的单位——观测是给它自己用的，
// 拿别人的视角打包是把 ground truth 换了个形状（有断言）。
//
// `flow` 可为 `nullptr`（见文件头）。三块缓冲区的长度必须**恰好**等于上面三个
// 常量，多一个少一个都抛：长度对不上时 `reshape` 在 Python 侧照样会成功，
// 而那正是 `obs.hpp` 文件头列为「本项目最防的失败形态」的那一类。
void pack_unit_obs(const WorldView& view, UnitId self, const FlowField* flow,
                   const ObsNorms& norms, std::span<float> cells,
                   std::span<float> self_vec, std::span<float> globals);

// 全局标量只与 `World` 有关、与哪个单位无关，所以单独给一个入口：
// 一批单位共用一份，`pack_unit_obs` 里那次调用是为了「一个单位一次调完」
// 的方便，批量打包时不该每个单位重算。
void pack_globals(const WorldView& view, const ObsNorms& norms,
                  std::span<float> globals);

}  // namespace rts

#endif  // RTS_OBS_PACK_HPP
