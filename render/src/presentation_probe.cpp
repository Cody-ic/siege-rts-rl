// No window/audio device required: presentation must leave simulation untouched.
#include "render/battle_audio.hpp"
#include "render/battle_atmosphere.hpp"
#include "render/scene_renderer.hpp"
#include "game/stats_loader.hpp"
#include "rts/utf8_path.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
namespace {
void check(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
bool has(const render::BattleSignals& s,render::BattleCue cue) {
    return std::any_of(s.events().begin(),s.events().end(),[&](const auto& e){return e.cue==cue;});
}
rts::WorldInit fixture(const rts::StatsTable& stats) {
    rts::WorldInit init;init.width=32;init.height=32;init.terrain.assign(32*32,rts::Terrain::Plain);
    init.keep={3,3};init.stats=stats;
    init.buildings={{rts::BldType::Keep,{3,3},10000,10000},{rts::BldType::Wall,{5,3},300,300},
                    {rts::BldType::Tower,{3,4},10000,10000},{rts::BldType::Flak,{4,4},10000,10000}};
    return init;
}
}
int main(int argc,char** argv) {
    if(argc<2 || argc>3) return 2;
    try {
        const auto init=fixture(game::StatsLoader::from_file(argv[1]));
        rts::World plain(init),observed(init);
        for(auto* world:{&plain,&observed}) {
            world->spawn_unit(rts::UnitType::Ram,{6.5f,3.5f},1,100000,100000);
            world->spawn_unit(rts::UnitType::Phoenix,{4.5f,1.5f},1,100000,100000);
            world->begin_assault();
        }
        render::BattleSignals signals;render::BattleAtmosphere atmosphere;
        signals.observe(observed.view(rts::Side::Defender));
        check(signals.events().empty(),"initial load replayed historical sounds");
        bool collapse=false,stone=false,shot=false;
        int first_bolt=-1;
        for(int tick=0;tick<240;++tick) {
            for(auto* world:{&plain,&observed}) {
                std::vector<rts::UnitId> ids;world->enumerate_units(rts::Side::Attacker,ids);
                std::vector<rts::UnitAction> actions;
                for(auto id:ids) actions.push_back(world->unit_type(id)==rts::UnitType::Ram?rts::UnitAction::AtkWall:rts::UnitAction::AtkBld);
                world->submit_actions(rts::Side::Attacker,actions.data(),actions.size());world->advance(1);
            }
            signals.observe(observed.view(rts::Side::Defender));
            atmosphere.observe(observed.view(rts::Side::Defender),observed.now(),observed.wave());
            collapse|=has(signals,render::BattleCue::Collapse);stone|=has(signals,render::BattleCue::Stone);
            shot|=has(signals,render::BattleCue::Bolt);
            if(first_bolt<0 && has(signals,render::BattleCue::Bolt)) first_bolt=observed.now();
            check(plain.state_hash()==observed.state_hash(),"presentation changed simulation/RNG/fog");
            signals.observe(observed.view(rts::Side::Defender));
            check(signals.events().empty(),"same tick retriggered audio");
        }
        std::printf("combat cues collapse=%d stone=%d bolt=%d wall_hp=%lld\n",collapse,stone,shot,
            static_cast<long long>(observed.view(rts::Side::Defender).bld_hp()[1]));
        check(collapse && stone && shot,"real combat did not produce collapse, damage and bolt cues");
        check(first_bolt==init.stats.of(rts::BldType::Flak).windup_ticks+1,"bolt sounded before actual release");
        rts::World hidden(init);hidden.advance(1);signals.reset();signals.observe(hidden.view(rts::Side::Defender));
        const auto hidden_bird=hidden.spawn_unit(rts::UnitType::Phoenix,{29.5f,29.5f},1,100000,100000);
        hidden.advance(1);signals.observe(hidden.view(rts::Side::Defender));
        check(!has(signals,render::BattleCue::Phoenix),"fog leaked Phoenix arrival");
        hidden.kill_unit(hidden_bird);hidden.advance(1);signals.observe(hidden.view(rts::Side::Defender));
        check(!has(signals,render::BattleCue::WingFall),"fog leaked Phoenix death");
        const auto visible_bird=hidden.spawn_unit(rts::UnitType::Phoenix,{4.5f,1.5f},1,100000,100000);
        hidden.advance(1);signals.observe(hidden.view(rts::Side::Defender));
        check(has(signals,render::BattleCue::Phoenix),"visible Phoenix arrival missing");
        hidden.kill_unit(visible_bird);hidden.advance(1);signals.observe(hidden.view(rts::Side::Defender));
        check(has(signals,render::BattleCue::WingFall),"cleared dead slot lost visible Phoenix death");
        signals.observe(plain.view(rts::Side::Defender));
        rts::World fresh(init);signals.observe(fresh.view(rts::Side::Defender));
        check(signals.events().empty(),"timeline reset replayed stale sounds");
        fresh.begin_assault();fresh.advance(1);signals.observe(fresh.view(rts::Side::Defender));
        check(has(signals,render::BattleCue::Horn),"assault transition missing");
        render::Sprite sprite; sprite.ground_anchor={150,600};
        for(float scale:{1.0f,render::SceneRenderer::presentation_scale("Keep",true)}) {
            const auto pixel=render::SceneRenderer::sprite_pixel(sprite,{1000,2000},{1000+25*scale,2000-350*scale},scale);
            check(std::abs(pixel.x-175)<0.01f && std::abs(pixel.y-250)<0.01f,"scaled keep picking drifted");
        }
        std::vector<float> preview;
        for(int i=0;i<static_cast<int>(render::BattleCue::Count);++i) {
            const auto pcm=render::synthesize_cue(static_cast<render::BattleCue>(i));
            float energy=0;
            for(float sample:pcm) {check(std::isfinite(sample)&&std::abs(sample)<=0.85f,"invalid audio sample");energy+=sample*sample;}
            check(energy>0.01f,"silent cue");
            preview.insert(preview.end(),pcm.begin(),pcm.end());preview.insert(preview.end(),12000,0.0f);
        }
        const auto wind=render::synthesize_wind();
        check(!wind.empty()&&wind.front()==0&&std::abs(wind.back())<0.001f,"wind has discontinuous endpoints");
        if(argc==3) {
            Wave wave{static_cast<unsigned int>(preview.size()),24000,32,1,preview.data()};
            check(ExportWave(wave,argv[2]),"audio preview export failed");
        }
        std::puts("PASS: 240 matching world hashes; real combat cues; fog; reset; grounded picking; 8 synthesized cues.");
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
}
