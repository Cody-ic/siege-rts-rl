#ifndef RENDER_BATTLE_SIGNALS_HPP
#define RENDER_BATTLE_SIGNALS_HPP
#include <vector>
#include "rts/world_view.hpp"
namespace render {
enum class BattleCue { Horn, Bow, Bolt, Stone, Collapse, Phoenix, WingFall, Work, Count };
struct BattleSignal { BattleCue cue; rts::Vec2 pos; };
// A read-only observer shared by audio and its headless verification probe.
// WorldView includes truth: hostile events must additionally pass this side's fog.
class BattleSignals {
public:
    void reset();
    void observe(const rts::WorldView& view);
    const std::vector<BattleSignal>& events() const {return events_;}
private:
    struct Sample {rts::Vec2 pos{};int type=0,cooldown=0,work=0;std::int64_t hp=0;bool alive=false,visible=false;int windup=0;};
    std::vector<Sample> buildings_,units_;
    std::vector<BattleSignal> events_;
    rts::Tick tick_=-1;
    rts::WavePhase phase_=rts::WavePhase::Build;
};
}
#endif
