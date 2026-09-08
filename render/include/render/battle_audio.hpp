#ifndef RENDER_BATTLE_AUDIO_HPP
#define RENDER_BATTLE_AUDIO_HPP
#include <array>
#include <vector>
#include "render/battle_signals.hpp"
#include "game/iso_projection.hpp"
#include "raylib.h"
namespace render {
// Original, bounded synthesis. The headless trainer never links this module.
std::vector<float> synthesize_cue(BattleCue cue, int sample_rate=24000);
std::vector<float> synthesize_wind(int sample_rate=24000);
class BattleAudio {
public:
    explicit BattleAudio(bool enabled);
    ~BattleAudio();
    BattleAudio(const BattleAudio&)=delete;
    BattleAudio& operator=(const BattleAudio&)=delete;
    void toggle();
    bool muted() const {return muted_;}
    bool ready() const {return ready_;}
    void active(bool enabled);
    void play(const std::vector<BattleSignal>& events,const game::IsoProjection& projection,
              const Camera2D& camera,Vector2 viewport);
private:
    static constexpr std::size_t kCues=static_cast<std::size_t>(BattleCue::Count);
    bool ready_=false,muted_=false,active_=true;
    Sound wind_{};
    std::array<Sound,kCues> sounds_{};
    std::array<double,kCues> next_{};
};
}
#endif
