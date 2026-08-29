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
//     倍率是千分比（量级 10³–10⁴）、倍率个数 ≤ 3（等级 / 克制 / 对结构），
//     乘积上界约 10¹⁴，余量三个数量级。Debug 下有断言
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

// 等级 → 千分比。`1000 + 系数 × (L − 1)`：1 级恒为 1000（不缩放），
// 系数来自 `GlobalStats`（§1.4 未定，所以血量与伤害各有一个、可独立归零）。
inline std::int64_t level_permille(std::int32_t level,
                                   std::int32_t per_level) noexcept {
    assert(level >= 1);
    return kPermilleOne +
           static_cast<std::int64_t>(per_level) * (static_cast<std::int64_t>(level) - 1);
}

}  // namespace rts

#endif  // RTS_COMBAT_MATH_HPP
