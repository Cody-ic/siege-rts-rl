#include "render/battle_signals.hpp"
#include <cmath>
namespace render {
void BattleSignals::reset() {buildings_.clear();units_.clear();events_.clear();tick_=-1;phase_=rts::WavePhase::Build;}
void BattleSignals::observe(const rts::WorldView& v) {
    if(v.now()==tick_) {events_.clear();return;}
    if(v.now()<tick_) reset();
    events_.clear();
    const bool first=tick_<0;
    const auto emit=[&](BattleCue cue,rts::Vec2 p) {if(events_.size()<32) events_.push_back({cue,p});};
    const auto visible=[&](rts::Vec2 p) {
        const int x=static_cast<int>(std::floor(p.x)),y=static_cast<int>(std::floor(p.y));
        return v.fog().in_bounds(x,y) && v.fog().at(x,y)==rts::Vis::Visible;
    };
    if(!first && phase_==rts::WavePhase::Build && v.phase()!=phase_) emit(BattleCue::Horn,rts::center_of(v.keep_pos()));
    for(std::size_t k=0;k<v.bld_alive().size();++k) {
        const auto type=v.bld_type()[k];const auto p=rts::center_of(v.bld_pos()[k]);
        Sample next{p,static_cast<int>(type),v.bld_cooldown()[k],v.bld_work_left()[k]+v.bld_upgrade_left()[k],
                    v.bld_hp()[k],v.bld_alive()[k]!=0,v.side()==rts::Side::Defender || visible(p)};
        next.windup=v.bld_windup()[k];
        if(!first && k<buildings_.size()) {
            const auto& old=buildings_[k];
            if(old.alive && old.visible && !next.alive && (v.side()==rts::Side::Defender || visible(old.pos)))
                emit(BattleCue::Collapse,old.pos);
            else if(old.alive && next.alive && old.visible && next.visible && old.type==next.type && old.pos.x==p.x && old.pos.y==p.y) {
                    if(next.hp<old.hp) emit(BattleCue::Stone,p);
                    const bool released=(old.windup>0 && next.windup==0) ||
                        (v.stats().of(type).windup_ticks==0 && next.cooldown>old.cooldown);
                    if(released && (type==rts::BldType::Tower || type==rts::BldType::Flak))
                        emit(type==rts::BldType::Flak?BattleCue::Bolt:BattleCue::Bow,p);
                    if(next.work<old.work && old.work>0 && v.now()%12==0) emit(BattleCue::Work,p);
            }
        }
        if(k<buildings_.size()) buildings_[k]=next;else buildings_.push_back(next);
    }
    for(std::size_t k=0;k<v.unit_alive().size();++k) {
        const auto type=v.unit_type()[k];const auto p=v.unit_pos()[k];
        Sample next{p,static_cast<int>(type),v.unit_cooldown()[k],0,v.unit_hp()[k],v.unit_alive()[k]!=0,
                    rts::side_of(type)==v.side() || visible(p)};
        next.windup=v.unit_windup()[k];
        if(!first) {
            const Sample old=k<units_.size()?units_[k]:Sample{};
            if(type==rts::UnitType::Phoenix && next.alive && next.visible && (!old.alive || !old.visible || old.type!=next.type)) emit(BattleCue::Phoenix,p);
            if(old.alive && old.visible && !next.alive && old.type==static_cast<int>(rts::UnitType::Phoenix) && visible(old.pos))
                emit(BattleCue::WingFall,old.pos);
            else if(old.alive && old.visible && next.visible && old.type==next.type) {
                if(next.alive && ((old.windup>0 && next.windup==0) ||
                    (v.stats().of(type).windup_ticks==0 && next.cooldown>old.cooldown)) &&
                    (type==rts::UnitType::Archer || type==rts::UnitType::Shade)) emit(BattleCue::Bow,p);
            }
        }
        if(k<units_.size()) units_[k]=next;else units_.push_back(next);
    }
    tick_=v.now();phase_=v.phase();
}
}
