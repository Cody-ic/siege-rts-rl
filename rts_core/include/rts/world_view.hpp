// `World` 的只读视图。**决定 ④：把「渲染层只读」变成编译期保证，不靠约定。**
//
// 不变量 2（CLAUDE.md「架构：三层隔离」）说「渲染层是 `rts_core` 的只读观察者，
// 渲染代码不允许修改仿真状态」。一条写在文档里的约定，四个人并行开发时**一定**
// 会被某次「就改一下这个字段最省事」破掉，而症状是训练与演示的行为对不上号。
// 这个类让那行代码根本编不过。
//
// ## 它包的是引用，不是快照
//
// 内部只有一个 `const World*` 与一个 `Side`，访问器现算 span。
//
// 反过来（存一堆 span）会带来一个隐蔽的悬垂：`spawn_unit()` 让 vector 扩容，
// 上一帧存下的 span 全部失效，而它多半仍然"能用"（读到旧内存）。
// 包引用则只剩一条容易记住的规则：**`World` 活着，视图就有效。**
//
// ## 它是 god 视角。硬要求 1 不在这里，这一点必须说清
//
// CLAUDE.md「RL 侧两条硬要求」第 1 条是「AI 的观测必须是它自己的迷雾状态，
// 不得是 ground truth」。**本类不实现那一条**，它给的是真实世界 + 本侧迷雾。
//
// 分开是有理由的，不是偷懒：
//
//   * 双视图演示（README 明写不可砍）要的就是 god 视角——它要同时画出
//     真实战场与 AI 的记忆图，那必然要同时读到两者
//   * `render/` 给人类玩家画的是人类玩家该看到的，那由**守方**的迷雾决定，
//     而不是「视图本身不含真相」
//   * 逐实体做迷雾过滤会把连续数组切碎，观测打包退化成 gather，
//     正好抵掉 SoA 的全部好处
//
// **所以硬要求 1 落在观测打包那一层**（`bindings/` 与 1c）：每一条空间通道
// 都必须过 `fog()`。本类把迷雾摆在同一个对象上、且不提供任何别的迷雾来源，
// 就是为了让打包器无处可躲；除此之外它挡不住一个存心不用它的打包器。
// 这是一处**需要评审而非编译器**把关的地方，写在这里以免它被读成已经解决。

#ifndef RTS_WORLD_VIEW_HPP
#define RTS_WORLD_VIEW_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rts/action.hpp"
#include "rts/fog.hpp"
#include "rts/roster.hpp"
#include "rts/terrain.hpp"
#include "rts/types.hpp"
#include "rts/world.hpp"

namespace rts {

class WorldView {
public:
    Side side() const noexcept { return side_; }

    Tick now() const noexcept { return w_->now(); }
    int wave() const noexcept { return w_->wave(); }
    WavePhase phase() const noexcept { return w_->phase(); }
    std::int32_t nominal_level() const noexcept { return w_->nominal_level(); }

    int width() const noexcept { return w_->width(); }
    int height() const noexcept { return w_->height(); }
    const TerrainMasks& terrain() const noexcept { return w_->terrain(); }
    GridPos keep_pos() const noexcept { return w_->keep_pos(); }
    const std::vector<SpawnSite>& spawns() const noexcept { return w_->spawns(); }
    const std::vector<ResourceSite>& resources() const noexcept {
        return w_->resources();
    }

    // **本侧的**迷雾。取另一侧要经 `World::fog(Side)`——那是 god 视角的入口，
    // 只有演示与调试该走它（见文件头）。
    const FogLayer& fog() const noexcept { return w_->fog(side_); }

    // ——三组实体的连续数组——
    //
    // **长度是槽位数，含空槽。** 用 `unit_alive()` 过滤，不要假设前 N 个活着：
    // 槽位会被复用（`SlotPool`），死掉的单位留下的洞就在数组中间。
    // 这条读错的症状是「偶尔渲染出一个血量为 0 的幽灵单位」。
    std::span<const UnitType> unit_type() const noexcept { return sp(w_->u_type_); }
    std::span<const std::int32_t> unit_level() const noexcept { return sp(w_->u_level_); }
    std::span<const std::int64_t> unit_hp() const noexcept { return sp(w_->u_hp_); }
    std::span<const std::int64_t> unit_max_hp() const noexcept {
        return sp(w_->u_max_hp_);
    }
    std::span<const Vec2> unit_pos() const noexcept { return sp(w_->u_pos_); }
    std::span<const std::int32_t> unit_windup() const noexcept {
        return sp(w_->u_windup_);
    }
    std::span<const UnitAction> unit_action() const noexcept { return sp(w_->u_action_); }
    std::span<const std::uint16_t> unit_garrison() const noexcept {
        return sp(w_->u_garrison_);
    }
    // 上墙延迟剩余（第三批）。与 `unit_garrison()` 配对读：garrison != kNoSlot
    // 且 mount > 0 = 在爬（画攀爬动画、既不打也不走），mount == 0 = 已登顶。
    std::span<const std::int32_t> unit_mount() const noexcept { return sp(w_->u_mount_); }
    std::span<const std::uint8_t> unit_force() const noexcept { return sp(w_->u_force_); }
    // 已承诺攻击的目标种类与锁定落点（机制第一批）。渲染层画「出手表现」
    // 靠它们：windup > 0 且 kind != None ⇒ 这个单位正在挥（或箭在弦上），
    // 落点是 aim——**它在前摇开始那一刻就定死了**，画预兆圈画它才是诚实的。
    std::span<const TgtKind> unit_target_kind() const noexcept {
        return sp(w_->u_tgt_kind_);
    }
    std::span<const Vec2> unit_aim() const noexcept { return sp(w_->u_aim_); }
    std::span<const std::int32_t> unit_cooldown() const noexcept {
        return sp(w_->u_cd_);
    }
    std::span<const std::uint8_t> unit_alive() const noexcept {
        return std::span<const std::uint8_t>(w_->unit_pool_.alive_bytes(),
                                             w_->unit_pool_.slot_count());
    }

    std::span<const BldType> bld_type() const noexcept { return sp(w_->b_type_); }
    std::span<const GridPos> bld_pos() const noexcept { return sp(w_->b_pos_); }
    std::span<const std::int64_t> bld_hp() const noexcept { return sp(w_->b_hp_); }
    std::span<const std::int64_t> bld_max_hp() const noexcept { return sp(w_->b_max_hp_); }
    std::span<const std::int32_t> bld_work_left() const noexcept {
        return sp(w_->b_work_);
    }
    std::span<const std::int32_t> bld_windup() const noexcept {
        return sp(w_->b_windup_);
    }
    // 完工位。**判「盖没盖完”用它，不要用 `bld_work_left() == 0`**：
    // 维修复用工时字段，后一种写法会把在修的塔判成没盖完的（`rts/world.hpp`）。
    std::span<const std::uint8_t> bld_built() const noexcept {
        return sp(w_->b_built_);
    }
    // 征兵状态（`kNoTrain` = 没在练）。脚本执行层与前端的兵营面板都读它。
    std::span<const std::uint8_t> bld_train_type() const noexcept {
        return sp(w_->b_train_type_);
    }
    std::span<const std::int32_t> bld_train_left() const noexcept {
        return sp(w_->b_train_left_);
    }
    std::span<const std::uint8_t> bld_alive() const noexcept {
        return std::span<const std::uint8_t>(w_->bld_pool_.alive_bytes(),
                                             w_->bld_pool_.slot_count());
    }

    std::span<const ObstacleType> obstacle_type() const noexcept {
        return sp(w_->o_type_);
    }
    std::span<const GridPos> obstacle_pos() const noexcept { return sp(w_->o_pos_); }
    std::span<const std::int64_t> obstacle_hp() const noexcept { return sp(w_->o_hp_); }
    std::span<const std::int64_t> obstacle_max_hp() const noexcept {
        return sp(w_->o_max_hp_);
    }
    std::span<const std::uint8_t> obstacle_alive() const noexcept {
        return std::span<const std::uint8_t>(w_->obstacle_pool_.alive_bytes(),
                                             w_->obstacle_pool_.slot_count());
    }
    // 玩家的清野指令位（`CommandKind::Clear` 的解算产物）。守方脚本执行层
    // 读它决定派谁去砸；破坏本身走既有机制（移动撞上自动开始破坏）。
    std::span<const std::uint8_t> obstacle_clear_ordered() const noexcept {
        return sp(w_->o_clear_ordered_);
    }

    // ——第四组：在途弹丸（机制第五批）——
    //
    // 与前三组不同，**没有 alive 掩码**：弹丸数组每 tick 稳定压实，长度即数量。
    // 渲染层要的四样都在：位置、目的地（飞行角 = aim - pos，追踪弹逐 tick
    // 刷新）、发射建筑、发射方阵营。伤害载荷刻意不暴露——渲染画的是箭，
    // 不是伤害数字。
    //
    // **挑精灵要 `src_bld` 与 `side` 两个一起读**：
    //
    //   * `src_bld != kProjFromUnit` ⇒ 建筑射的，按 `BldType` 挑（`Flak` 是
    //     弩矢 `Bolt`，`Tower` 是箭 `Arrow`）
    //   * `src_bld == kProjFromUnit` ⇒ 单位射的，**再按 `side` 分**：
    //     守方是 `Archer` 的箭，攻方是 `Shade` 的魔法弹
    //
    // 第二条成立是因为 `launches_projectile()`（Ranged × 非空中）在**每一侧
    // 恰好命中一个兵种**（守 `Archer` / 攻 `Shade`）。这个前提由
    // `tests/scene_model_test.cpp` 的 `[scene]` 用例钉住——**不是 `static_assert`**：
    // `launches_projectile()` 是虚函数、`behavior_of()` 不是 constexpr，编译期
    // 到不了（`CLAUDE.md`「多态按兵种」那节说的就是这处代价）。而**它会随花名册
    // 变化而失效，所以不能只写在注释里**：若哪天某一侧多出第二个远程地面兵种，
    // 按侧挑精灵就会让两者共用同一张图，而那是**静默**的（画面能出，只是错）。
    //
    // 这条路径刻意**不新增 `p_src_unit_`**：`p_side_` 本来就在 `World` 里
    // （放箭那刻定格、随压实搬移、进哈希），只是没暴露。加数组要动 `World`
    // 布局、`state_hash` 喂入清单与 `kWorldHashTag`（旧回放重录）；
    // 加一个**只读访问器**这三样一个都不动。
    std::span<const Vec2> proj_pos() const noexcept { return sp(w_->p_pos_); }
    std::span<const Vec2> proj_aim() const noexcept { return sp(w_->p_aim_); }
    std::span<const std::uint8_t> proj_src_bld() const noexcept {
        return sp(w_->p_src_bld_);
    }
    std::span<const Side> proj_side() const noexcept { return sp(w_->p_side_); }

    // 编队去处表（`MoveForce` 的解算产物），下标是编队号、值是格线性下标，
    // `kNoSlot` = 没下过。与 `unit_force()` 配对读：脚本按单位的编队查这里。
    std::span<const std::uint16_t> force_target() const noexcept {
        return std::span<const std::uint16_t>(w_->force_target_.data(),
                                              w_->force_target_.size());
    }

    // 按格的驻守指令表（`Garrison` 的解算产物，第三批）。下标是格线性下标、
    // 值是编队号，`kNoForce` = 没下过。登墙本身由 World 解算（tick_garrison），
    // 脚本执行层读它只为一件事：把还没到墙边的编队成员往指令格挪。
    std::span<const std::uint8_t> garrison_order() const noexcept {
        return sp(w_->garrison_order_);
    }

    // 数值表（只读）。脚本执行层与 flow field（rts/flow.hpp）按它算代价——
    // 与机制同一份表、同一对函数（combat_math），不另立公式。
    const StatsTable& stats() const noexcept { return w_->stats(); }

    // 动作掩码直通（只读）。脚本执行层与观测打包都要它（掩码本来就是观测的
    // 一部分，契约 §2）——不给这条，脚本就得捧着 `World&`，「脚本一律经
    // `WorldView`」那条纪律就名存实亡。
    std::uint16_t action_mask(UnitId id) const { return w_->action_mask(id); }

    // 守方三资源的存量。攻方没有经济系统，所以它不按侧参数化——
    // 攻方策略读到它是正常的（那是**免费可见**的一部分吗？不是，
    // 但把「攻方能不能看见玩家有多少钱」这个决定塞进一个 getter 是错的位置：
    // 它是观测通道的取舍，归 `rts/obs.hpp`。本类不做过滤，见文件头）。
    std::span<const std::int64_t> stock() const noexcept {
        return std::span<const std::int64_t>(w_->stock_.data(), w_->stock_.size());
    }

private:
    friend class World;

    WorldView(const World& w, Side s) noexcept : w_(&w), side_(s) {}

    template <class T>
    static std::span<const T> sp(const std::vector<T>& v) noexcept {
        return std::span<const T>(v.data(), v.size());
    }

    const World* w_;
    Side side_;
};

}  // namespace rts

#endif  // RTS_WORLD_VIEW_HPP
