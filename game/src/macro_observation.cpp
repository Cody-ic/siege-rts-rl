#include "game/macro_observation.hpp"
#include <algorithm>
#include <cmath>
#include "game/player_input.hpp"

namespace game {
bool macro_command_legal(const rts::WorldView& v,const rts::Command& c,bool summon_allowed) {
    if(v.side()!=rts::Side::Defender || c.side!=rts::Side::Defender) return false;
    if(c.kind==rts::CommandKind::None) return true;
    if(c.kind==rts::CommandKind::Summon) return summon_allowed && v.phase()==rts::WavePhase::Build;
    if(c.slot==rts::kNoSlot || static_cast<int>(c.slot)>=v.width()*v.height()) return false;
    const rts::GridPos cell{static_cast<std::int16_t>(c.slot%v.width()),static_cast<std::int16_t>(c.slot/v.width())};
    switch(c.kind) {
        case rts::CommandKind::Build: {
            if(c.what>=rts::kBldTypeCount) return false;
            const auto type=static_cast<rts::BldType>(c.what);
            const auto& types=buildable_types();
            return std::find(types.begin(),types.end(),type)!=types.end() &&
                   can_place_hint(v,type,cell) && can_afford_build(v,type);
        }
        case rts::CommandKind::Train:
            return c.what<rts::kUnitTypeCount && rts::side_of(static_cast<rts::UnitType>(c.what))==rts::Side::Defender &&
                   can_train_hint(v,cell) && !train_pop_full(v) &&
                   can_afford_train(v,static_cast<rts::UnitType>(c.what),c.level);
        case rts::CommandKind::Repair:
            for(std::size_t i=0;i<v.bld_alive().size();++i)
                if(v.bld_alive()[i] && v.bld_pos()[i]==cell && v.bld_upgrade_left()[i]>0) return false;
            return can_repair_hint(v,cell) && can_afford_repair(v,cell);
        case rts::CommandKind::Upgrade: return can_upgrade_hint(v,cell) && can_afford_upgrade(v,cell);
        case rts::CommandKind::Cancel: return can_cancel_build_hint(v,cell);
        case rts::CommandKind::Demolish: return can_demolish_hint(v,cell);
        case rts::CommandKind::Clear: return classify_click(v,cell)==ClickTarget::Obstacle;
        case rts::CommandKind::None:
        case rts::CommandKind::Summon:
        case rts::CommandKind::Composition:
        case rts::CommandKind::PickSpawn: return false;
    }
    return false;
}
std::vector<std::string> macro_cell_names() {
    std::vector<std::string> result;
    for(int i=0;i<rts::kUnitTypeCount;++i)
        for(const auto* field:{"count","hp_fraction_sum","level_sum"})
            result.push_back("unit_"+std::string(rts::ident_of(static_cast<rts::UnitType>(i)))+"_"+field);
    for(int i=0;i<rts::kBldTypeCount;++i)
        for(const auto* field:{"count","hp_fraction_sum","level_sum"})
            result.push_back("building_"+std::string(rts::ident_of(static_cast<rts::BldType>(i)))+"_"+field);
    for(const auto* field:{"visible_fraction","explored_fraction","stone_sites","wood_sites","gold_sites"})
        result.emplace_back(field);
    return result;
}
std::vector<std::string> macro_global_names() {
    return {"stone","wood","gold","wave","assault","keep_hp_fraction","keep_level",
            "unit_level_cap","population","population_cap"};
}
MacroObservation pack_macro_observation(const rts::WorldView& v) {
    if(v.side()!=rts::Side::Defender) throw rts::ContractError("Macro observation requires defender view");
    MacroObservation out;
    out.cells.assign(kMacroGrid*kMacroGrid*kMacroChannels,0.f);
    const auto region=[&](int x,int y) {
        return (std::clamp(y*kMacroGrid/v.height(),0,kMacroGrid-1)*kMacroGrid+
                std::clamp(x*kMacroGrid/v.width(),0,kMacroGrid-1))*kMacroChannels;
    };
    constexpr int fog_offset=3*(rts::kUnitTypeCount+rts::kBldTypeCount);
    std::array<int,kMacroGrid*kMacroGrid> area{};
    for(int y=0;y<v.height();++y) for(int x=0;x<v.width();++x) {
        const auto index=region(x,y);
        ++area[static_cast<std::size_t>(index/kMacroChannels)];
        out.cells[static_cast<std::size_t>(index+fog_offset)]+=v.fog().at(x,y)==rts::Vis::Visible?1.f:0.f;
        out.cells[static_cast<std::size_t>(index+fog_offset+1)]+=v.fog().at(x,y)!=rts::Vis::Unseen?1.f:0.f;
    }
    for(std::size_t i=0;i<area.size();++i) if(area[i]) {
        out.cells[i*kMacroChannels+fog_offset]/=static_cast<float>(area[i]);
        out.cells[i*kMacroChannels+fog_offset+1]/=static_cast<float>(area[i]);
    }
    for(std::size_t i=0;i<v.unit_alive().size();++i) {
        if(!v.unit_alive()[i]) continue;
        const auto type=v.unit_type()[i];const auto pos=v.unit_pos()[i];
        const int x=static_cast<int>(std::floor(pos.x)),y=static_cast<int>(std::floor(pos.y));
        if(x<0 || y<0 || x>=v.width() || y>=v.height()) continue;
        if(rts::side_of(type)!=rts::Side::Defender && v.fog().at(x,y)!=rts::Vis::Visible) continue;
        const auto index=static_cast<std::size_t>(region(x,y)+3*static_cast<int>(type));
        out.cells[index]+=1.f/16.f;
        out.cells[index+1]+=static_cast<float>(v.unit_hp()[i])/static_cast<float>(std::max<std::int64_t>(1,v.unit_max_hp()[i]))/16.f;
        out.cells[index+2]+=static_cast<float>(v.unit_level()[i])/32.f;
    }
    for(std::size_t i=0;i<v.bld_alive().size();++i) {
        if(!v.bld_alive()[i]) continue;
        const auto pos=v.bld_pos()[i];const auto type=v.bld_type()[i];
        const float hp=static_cast<float>(v.bld_hp()[i])/static_cast<float>(std::max<std::int64_t>(1,v.bld_max_hp()[i]));
        const auto index=static_cast<std::size_t>(region(pos.i,pos.j)+3*rts::kUnitTypeCount+3*static_cast<int>(type));
        out.cells[index]+=1.f/16.f;out.cells[index+1]+=hp/16.f;
        out.cells[index+2]+=static_cast<float>(v.bld_level()[i])/32.f;
        if(type==rts::BldType::Keep) {out.global[5]=hp;out.global[6]=static_cast<float>(v.bld_level()[i])/32.f;}
    }
    for(const auto& site:v.resources()) {
        int channel=site.kind==rts::Resource::Stone?0:site.kind==rts::Resource::Wood?1:2;
        out.cells[static_cast<std::size_t>(region(site.pos.i,site.pos.j)+fog_offset+2+channel)]+=1.f/16.f;
    }
    out.global[0]=static_cast<float>(v.stock()[static_cast<std::size_t>(rts::Resource::Stone)])/1000.f;
    out.global[1]=static_cast<float>(v.stock()[static_cast<std::size_t>(rts::Resource::Wood)])/1000.f;
    out.global[2]=static_cast<float>(v.stock()[static_cast<std::size_t>(rts::Resource::Gold)])/1000.f;
    out.global[3]=static_cast<float>(v.wave())/70.f;
    out.global[4]=v.phase()==rts::WavePhase::Assault?1.f:0.f;
    out.global[7]=static_cast<float>(v.unit_level_cap())/32.f;
    out.global[8]=static_cast<float>(v.defender_pop())/32.f;
    out.global[9]=static_cast<float>(v.defender_pop_cap())/32.f;
    return out;
}
}
