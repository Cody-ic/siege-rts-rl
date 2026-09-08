#ifndef GAME_MACRO_POLICY_HPP
#define GAME_MACRO_POLICY_HPP
#include <memory>
#include <string>
#include <span>
#include "rts/rng.hpp"
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
    // Caller owns one stream per battle and persists it alongside that battle.
    // Null selects greedy. Sampling commits four draws only after success.
    rts::Command decide(const rts::WorldView& defender,bool summon_allowed,rts::Rng* rng=nullptr);
    static std::size_t sample_index(std::span<const float> logits,double uniform);
    int period() const noexcept;
    std::uint64_t stats_fingerprint() const noexcept;
    const std::string& identity() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
#endif
