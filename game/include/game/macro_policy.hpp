#ifndef GAME_MACRO_POLICY_HPP
#define GAME_MACRO_POLICY_HPP
#include <memory>
#include <string>
#include "rts/world_view.hpp"

namespace game {
// Offline-trained defender inference. Only its filtered view and legal player
// commands enter the network. This object never advances or mutates the world.
class MacroPolicy {
public:
    MacroPolicy(const std::string& directory,const std::string& stats_path);
    ~MacroPolicy();
    MacroPolicy(const MacroPolicy&)=delete;
    MacroPolicy& operator=(const MacroPolicy&)=delete;
    rts::Command decide(const rts::WorldView& defender,bool summon_allowed);
    int period() const noexcept;
    const std::string& identity() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
#endif
