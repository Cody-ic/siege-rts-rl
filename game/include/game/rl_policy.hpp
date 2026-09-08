#ifndef GAME_RL_POLICY_HPP
#define GAME_RL_POLICY_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rts/action.hpp"
#include "rts/roster.hpp"
#include "rts/world.hpp"

namespace game {

// CPU inference only. Public observations enter; no World or hidden state is
// accessible to the network. A model's metadata must match the runtime contract.
class TacticalPolicy {
public:
    TacticalPolicy(const std::string& utf8_model_path, std::uint64_t stats_fingerprint);
    ~TacticalPolicy();
    TacticalPolicy(TacticalPolicy&&) noexcept;
    TacticalPolicy& operator=(TacticalPolicy&&) noexcept;
    TacticalPolicy(const TacticalPolicy&) = delete;
    TacticalPolicy& operator=(const TacticalPolicy&) = delete;

    static bool runtime_available() noexcept;
    // Available without ONNX: used to choose a model-specific save directory
    // before loading the saved map/stats. Same content identity as identity().
    static std::string file_identity(const std::string& utf8_model_path);
    const std::string& identity() const noexcept;
    bool supports(rts::UnitType type, int level) const noexcept;
    int ticks_per_step() const noexcept;
    std::uint64_t stats_fingerprint() const noexcept;

    // cells: [agents,K,K,C], own: [agents,S], globals: [agents,G].
    // Masks use the same bit positions as rts::World::action_mask.
    std::vector<float> logits(std::size_t agents, std::span<const float> cells,
                              std::span<const float> own, std::span<const float> globals);
    static std::vector<rts::UnitAction> argmax(std::span<const float> logits,
                                              std::span<const std::uint16_t> masks);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Override only supported squads, in enumerate_units order. This explicit model
// contract reproduces BatchedEnv; the default game's scripted controller remains
// independent. Only the attacker's fog-filtered view enters the observation pack.
std::size_t apply_tactical_policy(const rts::World& world, TacticalPolicy& policy,
                                 std::span<const rts::UnitId> ids,
                                 std::span<rts::UnitAction> actions);

}  // namespace game
#endif
