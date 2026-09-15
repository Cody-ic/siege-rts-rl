#ifndef GAME_SELECTION_CYCLE_HPP
#define GAME_SELECTION_CYCLE_HPP
#include <optional>
#include <vector>
#include "rts/types.hpp"
namespace game {
// UI state only: candidates arrive in front-to-back draw order.
class SelectionCycle {
public:
    void reset() { hits_.clear(); index_=0; }
    void observe(rts::Vec2 screen,rts::Vec2 world) {
        const auto moved=[](rts::Vec2 a,rts::Vec2 b) {
            return (a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)>25.0f;
        };
        if(moved(screen,screen_) || moved(world,world_)) reset();
    }
    std::optional<rts::GridPos> select(const std::vector<rts::GridPos>& hits,
                                      rts::Vec2 screen,rts::Vec2 world) {
        observe(screen,world);
        if(hits.empty()) {reset();return std::nullopt;}
        index_=hits==hits_?(index_+1)%hits.size():0;
        hits_=hits;screen_=screen;world_=world;
        return hits_[index_];
    }
private:
    std::vector<rts::GridPos> hits_;
    std::size_t index_=0;
    rts::Vec2 screen_{},world_{};
};
}
#endif
