#include "game/training_campaign.hpp"
#include "game/macro_observation.hpp"

namespace game {
namespace {
rts::World::Tally difference(const rts::World::Tally& a,const rts::World::Tally& b) {
    return {a.dmg_to_units-b.dmg_to_units,a.dmg_to_blds-b.dmg_to_blds,
        a.units_killed-b.units_killed,a.blds_destroyed-b.blds_destroyed,
        a.bld_value-b.bld_value,a.scouts_killed-b.scouts_killed,a.losses-b.losses,
        a.scout_units_killed-b.scout_units_killed,
        a.masons_killed-b.masons_killed,
        a.phoenix_losses-b.phoenix_losses,
        a.enemy_unit_gold-b.enemy_unit_gold,
        a.repair_wood_spent-b.repair_wood_spent,
        a.friendly_unit_damage-b.friendly_unit_damage,
        a.friendly_units_killed-b.friendly_units_killed};
}
}
TrainingCampaign::TrainingCampaign(const MapData& map,const rts::StatsTable& stats,
    std::uint64_t seed,int macro_period,const MacroParams& params,
    std::shared_ptr<TacticalPolicy> attacker)
    : battle_(map,stats,seed),attacker_policy_(std::move(attacker)),
      macro_(map,params),macro_period_(macro_period) {
    if(macro_period<1) throw rts::ContractError("Campaign macro period must be positive");
    battle_.set_tactical_policy(attacker_policy_);
}

CampaignTransition TrainingCampaign::advance_scripted(int max_ticks) {
    return run(max_ticks,true,{},{});
}
rts::Command TrainingCampaign::teacher_command() const {
    auto teacher=macro_;
    std::vector<rts::Command> proposed;
    std::vector<UnitOrder> ignored_orders;
    teacher.decide(world(),proposed,ignored_orders);
    for(const auto& command:proposed)
        if(macro_command_legal(world().view(rts::Side::Defender),command,summon_allowed())) return command;
    return {};
}
CampaignTransition TrainingCampaign::advance(int max_ticks,
    std::span<const rts::Command> commands,std::span<const UnitOrder> orders) {
    return run(max_ticks,false,commands,orders);
}
CampaignTransition TrainingCampaign::run(int max_ticks,bool scripted,
    std::span<const rts::Command> commands,std::span<const UnitOrder> orders) {
    if(max_ticks<1) throw rts::ContractError("Campaign transition must have positive duration");
    const auto wave=world().wave();
    const auto attacker_before=world().peek_tally(rts::Side::Attacker);
    const auto defender_before=world().peek_tally(rts::Side::Defender);
    CampaignTransition result;
    std::vector<rts::UnitId> audit_units;
    if(!scripted && !defeated()) battle_.submit_defender(commands.data(),commands.size());
    while(result.ticks<max_ticks && !defeated() && world().wave()==wave) {
        if(scripted && world().now()%macro_period_==0) {
            commands_.clear();orders_.clear();
            macro_.decide(world(),commands_,orders_);
            battle_.submit_defender(commands_.data(),commands_.size());
        }
        const auto active_orders=scripted?std::span<const UnitOrder>(orders_):orders;
        for(const auto& order:active_orders) {
            if(order.garrison) battle_.issue_garrison_order(order.ids,order.target);
            else battle_.issue_move_order(order.ids,order.target);
        }
        if(world().phase()==rts::WavePhase::Assault) {
            world().enumerate_units(rts::Side::Attacker,audit_units);
            for(const auto id:audit_units) {
                const auto type=world().unit_type(id);
                if(!rts::is_combat(type)) continue;
                if(attacker_policy_ && attacker_policy_->supports(type,world().unit_level(id)))
                    ++result.model_eligible_unit_ticks;
                else ++result.script_combat_unit_ticks;
            }
        }
        battle_.update(1);
        ++result.ticks;
    }
    result.wave_advanced=world().wave()!=wave;
    result.defeated=defeated();
    result.attacker=difference(world().peek_tally(rts::Side::Attacker),attacker_before);
    result.defender=difference(world().peek_tally(rts::Side::Defender),defender_before);
    return result;
}
} // namespace game
