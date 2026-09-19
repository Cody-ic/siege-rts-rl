#include "game/attacker_macro.hpp"

#include <algorithm>
#include <cmath>

#include "rts/fog.hpp"

namespace game {

namespace {

int cheb(rts::GridPos a, rts::GridPos b) {
    const int dx = a.i > b.i ? a.i - b.i : b.i - a.i;
    const int dy = a.j > b.j ? a.j - b.j : b.j - a.j;
    return dx > dy ? dx : dy;
}

}   // namespace

// ——`WaveCurve` 的两条曲线。唯一实现处（`tools/balance/budget_curves.py`
//   是它的 Python 镜像，改系数两处一起改）。2026-09-05 随结构体从
//   `demo_driver.cpp` 搬来。——

int WaveCurve::slots_at(int wave) const {
    const double s = slots_base + slots_per_wave * static_cast<double>(wave - 1);
    int n = static_cast<int>(s);
    if (slots_cap > 0 && n > slots_cap) n = slots_cap;
    return n < 1 ? 1 : n;
}

double WaveCurve::power_at(int wave) const {
    const double w = static_cast<double>(wave);
    switch (power_form) {
        case PowerForm::Linear:
            return power_base * (1.0 + power_alpha * (w - 1.0));
        case PowerForm::Log:
            return power_base * (1.0 + power_alpha * std::log(w));
        case PowerForm::Saturating:
            // 后期趋平：w 小时近似 base·(1+alpha·w)，w >> half 时趋于常数。
            // 它存在的理由是「无尽模式必败」只要求**攻方不停涨**，不要求
            // 涨得比守方的饱和收入快无穷多——那条曲线的形状是待定的。
            return power_base *
                   (1.0 + power_alpha * w / (1.0 + w / (power_half > 0.0 ? power_half : 1.0)));
        case PowerForm::Power:
        default:
            return power_base * std::pow(w, power_alpha);
    }
}

// ——`AttackerMacro`——

AttackerMacro::AttackerMacro(const MapData& map, const AttackerParams& params)
    : p_(params) {
    keep_ = map.keep();

    // 环半径 = max(|墙格 − keep|)。**与 `DefenderMacro` 和
    // `tools/calibration_runner` 的 `collect_map_info` 同一条判据**——同一件事
    // 只能有一个定义，三处若各写各的，「城区多大」会在三份报告里给出三个数。
    for (const WallSegment& s : map.walls()) {
        ring_r_ = std::max(ring_r_, cheb(s.pos, keep_));
    }

    // 城区内（含环）的所有格，**行主序，顺序即规范**（纪律 2）。
    // (2R+1)² 格：R = 10 时 441 个，一波扫一次可以忽略不计。
    //
    // 为什么不只扫环：塔在环**内侧**一格、防空在离堡垒 4–6 格
    // （`DefenderMacro` 的两组候选位），只扫环一座都看不见。
    city_cells_.reserve(static_cast<std::size_t>((2 * ring_r_ + 1) * (2 * ring_r_ + 1)));
    for (int x = keep_.i - ring_r_; x <= keep_.i + ring_r_; ++x) {
        for (int y = keep_.j - ring_r_; y <= keep_.j + ring_r_; ++y) {
            if (x < 0 || y < 0 || x >= map.width() || y >= map.height()) continue;
            city_cells_.push_back({static_cast<std::int16_t>(x),
                                   static_cast<std::int16_t>(y)});
        }
    }

    // 资源点，**地图文件序**（天然确定，不需要再排一次）。
    resource_cells_.reserve(map.resources().size());
    for (const ResourceNode& r : map.resources()) resource_cells_.push_back(r.pos);
}

AttackerIntel AttackerMacro::read_intel(const rts::WorldView& av, bool fresh) const {
    AttackerIntel out;
    out.fresh = fresh;

    const rts::FogLayer& fog = av.fog();

    // **`FogLayer` 的访问器只有 `assert` 没有边界检查**（`fog.cpp` 的 `idx()`），
    // Release 下越界是野读而不是返回 false。所以每一次都先 `in_bounds()`。
    const auto seen = [&](rts::GridPos c) {
        return fog.in_bounds(c.i, c.j) && fog.at(c.i, c.j) != rts::Vis::Unseen;
    };

    for (const rts::GridPos c : city_cells_) {
        rts::RememberedBld b;
        if (!seen(c)) continue;
        const bool has = fog.remembered_bld(c.i, c.j, &b);
        if (cheb(c, keep_) == ring_r_) {
            // 环上：有墙/门算「还立着」，什么都没有算**缺口**。
            //
            // 判据照 `fog.hpp` 写明的那条（`at() != Unseen && !remembered_bld()`）
            // ——`remembered_bld()` 返回 false 的两种含义必须分开，「从未见过」
            // 不是缺口。上面的 `seen()` 已经把 `Unseen` 滤掉了。
            if (has && (b.type == rts::BldType::Wall || b.type == rts::BldType::Gate)) {
                ++out.walls;
            } else if (!has) {
                ++out.gaps;
            }
        }
    }
    // Defenses can expand beyond that ring. Read all explored fog memory;
    // unseen construction must never leak into attack composition.
    for (int y=0;y<av.height();++y) for (int x=0;x<av.width();++x) {
        if (fog.at(x,y)==rts::Vis::Unseen) continue;
        rts::RememberedBld b;
        if (!fog.remembered_bld(x,y,&b)) continue;
        if (b.type == rts::BldType::Tower) ++out.towers;
        if (b.type == rts::BldType::Flak) ++out.flaks;
    }

    // 采集建筑：只认记忆里**真的看见过**建筑的资源点。没看见过的点即便地图上
    // 有资源，攻方也不该知道守方在那儿铺了矿——那是 god 视角。
    for (const rts::GridPos c : resource_cells_) {
        rts::RememberedBld b;
        if (!fog.in_bounds(c.i, c.j)) continue;
        if (fog.at(c.i, c.j) == rts::Vis::Unseen) continue;
        if (!fog.remembered_bld(c.i, c.j, &b)) continue;
        if (b.type != rts::BldType::Quarry && b.type != rts::BldType::Lumber &&
            b.type != rts::BldType::Mine) {
            continue;
        }
        out.economy.push_back(c);
    }

    return out;
}

WavePlan AttackerMacro::compose(const WaveCurve& c, int wave,
                                const AttackerIntel& in) const {
    const int slots = c.slots_at(wave);

    int shade_pm = c.shade_permille;
    int knight_pm = c.knight_permille;
    int ram_pm = c.ram_permille;

    if (p_.adapt_composition) {
        // 一、塔海 ⇒ 多带攻城锤。攻方对成片防御工事的答案只有两个
        //     （`Ram` 的 AOE 与 `Phoenix` 的点杀），这是其中一个。
        if (p_.tower_ram_per > 0 && in.towers > p_.tower_ram_from) {
            const int steps = (in.towers - p_.tower_ram_from) / p_.tower_ram_per + 1;
            ram_pm += steps * p_.ram_step_permille;
        }
        // 二、**记忆里有缺口 ⇒ 大砍攻城锤**。人从洞里进去比砸墙快得多，
        //     而带着一堆攻城锤去撞一面已经破了的墙，正是 `CLAUDE.md` 头号
        //     评估指标点名的那个反面行为。
        //
        //     顺序是「先加后砍」而不是二选一：塔多**且**有缺口时，缺口那条
        //     应当压过塔海那条——洞已经开了，火力再密也是从洞里灌进去。
        if (in.gaps > 0) {
            ram_pm = ram_pm * (1000 - p_.gap_ram_cut_permille) / 1000;
        }
    }

    shade_pm = std::max(0, shade_pm);
    knight_pm = std::max(0, knight_pm);
    ram_pm = std::max(0, ram_pm);

    // 三种非 `Ghoul` 战斗兵的比例上界。`Ghoul` 是余量，而它是唯一的肉盾——
    // 不留够会让整波在塔的火力网里被点没。超了就等比压回去。
    const int sum_pm = shade_pm + knight_pm + ram_pm;
    if (p_.specials_max_permille > 0 && sum_pm > p_.specials_max_permille) {
        shade_pm = shade_pm * p_.specials_max_permille / sum_pm;
        knight_pm = knight_pm * p_.specials_max_permille / sum_pm;
        ram_pm = ram_pm * p_.specials_max_permille / sum_pm;
    }

    // **`max(1, ...)` 的地板是既有行为，保留。** 它的意思是「一个兵种一旦
    // 过了出场波数就至少来一个」，所以**把千分比调成 0 删不掉任何兵种**——
    // 想让某个兵种消失得走它自己的计数路径（`Phoenix` 就是这么做的）。
    // 这条不改，但依赖它的地方要知道它在。
    const auto share = [&](int permille) { return std::max(1, slots * permille / 1000); };

    WavePlan out;
    out.shades = wave >= c.shade_from_wave ? share(shade_pm) : 0;
    out.knights = wave >= c.knight_from_wave ? share(knight_pm) : 0;
    out.rams = wave >= c.ram_from_wave ? share(ram_pm) : 0;

    // 不死鸟：独立计数路径（不走千分比，所以能真的降到 0）。
    if (wave >= c.phoenix_from_wave) {
        int ph = c.phoenix_base +
                 (c.phoenix_per_waves > 0
                      ? (wave - c.phoenix_from_wave) / c.phoenix_per_waves
                      : 0);
        ph = std::min(ph, c.phoenix_cap);
        // **守方的防空第一次真的买到了东西。** 此前不死鸟恒 1，建不建 `Flak`
        // 对攻方毫无影响 ⇒ 「每座 AA 意味着该位置少一座对地火力」这个机会
        // 成本是白付的。现在记忆里每 `flak_per_phoenix_cut` 座防空少来一只。
        if (p_.adapt_composition && p_.flak_per_phoenix_cut > 0) {
            ph -= in.flaks / p_.flak_per_phoenix_cut;
        }
        out.phoenixes = std::max(0, ph);
    }

    out.wraiths = wave >= c.wraith_from_wave ? 1 : 0;
    out.ghouls = std::max(1, slots - out.shades - out.knights - out.rams -
                                 out.phoenixes - out.wraiths);
    return out;
}

}   // namespace game
