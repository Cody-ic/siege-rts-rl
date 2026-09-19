#pragma once
#include "game/demo_driver.hpp"
#include <stdexcept>
#include <string>
// Versioned, field-based snapshots. Never dump object memory or pointers.
#include <nlohmann/json.hpp>
#include <type_traits>
#include <limits>
#include <source_location>
#include <set>
namespace game {
struct SnapshotCodec {
    using Json=nlohmann::json;
    static void check(bool ok,const std::source_location where=std::source_location::current()) {if(!ok) throw std::runtime_error("invalid battle snapshot at field line "+std::to_string(where.line()));}
    template<class T,class F> static void fields(T& v,F&& f) {
        using U=std::remove_cvref_t<T>;
        if constexpr(std::is_same_v<U,rts::World>) {
            f("tick_",v.tick_);
            f("wave_",v.wave_);
            f("phase_",v.phase_);
            f("nominal_level_",v.nominal_level_);
            f("rng_",v.rng_);
            f("developer_",v.developer_);
            f("pop_cap_base_",v.pop_cap_base_);
            f("pop_cap_per_keep_level_",v.pop_cap_per_keep_level_);
            f("unit_pool_",v.unit_pool_);
            f("bld_pool_",v.bld_pool_);
            f("obstacle_pool_",v.obstacle_pool_);
            f("u_type_",v.u_type_);
            f("u_level_",v.u_level_);
            f("u_hp_",v.u_hp_);
            f("u_max_hp_",v.u_max_hp_);
            f("u_pos_",v.u_pos_);
            f("u_squad_",v.u_squad_);
            f("u_windup_",v.u_windup_);
            f("u_action_",v.u_action_);
            f("u_garrison_",v.u_garrison_);
            f("u_garrison_target_",v.u_garrison_target_);
            f("u_mount_",v.u_mount_);
            f("u_charge_",v.u_charge_);
            f("u_cd_",v.u_cd_);
            f("u_tgt_kind_",v.u_tgt_kind_);
            f("u_tgt_raw_",v.u_tgt_raw_);
            f("u_aim_",v.u_aim_);
            f("b_type_",v.b_type_);
            f("b_pos_",v.b_pos_);
            f("b_hp_",v.b_hp_);
            f("b_max_hp_",v.b_max_hp_);
            f("b_work_",v.b_work_);
            f("b_cd_",v.b_cd_);
            f("b_windup_",v.b_windup_);
            f("b_tgt_raw_",v.b_tgt_raw_);
            f("b_aim_",v.b_aim_);
            f("b_built_",v.b_built_);
            f("b_train_type_",v.b_train_type_);
            f("b_train_left_",v.b_train_left_);
            f("b_train_level_",v.b_train_level_);
            f("b_level_",v.b_level_);
            f("b_upgrade_left_",v.b_upgrade_left_);
            f("o_type_",v.o_type_);
            f("o_pos_",v.o_pos_);
            f("o_hp_",v.o_hp_);
            f("o_max_hp_",v.o_max_hp_);
            f("o_clear_ordered_",v.o_clear_ordered_);
            f("p_pos_",v.p_pos_);
            f("p_origin_",v.p_origin_);
            f("p_aim_",v.p_aim_);
            f("p_speed_",v.p_speed_);
            f("p_kind_",v.p_kind_);
            f("p_raw_",v.p_raw_);
            f("p_dmg_",v.p_dmg_);
            f("p_lvl_pm_",v.p_lvl_pm_);
            f("p_from_high_",v.p_from_high_);
            f("p_aoe_",v.p_aoe_);
            f("p_side_",v.p_side_);
            f("p_src_bld_",v.p_src_bld_);
            f("stock_",v.stock_);
            f("composition_",v.composition_);
            f("spawn_chosen_",v.spawn_chosen_);
            f("cmd_queue_",v.cmd_queue_);
            f("fog_",v.fog_);
            f("bld_at_",v.bld_at_);
            f("obstacle_at_",v.obstacle_at_);
        }
        else if constexpr(std::is_same_v<U,rts::FogLayer>) {
            f("width_",v.width_);
            f("height_",v.height_);
            f("vis_",v.vis_);
            f("last_seen_",v.last_seen_);
            f("bld_",v.bld_);
            f("bld_hp_permille_",v.bld_hp_permille_);
            f("has_bld_",v.has_bld_);
        }
        else if constexpr(std::is_same_v<U,DemoBattle>) {
            f("phoenix_roster_",v.phoenix_roster_);
            f("phoenix_respawn_",v.phoenix_respawn_);
            f("phoenix_id_of_",v.phoenix_id_of_);
            f("next_phoenix_id_",v.next_phoenix_id_);
            f("white_feather_",v.white_feather_);
            f("w_",v.w_);
            f("script_",v.script_);
            f("macro_",v.macro_);
            f("timing_",v.timing_);
            f("curve_",v.curve_);
            f("setup_",v.setup_);
            f("build_left_",v.build_left_);
            f("assault_ticks_",v.assault_ticks_);
            f("build_start_",v.build_start_);
            f("wave_scouted_",v.wave_scouted_);
            f("attacker_knowledge_",v.attacker_knowledge_);
            f("recon_observer_",v.recon_observer_);
            f("recon_started_",v.recon_started_);
            f("formation_wait_since_",v.formation_wait_since_);
            f("formation_last_hp_",v.formation_last_hp_);
            f("phoenix_empty_return_",v.phoenix_empty_return_);
            f("prev_wave_scouted_",v.prev_wave_scouted_);
            f("squad_of_",v.squad_of_);
            f("squad_goal_",v.squad_goal_);
            f("keep_goals_",v.keep_goals_);
            f("econ_goals_",v.econ_goals_);
            f("econ_squads_",v.econ_squads_);
            f("wave_intel_",v.wave_intel_);
            f("recon_intel_",v.recon_intel_);
            f("wave_plan_",v.wave_plan_);
            f("baseline_plan_",v.baseline_plan_);
            f("recon_rng_",v.recon_rng_);
            f("scout_outcome_",v.scout_outcome_);
            f("scout_report_",v.scout_report_);
            f("scout_rolled_",v.scout_rolled_);
            f("scout_seen_",v.scout_seen_);
            f("scout_report_tick_",v.scout_report_tick_);
            f("defeated_",v.defeated_);
            f("since_decision_",v.since_decision_);
            if(v.defender_policy_) f("defender_rng_",v.defender_rng_);
        }
        else if constexpr(std::is_same_v<U,DemoBattle::PhoenixRecord>) {
            f("id",v.id);f("waves_alive",v.waves_alive);
        }
        else if constexpr(std::is_same_v<U,AttackerKnowledge>) {
            f("buildings",v.buildings_);
        }
        else if constexpr(std::is_same_v<U,rts::FlowBuilding>) {
            f("pos",v.pos);f("type",v.type);f("hp",v.hp);f("level",v.level);
            f("built",v.built);f("observed_at",v.observed_at);
        }
        else if constexpr(std::is_same_v<U,std::pair<int,int>>) {
            f("first",v.first);f("second",v.second);
        }
        else if constexpr(std::is_same_v<U,DefenderScript>) {
            f("p_",v.p_);
            f("rng_",v.rng_);
            f("threat_streak_",v.threat_streak_);
            f("manual_order_",v.manual_order_);
            f("patrol_goal_",v.patrol_goal_);
            f("scout_navigation_",v.scout_navigation_);
        }
        else if constexpr(std::is_same_v<U,rts::Vec2>) {
            f("x",v.x);
            f("y",v.y);
        }
        else if constexpr(std::is_same_v<U,rts::GridPos>) {
            f("i",v.i);
            f("j",v.j);
        }
        else if constexpr(std::is_same_v<U,rts::Command>) {
            f("slot",v.slot);
            f("kind",v.kind);
            f("side",v.side);
            f("what",v.what);
            f("level",v.level);
        }
        else if constexpr(std::is_same_v<U,ManualOrder>) {
            f("active",v.active);
            f("target",v.target);
            f("garrison",v.garrison);
            f("generation",v.generation);
            f("forced",v.forced);
            f("building",v.building);
            f("upgrade",v.upgrade);
        }
        else if constexpr(std::is_same_v<U,PatrolGoal>) {
            f("active",v.active);
            f("target",v.target);
            f("generation",v.generation);
        }
        else if constexpr(std::is_same_v<U,ScoutNavigation>) {
            f("active",v.active);
            f("cell",v.cell);
            f("target",v.target);
            f("generation",v.generation);
        }
        else if constexpr(std::is_same_v<U,AttackerIntel>) {
            f("fresh",v.fresh);
            f("towers",v.towers);
            f("flaks",v.flaks);
            f("walls",v.walls);
            f("gaps",v.gaps);
            f("economy",v.economy);
        }
        else if constexpr(std::is_same_v<U,ScriptParams>) {
            f("kite_trigger_cells",v.kite_trigger_cells);
            f("kite_permille",v.kite_permille);
            f("reaction_decisions",v.reaction_decisions);
            f("avoid_knight_cells",v.avoid_knight_cells);
            f("arrive_cells",v.arrive_cells);
            f("spear_engage_cells",v.spear_engage_cells);
        }
        else if constexpr(std::is_same_v<U,WaveTiming>) {
            f("build_ticks",v.build_ticks);
            f("first_build_ticks",v.first_build_ticks);
            f("assault_max_ticks",v.assault_max_ticks);
        }
        else if constexpr(std::is_same_v<U,WaveCurve>) {
            f("slots_base",v.slots_base);
            f("slots_per_wave",v.slots_per_wave);
            f("slots_cap",v.slots_cap);
            f("power_form",v.power_form);
            f("power_base",v.power_base);
            f("power_alpha",v.power_alpha);
            f("power_half",v.power_half);
            f("shade_permille",v.shade_permille);
            f("knight_permille",v.knight_permille);
            f("ram_permille",v.ram_permille);
            f("shade_from_wave",v.shade_from_wave);
            f("knight_from_wave",v.knight_from_wave);
            f("ram_from_wave",v.ram_from_wave);
            f("phoenix_from_wave",v.phoenix_from_wave);
            f("wraith_from_wave",v.wraith_from_wave);
            f("phoenix_base",v.phoenix_base);
            f("phoenix_per_waves",v.phoenix_per_waves);
            f("phoenix_cap",v.phoenix_cap);
            f("phoenix_withdraw_hp_permille",v.phoenix_withdraw_hp_permille);
            f("phoenix_respawn_waves",v.phoenix_respawn_waves);
            f("main_permille",v.main_permille);
            f("econ_raid_permille",v.econ_raid_permille);
        }
        else if constexpr(std::is_same_v<U,DefenderSetup>) {
            f("pop_cap_base",v.pop_cap_base);
            f("pop_cap_per_keep_level",v.pop_cap_per_keep_level);
            f("scout_death_permille",v.scout_death_permille);
            f("scout_arrive_cells",v.scout_arrive_cells);
        }
        else if constexpr(std::is_same_v<U,WavePlan>) {
            f("ghouls",v.ghouls);
            f("shades",v.shades);
            f("knights",v.knights);
            f("rams",v.rams);
            f("phoenixes",v.phoenixes);
            f("wraiths",v.wraiths);
        }
        else if constexpr(std::is_same_v<U,SightedType>) {
            f("type",v.type);
            f("count",v.count);
        }
        else if constexpr(std::is_same_v<U,AttackerMacro>) {
            f("params",v.p_);f("keep",v.keep_);f("radius",v.ring_r_);f("city",v.city_cells_);f("resources",v.resource_cells_);
        }
        else if constexpr(std::is_same_v<U,AttackerParams>) {
            f("adapt",v.adapt_composition);f("tower_from",v.tower_ram_from);f("tower_per",v.tower_ram_per);
            f("ram_step",v.ram_step_permille);f("gap_cut",v.gap_ram_cut_permille);f("specials_max",v.specials_max_permille);f("flak_cut",v.flak_per_phoenix_cut);
        }
        else if constexpr(requires {v.generation_;v.alive_;v.free_;}) {f("generation",v.generation_);f("alive",v.alive_);f("free",v.free_);}
        else static_assert(sizeof(T)==0,"snapshot type has no field mapping");
    }
    template<class T> static Json encode(const T& v) {
        if constexpr(std::is_enum_v<T>) return static_cast<std::underlying_type_t<T>>(v);
        else if constexpr(std::is_arithmetic_v<T>) return v;
        else if constexpr(std::is_same_v<T,rts::UnitId>) return v.raw();
        else if constexpr(std::is_same_v<T,rts::Rng>) return encode(v.state());
        else if constexpr(requires {v.begin();v.end();}) {Json j=Json::array();for(const auto& e:v) j.push_back(encode(e));return j;}
        else {Json j=Json::object();fields(v,[&](const char* name,const auto& x){j[name]=encode(x);});return j;}
    }
    template<class T> static void decode(const Json& j,T& v) {
        if constexpr(std::is_enum_v<T>) {std::underlying_type_t<T> n{};decode(j,n);v=static_cast<T>(n);}
        else if constexpr(std::is_same_v<T,bool>) {check(j.is_boolean());v=j.get<bool>();}
        else if constexpr(std::is_arithmetic_v<T>) {
            check(j.is_number());const auto n=j.get<double>();
            check(n>=static_cast<double>(std::numeric_limits<T>::lowest()) && n<=static_cast<double>(std::numeric_limits<T>::max()));v=j.get<T>();
        }
        else if constexpr(std::is_same_v<T,rts::UnitId>) {std::uint32_t raw{};decode(j,raw);if(raw==rts::UnitId::kInvalidRaw) v={};else {check((raw>>16)<=rts::UnitId::kMaxIndex);v=rts::UnitId::make(static_cast<std::uint16_t>(raw>>16),static_cast<std::uint16_t>(raw&0xffffu));}}
        else if constexpr(std::is_same_v<T,rts::Rng>) {rts::Rng::State st{};decode(j,st);check(st[0]||st[1]||st[2]||st[3]);v.set_state(st);}
        else if constexpr(requires {v.begin();v.end();}) {
            check(j.is_array() && j.size()<=2000000);
            if constexpr(requires {v.resize(j.size());}) v.resize(j.size());else check(j.size()==v.size());
            std::size_t i=0;for(auto& e:v) decode(j.at(i++),e);
        } else {
            check(j.is_object());
            fields(v,[&](const char* name,auto& x){
                // Older snapshots contain only ordinary manual orders.
                if constexpr(std::is_same_v<T,ManualOrder>) {
                    if (!j.contains("forced") && (std::string_view(name)=="forced" ||
                        std::string_view(name)=="building" || std::string_view(name)=="upgrade")) { x={}; return; }
                }
                if constexpr(std::is_same_v<T,DemoBattle>) {
                    if(std::string_view(name)=="white_feather_" && !j.contains(name)) {v.white_feather_=false;return;}
                }
                decode(j.at(name),x);
            });
        }
    }
    template<class P> static void validate_pool(const P& p) {
        check(p.generation_.size()==p.alive_.size() && p.alive_.size()<65535);
        std::vector<bool> seen(p.alive_.size(),false);
        for(const auto k:p.free_) {check(k<p.alive_.size() && !p.alive_[k] && !seen[k]);seen[k]=true;}
        for(std::size_t k=0;k<p.alive_.size();++k) check(p.alive_[k]<=1 && (p.alive_[k]!=0 || seen[k]));
    }
    static void validate(const DemoBattle& b) {
        const auto& w=b.w_;validate_pool(w.unit_pool_);validate_pool(w.bld_pool_);validate_pool(w.obstacle_pool_);
        check(b.next_phoenix_id_>=0 && b.curve_.phoenix_respawn_waves>=0);
        check(b.curve_.phoenix_withdraw_hp_permille>=0 && b.curve_.phoenix_withdraw_hp_permille<=1000);
        std::set<int> reserved, assigned;
        for(const auto& r:b.phoenix_roster_) {
            check(r.id>=0 && r.id<b.next_phoenix_id_ && r.waves_alive>=0);
            check(reserved.insert(r.id).second);
        }
        for(const auto& [id,left]:b.phoenix_respawn_) {
            check(id>=0 && id<b.next_phoenix_id_ && left>=0 && left<=b.curve_.phoenix_respawn_waves);
            check(reserved.insert(id).second);
        }
        for(const int id:b.phoenix_id_of_) {
            check(id>=-1 && id<b.next_phoenix_id_);
            if(id>=0) check(assigned.insert(id).second);
        }
        const auto n=static_cast<std::size_t>(w.width()*w.height());
        check(b.attacker_knowledge_.buildings_.size()<=n);
        int previous_cell=-1;
        for(const auto& memory:b.attacker_knowledge_.buildings_) {
            check(w.terrain_.in_bounds(memory.pos.i,memory.pos.j));
            const int cell=memory.pos.j*w.width()+memory.pos.i;
            check(cell>previous_cell);previous_cell=cell;
            check(static_cast<unsigned>(memory.type)<rts::kBldTypeCount);
            check(memory.hp>=0 && memory.level>=1 && memory.level<=1000000);
            check(memory.observed_at>=0 && memory.observed_at<=w.now());
        }
        check(!w.developer_ && w.wave_>=1 && w.wave_<=1000000 && w.tick_>=0 && w.tick_<=2000000);
        check(b.since_decision_>=0 && b.since_decision_<=rts::kDecisionPeriodMax);
        check(w.u_type_.size()==w.unit_pool_.slot_count());
        check(w.u_level_.size()==w.unit_pool_.slot_count());
        check(w.u_hp_.size()==w.unit_pool_.slot_count());
        check(w.u_max_hp_.size()==w.unit_pool_.slot_count());
        check(w.u_pos_.size()==w.unit_pool_.slot_count());
        check(w.u_squad_.size()==w.unit_pool_.slot_count());
        check(w.u_windup_.size()==w.unit_pool_.slot_count());
        check(w.u_action_.size()==w.unit_pool_.slot_count());
        check(w.u_garrison_.size()==w.unit_pool_.slot_count());
        check(w.u_garrison_target_.size()==w.unit_pool_.slot_count());
        check(w.u_mount_.size()==w.unit_pool_.slot_count());
        check(w.u_charge_.size()==w.unit_pool_.slot_count());
        check(w.u_cd_.size()==w.unit_pool_.slot_count());
        check(w.u_tgt_kind_.size()==w.unit_pool_.slot_count());
        check(w.u_tgt_raw_.size()==w.unit_pool_.slot_count());
        check(w.u_aim_.size()==w.unit_pool_.slot_count());
        check(w.b_type_.size()==w.bld_pool_.slot_count());
        check(w.b_pos_.size()==w.bld_pool_.slot_count());
        check(w.b_hp_.size()==w.bld_pool_.slot_count());
        check(w.b_max_hp_.size()==w.bld_pool_.slot_count());
        check(w.b_work_.size()==w.bld_pool_.slot_count());
        check(w.b_cd_.size()==w.bld_pool_.slot_count());
        check(w.b_windup_.size()==w.bld_pool_.slot_count());
        check(w.b_tgt_raw_.size()==w.bld_pool_.slot_count());
        check(w.b_aim_.size()==w.bld_pool_.slot_count());
        check(w.b_built_.size()==w.bld_pool_.slot_count());
        check(w.b_train_type_.size()==w.bld_pool_.slot_count());
        check(w.b_train_left_.size()==w.bld_pool_.slot_count());
        check(w.b_train_level_.size()==w.bld_pool_.slot_count());
        check(w.b_level_.size()==w.bld_pool_.slot_count());
        check(w.b_upgrade_left_.size()==w.bld_pool_.slot_count());
        check(w.o_type_.size()==w.obstacle_pool_.slot_count());
        check(w.o_pos_.size()==w.obstacle_pool_.slot_count());
        check(w.o_hp_.size()==w.obstacle_pool_.slot_count());
        check(w.o_max_hp_.size()==w.obstacle_pool_.slot_count());
        check(w.o_clear_ordered_.size()==w.obstacle_pool_.slot_count());
        check(w.p_origin_.size()==w.p_pos_.size());
        check(w.p_aim_.size()==w.p_pos_.size());
        check(w.p_speed_.size()==w.p_pos_.size());
        check(w.p_kind_.size()==w.p_pos_.size());
        check(w.p_raw_.size()==w.p_pos_.size());
        check(w.p_dmg_.size()==w.p_pos_.size());
        check(w.p_lvl_pm_.size()==w.p_pos_.size());
        check(w.p_from_high_.size()==w.p_pos_.size());
        check(w.p_aoe_.size()==w.p_pos_.size());
        check(w.p_side_.size()==w.p_pos_.size());
        check(w.p_src_bld_.size()==w.p_pos_.size());
        check(w.p_pos_.size()<2000000 && w.bld_at_.size()==n && w.obstacle_at_.size()==n);
        for(const auto& fog:w.fog_) {
            check(fog.width_==w.width() && fog.height_==w.height());
            check(fog.vis_.size()==n && fog.last_seen_.size()==n && fog.bld_.size()==n && fog.bld_hp_permille_.size()==n && fog.has_bld_.size()==n);
        }
        for(const auto t:w.u_type_) check(static_cast<int>(t)<rts::kUnitTypeCount);
        for(const auto t:w.b_type_) check(static_cast<int>(t)<rts::kBldTypeCount);
        for(const auto t:w.o_type_) check(static_cast<int>(t)<rts::kObstacleTypeCount);
        for(const auto p:w.u_pos_) check(p.x>=0 && p.y>=0 && p.x<float(w.width()) && p.y<float(w.height()));
        for(const auto p:w.b_pos_) check(w.terrain_.in_bounds(p.i,p.j));
        for(const auto p:w.o_pos_) check(w.terrain_.in_bounds(p.i,p.j));
        for(const auto k:w.bld_at_) check(k<=w.bld_pool_.slot_count());
        for(const auto k:w.obstacle_at_) check(k<=w.obstacle_pool_.slot_count());
        for(const auto k:w.u_garrison_) check(k==rts::kNoSlot || k<n);
        for(const auto k:w.u_garrison_target_) check(k==rts::kNoSlot || k<n);
        for(const auto lv:w.u_level_) check(lv>=1);
        for(std::size_t k=0;k<w.b_max_hp_.size();++k) check(w.bld_pool_.alive_[k]?w.b_max_hp_[k]>0:w.b_max_hp_[k]==0);
    }
    static std::string capture(const DemoBattle& b) {return encode(b).dump();}
    static void restore(DemoBattle& b,const std::string& text,const std::vector<DemoBattle::PlayerEvent>& events) {decode(Json::parse(text),b);validate(b);b.player_events_=events;}
};
}
