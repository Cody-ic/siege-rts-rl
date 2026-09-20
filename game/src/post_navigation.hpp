#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <span>
#include <tuple>
#include <vector>

#include "rts/action.hpp"

namespace game::detail {

struct PostRoute {
    int goal = -1;
    rts::UnitAction first = rts::UnitAction::Stop;
};

// Every target is a traversable standing cell, not the occupied building cell.
// Complete A* uses an admissible Manhattan heuristic and deterministic ties.
// No greedy shortcut or wall-clock cutoff may replace a reachable route.
inline PostRoute route_to_any(std::span<const std::uint8_t> open, int w, int h, int start,
                      std::span<const int> goals, std::uint16_t mask) {
    if (goals.empty()) return {};
    const auto distance = [w](int a, int b) {
        return std::abs(a%w-b%w) + std::abs(a/w-b/w);
    };
    const auto estimate = [&](int c) {
        int best = w+h;
        for (const int goal : goals) best = std::min(best, distance(c,goal));
        return best;
    };
    using Node = std::tuple<int,int,int>; // f, h, cell: deterministic ties prefer progress.
    std::priority_queue<Node,std::vector<Node>,std::greater<Node>> pending;
    std::vector<int> cost(open.size(),std::numeric_limits<int>::max());
    std::vector<rts::UnitAction> first(open.size(),rts::UnitAction::Stop);
    cost[static_cast<std::size_t>(start)]=0;
    const int initial=estimate(start);
    pending.emplace(initial,initial,start);
    while (!pending.empty()) {
        const auto [f,remaining,c]=pending.top();
        pending.pop();
        const int d=f-remaining;
        if (cost[static_cast<std::size_t>(c)]!=d) continue;
        if (remaining==0) return {c,first[static_cast<std::size_t>(c)]};
        for (int k=0; k<rts::kMoveDirCount; ++k) {
            const auto action=rts::move_of(k);
            if (rts::is_grid_diagonal(action) || (c==start && (mask & (1u << static_cast<unsigned>(action))) == 0)) continue;
            const auto delta=rts::move_delta(action);
            const int x=c%w+delta.di, y=c/w+delta.dj;
            if (x<0 || y<0 || x>=w || y>=h) continue;
            const int next=y*w+x;
            const auto u=static_cast<std::size_t>(next);
            if (!open[u] || cost[u]<=d+1) continue;
            cost[u]=d+1;
            first[u]=c==start?action:first[static_cast<std::size_t>(c)];
            const int heuristic=estimate(next);
            pending.emplace(d+1+heuristic,heuristic,next);
        }
    }
    return {};
}

} // namespace game::detail
