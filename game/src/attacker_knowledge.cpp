#include "game/attacker_knowledge.hpp"

#include <algorithm>
#include <cmath>
#include "rts/visibility.hpp"

namespace game {

void AttackerKnowledge::observe(const rts::WorldView& view, bool global_snapshot) {
    width_ = view.width(); height_ = view.height();
    visible_.assign(static_cast<std::size_t>(width_*height_), 0);
    for (std::size_t k = 0; k < view.unit_alive().size(); ++k) {
        if (!view.unit_alive()[k]) continue;
        const auto type = view.unit_type()[k];
        if (rts::side_of(type) != rts::Side::Attacker || !rts::is_combat(type)) continue;
        const auto p = view.unit_pos()[k];
        const auto at = rts::grid_of(p);
        const float radius = view.stats().of(type).vision;
        visible_[static_cast<std::size_t>(at.j*width_+at.i)] = 1;
        for (int y = std::max(0, static_cast<int>(std::floor(p.y-radius)));
             y <= std::min(height_-1, static_cast<int>(std::floor(p.y+radius))); ++y) {
            for (int x = std::max(0, static_cast<int>(std::floor(p.x-radius)));
                 x <= std::min(width_-1, static_cast<int>(std::floor(p.x+radius))); ++x) {
                const auto c = static_cast<std::size_t>(y*width_+x);
                if (visible_[c]) continue;
                const float dx = static_cast<float>(x)+0.5f-p.x;
                const float dy = static_cast<float>(y)+0.5f-p.y;
                const rts::GridPos target{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
                if (dx*dx+dy*dy <= radius*radius &&
                    (rts::is_aerial(type) || rts::line_of_sight(view.terrain(), at, target))) visible_[c] = 1;
            }
        }
    }

    // Observing an empty position removes its old record. Unseen demolition,
    // rebuilding, repair and upgrades leave the previous observation intact.
    if (global_snapshot) buildings_.clear();
    else std::erase_if(buildings_, [&](const auto& b) { return visible(b.pos); });
    for (std::size_t k = 0; k < view.bld_alive().size(); ++k) {
        if (!view.bld_alive()[k]) continue;
        const auto pos = view.bld_pos()[k];
        if (!global_snapshot && !visible(pos)) continue;
        buildings_.push_back({pos, view.bld_type()[k], view.bld_hp()[k],
                              view.bld_level()[k], view.bld_built()[k] != 0, view.now()});
    }
    std::sort(buildings_.begin(), buildings_.end(), [](const auto& a, const auto& b) {
        return a.pos.j != b.pos.j ? a.pos.j < b.pos.j : a.pos.i < b.pos.i;
    });
}

}  // namespace game
