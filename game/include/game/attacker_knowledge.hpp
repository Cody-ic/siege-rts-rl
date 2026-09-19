#ifndef GAME_ATTACKER_KNOWLEDGE_HPP
#define GAME_ATTACKER_KNOWLEDGE_HPP

#include <vector>
#include "rts/flow.hpp"
#include "rts/world_view.hpp"

namespace game {

// Shared observations of ordinary combat squads. Wraith intelligence is
// delivered only through a successful, one-off global snapshot.
class AttackerKnowledge {
public:
    void observe(const rts::WorldView& view, bool global_snapshot = false);
    const std::vector<rts::FlowBuilding>& buildings() const noexcept { return buildings_; }
    bool visible(rts::GridPos p) const noexcept {
        return p.i >= 0 && p.j >= 0 && p.i < width_ && p.j < height_ &&
            visible_[static_cast<std::size_t>(p.j*width_+p.i)] != 0;
    }
private:
    friend struct SnapshotCodec;
    std::vector<rts::FlowBuilding> buildings_;  // Sorted by (row, column); persistent.
    int width_ = 0, height_ = 0;
    std::vector<std::uint8_t> visible_;        // Derived anew at each decision.
};

}  // namespace game
#endif
