#ifndef RENDER_BATTLE_ATMOSPHERE_HPP
#define RENDER_BATTLE_ATMOSPHERE_HPP
#include <cstdint>
#include <vector>
#include "game/iso_projection.hpp"
#include "rts/world_view.hpp"
#include "render/sprite_atlas.hpp"
#include "render/text.hpp"
namespace render {
// 只读渲染状态：不写入 World、不消耗仿真随机数、特效数量有上限。
class BattleAtmosphere {
public:
    void reset();
    void observe(const rts::WorldView& view, rts::Tick tick, int wave);
    void draw_world(const game::IsoProjection& proj, SpriteAtlas& atlas, float zoom) const;
    void draw_screen(Vector2 viewport,const FontSet& font) const;
private:
    struct Building { rts::GridPos pos{}; rts::BldType type{}; std::int64_t hp=0,max_hp=1; bool alive=false; int level=1; bool upgrading=false; bool built=true; };
    struct Burst { rts::GridPos pos; float born; bool collapse; };
    std::vector<Building> previous_;
    std::vector<Burst> bursts_;
    float seconds_=0, wave_since_=0;
    int wave_=0;
};
}
#endif
