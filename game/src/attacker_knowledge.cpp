#include "game/attacker_knowledge.hpp"

#include <algorithm>
#include <cmath>
#include "rts/visibility.hpp"

namespace game {

namespace {

// A low, uniform prior keeps an unseen corridor usable while avoiding the
// unsafe assumption that every unseen cell is empty. It is a routing hint, not
// a hidden building or a guaranteed attack.
constexpr float kUnknownDamagePerTick = 0.05f;
constexpr float kFeedbackDecay = 0.86f;
constexpr float kFeedbackBase = 0.025f;
constexpr float kFeedbackPerDamage = 0.0015f;
constexpr float kFeedbackCap = 0.30f;
constexpr int kFeedbackRadius = 2;

}  // namespace

void AttackerKnowledge::observe(const rts::WorldView& view, bool global_snapshot) {
    width_ = view.width(); height_ = view.height();
    full_snapshot_ = global_snapshot;
    const auto n = static_cast<std::size_t>(width_ * height_);
    visible_.assign(n, 0);
    if (feedback_.size() != n) feedback_.assign(n, 0.0f);
    else for (auto& value : feedback_) value *= kFeedbackDecay;
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
    rebuild_risks();
}

void AttackerKnowledge::report_unexplained_damage(rts::GridPos pos, float damage) {
    if (pos.i < 0 || pos.j < 0 || pos.i >= width_ || pos.j >= height_ ||
        !std::isfinite(damage) || damage <= 0.0f) return;
    const float added = std::min(kFeedbackCap, kFeedbackBase + damage * kFeedbackPerDamage);
    for (int y = std::max(0, static_cast<int>(pos.j) - kFeedbackRadius);
         y <= std::min(height_ - 1, static_cast<int>(pos.j) + kFeedbackRadius); ++y) {
        for (int x = std::max(0, static_cast<int>(pos.i) - kFeedbackRadius);
             x <= std::min(width_ - 1, static_cast<int>(pos.i) + kFeedbackRadius); ++x) {
            const int dx = x - pos.i, dy = y - pos.j;
            const int d2 = dx * dx + dy * dy;
            if (d2 > kFeedbackRadius * kFeedbackRadius) continue;
            const float attenuation = d2 == 0 ? 1.0f : (d2 == 1 ? 0.65f : 0.35f);
            auto& value = feedback_[static_cast<std::size_t>(y * width_ + x)];
            value = std::min(kFeedbackCap, value + added * attenuation);
        }
    }
    rebuild_risks();
}

void AttackerKnowledge::rebuild_risks() {
    risks_.clear();
    if (width_ <= 0 || height_ <= 0) return;
    const auto n = static_cast<std::size_t>(width_ * height_);
    std::vector<std::uint8_t> known_building(n, 0);
    for (const auto& b : buildings_) {
        if (b.pos.i >= 0 && b.pos.j >= 0 && b.pos.i < width_ && b.pos.j < height_)
            known_building[static_cast<std::size_t>(b.pos.j * width_ + b.pos.i)] = 1;
    }
    for (std::size_t c = 0; c < n; ++c) {
        float value = feedback_[c];
        if (!full_snapshot_ && !visible_[c] && !known_building[c]) value += kUnknownDamagePerTick;
        if (value <= 0.0f) continue;
        const int x = static_cast<int>(c % static_cast<std::size_t>(width_));
        const int y = static_cast<int>(c / static_cast<std::size_t>(width_));
        risks_.push_back({{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)}, value});
    }
}

}  // namespace game
