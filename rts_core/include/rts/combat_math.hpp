// 整数战斗解算的唯一乘除入口（决定 ⑫，`rts_core 接口契约.md` §3.4）。
//
// 四个子条全部落在这一个函数上：
//
//   * **截断只能发生一次，且由这里强制。** 倍率作为列表传进来，先把分子分母
//     各自乘完、最后做唯一一次除法。链式调用会静默少算：
//     `5×1500×1500/1e6 = 11`，而 `(5×1500/1000=7)×1500/1000 = 10`
//   * **四舍五入，不向下截断**：向下截断每次少算至多 1 点，在低伤害区是
//     20–30% 的相对偏差，全局稳定偏向防守方
//   * **结果必须为正**（钳到 1）：0 伤害凭空造出一种免疫，违反「克制的本质是
//     资源效率，不是二元胜负」。要真正禁止某类打击，用结构（`can_engage` /
//     `can_break_structure`），不要指望这里算出 0
//   * **中间量比存储宽**：分子分母都在 64 位里乘。调用方要保证
//     `base × ∏permille` 不溢出 int64——base 是表里的基础伤害（量级 10²）、
//     倍率是千分比（量级 10³–10⁴）、倍率个数 ≤ 4（等级 / 冲锋 / 高度或反冲锋 /
//     对结构），乘积上界约 10¹⁷，余量一个多数量级。Debug 下有断言
//
// 放在独立的头而不是 `world.hpp`：平衡工具（成本产出矩阵）与测试都要单独调它。

#ifndef RTS_COMBAT_MATH_HPP
#define RTS_COMBAT_MATH_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace rts {

inline constexpr std::int64_t kPermilleOne = 1000;

// `base × (p₀/1000) × (p₁/1000) × …`，一次除法、四舍五入、钳到 >= 1。
//
// 倍率取 int64 而不是 int32：等级缩放的千分比是 `1000 + 系数 × (L−1)`，
// 等级无上限（守方升级轴、攻方兵力预算都无界），int32 在离谱但合法的等级上
// 会溢出——「接口的承诺不得依赖任何待定数值」（§4.1）。
inline std::int64_t apply_permille(std::int64_t base, const std::int64_t* permille,
                                   std::size_t count) noexcept {
    assert(base >= 0);
    std::int64_t num = base;
    std::int64_t den = 1;
    for (std::size_t k = 0; k < count; ++k) {
        assert(permille[k] >= 0);
        // 溢出断言：乘之前查一次。Release 下不查——上面文件头给了量级论证，
        // 这里防的是「有人传了个绝对值离谱的倍率」这种编程错误。
        assert(permille[k] == 0 || num <= INT64_MAX / (permille[k] > 0 ? permille[k] : 1));
        num *= permille[k];
        den *= kPermilleOne;
    }
    const std::int64_t v = (num + den / 2) / den;
    return v < 1 ? 1 : v;
}

inline std::int64_t apply_permille(std::int64_t base,
                                   std::initializer_list<std::int64_t> permille) noexcept {
    return apply_permille(base, permille.begin(), permille.size());
}

// 整数平方根（floor），牛顿法。**刻意不用 `std::sqrt`**：浮点开方的结果在
// 两套工具链上可以差一个最低位，而这个函数的返回值直接进 `state_hash` 与回放
// ——「同平台同编译器可复现」那条底线管不住一个会在 Windows/Linux 之间飘的
// 中间量（CLAUDE.md「确定性要求」）。整数牛顿法逐位确定。
inline std::int64_t isqrt_permille(std::int64_t n) noexcept {
    assert(n >= 0);
    if (n <= 0) return 0;
    std::int64_t x = n;
    std::int64_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// 等级 → 千分比。**`√(1 + 系数 × (L − 1))`，血量与伤害各开一份平方根。**
//
// 1 级恒为 1000（不缩放）：`isqrt(1000 × 1000) = 1000`，精确、不靠舍入。
//
// ## 为什么是平方根，而不是「只涨伤害」
//
// 这一条 #96 定过一次（只涨伤害、`hp` 系数归零），随后被一次测量重开
// （`CLAUDE.md` §1.4）。原论证的隐含假设是「每属性线性增长」，写成
// `血 = B(L)^p` / `伤 = B(L)^q`（`B(L) = 1 + k(L−1)`）之后，三条判据各吃一个
// **互相正交**的组合：
//
//   * 克制比例在等级差下的衰减 → `1/(血×伤) = B^-(p+q)`，**只看 p+q**
//   * TTK 稳不稳（短 episode 那条训练前提） → `血/伤 = B^(p−q)`，**只看 p−q**
//   * 破墙时间（「结构破坏规则」那条区间不变量） → 也只看 p−q
//
// p、q 限在 {0,1} 时 p+q 与 p−q 被锁死在一起，于是「保住克制关系」与
// 「TTK 别乱跑」看起来必须二选一。放开之后它们不冲突：**p = q = 0.5 与
// 「只涨伤害」的战力曲线逐点相同**（k = 220 下 1.22 / 1.44 / 1.88 / 2.98 /
// 5.18 / 9.58），而 TTK 从 ×0.19 回到 ×1.00、破墙时间从 ×0.20 回到 ×1.03。
// **没有一项更差**，所以这不是权衡、是纯粹的改进。
//
// 落地上因此拆成两半：**`p − q = 0` 是结构约束**（由 TTK 与破墙两条不变量定，
// 与标定无关），它由「两个系数必须相等」在 `StatsLoader` 里强制；
// **`p + q` 是数值**（管「一级值多少战力」），它就是这里的 `per_level`，
// 要与波次预算、堡垒等级两条曲线一起标。
inline std::int64_t level_permille(std::int32_t level,
                                   std::int32_t per_level) noexcept {
    assert(level >= 1);
    const std::int64_t linear =
        kPermilleOne +
        static_cast<std::int64_t>(per_level) * (static_cast<std::int64_t>(level) - 1);
    // 千分比下的开方：`√(x/1000) × 1000 = √(1000·x)`。先乘再开，不先除——
    // 先除会把 1440‰ 这类值截成 1，整条曲线塌掉。
    return isqrt_permille(kPermilleOne * linear);
}

// 冲锋动量 → 千分比。`1000 + 每格加成 × min(动量, 封顶)`，1000 = 没有动量。
// **机制等级无关**（CLAUDE.md：所有机制性克制必须等级无关）——等级只经
// 伤害基数进来，这里只看跑了多远。封顶 <= 0 表示冲锋系统整个关着（诚实默认）。
inline std::int64_t charge_permille(float run_cells, float max_cells,
                                    std::int32_t per_cell_permille) noexcept {
    if (max_cells <= 0.0f || per_cell_permille <= 0 || run_cells <= 0.0f) {
        return kPermilleOne;
    }
    const float m = run_cells < max_cells ? run_cells : max_cells;
    return kPermilleOne + static_cast<std::int64_t>(
                              static_cast<float>(per_cell_permille) * m + 0.5f);
}

// 反冲锋（枪阵）→ 千分比。克制幅度随**目标的动量**线性放大：满动量给全额
// `full_permille`，没有动量恒为 1000（不成立）。**同一份动量既给冲锋加成、
// 也给顶着它的枪阵加成**——「开阔地克、巷战被反克」因此不需要读地形：
// 巷战里骑兵攒不出动量，枪阵的克制也就自动消失，剩下的是普通近战对拼。
inline std::int64_t anti_charge_permille(float target_run_cells, float max_cells,
                                         std::int32_t full_permille) noexcept {
    if (max_cells <= 0.0f || target_run_cells <= 0.0f ||
        full_permille <= kPermilleOne) {
        return kPermilleOne;
    }
    const float m =
        target_run_cells < max_cells ? target_run_cells : max_cells;
    return kPermilleOne +
           static_cast<std::int64_t>(
               static_cast<float>(full_permille - kPermilleOne) * (m / max_cells) +
               0.5f);
}

}  // namespace rts

#endif  // RTS_COMBAT_MATH_HPP
