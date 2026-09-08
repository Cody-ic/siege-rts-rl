#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cmath>
#include <limits>
#include "game/macro_policy.hpp"

TEST_CASE("Macro inverse CDF is stable, normalized and validates inputs", "[macrosampling]") {
    const std::array<float,3> logits{0,std::log(2.f),0};
    REQUIRE(game::MacroPolicy::sample_index(logits,0)==0);
    REQUIRE(game::MacroPolicy::sample_index(logits,.24)==0);
    REQUIRE(game::MacroPolicy::sample_index(logits,.26)==1);
    REQUIRE(game::MacroPolicy::sample_index(logits,.74)==1);
    REQUIRE(game::MacroPolicy::sample_index(logits,.76)==2);
    std::array<int,3> counts{};
    for(int i=0;i<10000;++i) ++counts[game::MacroPolicy::sample_index(logits,(i+.5)/10000.)];
    REQUIRE(counts==std::array<int,3>{2500,5000,2500});
    const std::array<float,3> extreme{-1e30f,1e30f,-1e30f};
    REQUIRE(game::MacroPolicy::sample_index(extreme,0)==1);
    REQUIRE(game::MacroPolicy::sample_index(extreme,.999999)==1);
    REQUIRE_THROWS(game::MacroPolicy::sample_index({},.5));
    REQUIRE_THROWS(game::MacroPolicy::sample_index(logits,-.1));
    REQUIRE_THROWS(game::MacroPolicy::sample_index(logits,1));
    REQUIRE_THROWS(game::MacroPolicy::sample_index(logits,std::numeric_limits<double>::quiet_NaN()));
    const std::array<float,1> bad{std::numeric_limits<float>::infinity()};
    REQUIRE_THROWS(game::MacroPolicy::sample_index(bad,.5));
}
