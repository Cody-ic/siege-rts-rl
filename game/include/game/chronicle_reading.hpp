#pragma once
#include <string>
#include "game/chronicle.hpp"
#include "game/chronicle_appendix_text.hpp"

namespace game {
// Reading permission is separate from the one-time decision. No alternative
// ending is exposed after a choice, including the biography's final chapter.
constexpr bool can_read_ending(ChronicleChoice choice, int ending) noexcept {
    return ending==0 || (ending==1 && choice==ChronicleChoice::Guard) ||
           (ending==2 && choice==ChronicleChoice::Release);
}
inline std::string smiler_for_choice(ChronicleChoice choice) {
    constexpr std::string_view guard="### 如果指挥官选择继续守护\n";
    constexpr std::string_view release="### 如果指挥官选择放下武器\n";
    const auto chapter=kChronicleSmiler.find("## 八\n");
    const auto first=kChronicleSmiler.find(guard);
    const auto second=kChronicleSmiler.find(release);
    static_assert(kChronicleSmiler.find("## 八\n")!=std::string_view::npos);
    static_assert(kChronicleSmiler.find(guard)!=std::string_view::npos);
    static_assert(kChronicleSmiler.find(release)!=std::string_view::npos);
    if(choice==ChronicleChoice::None) return std::string(kChronicleSmiler.substr(0,chapter));
    std::string result(kChronicleSmiler.substr(0,first));
    if(choice==ChronicleChoice::Guard) result+=kChronicleSmiler.substr(first+guard.size(),second-first-guard.size());
    else result+=kChronicleSmiler.substr(second+release.size());
    return result;
}
}
