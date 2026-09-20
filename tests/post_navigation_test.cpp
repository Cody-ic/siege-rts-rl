#include <chrono>
#include <cstdio>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "rts/rng.hpp"
#include "../game/src/post_navigation.hpp"

namespace {
constexpr std::uint16_t all_moves=0x1ff;

// Independent reverse breadth-first oracle, with no heuristic or greedy path.
std::vector<int> distances(const std::vector<std::uint8_t>& open, int w, int h,
                           const std::vector<int>& goals) {
    std::vector<int> result(open.size(),-1), queue;
    for (int goal:goals) {result[static_cast<std::size_t>(goal)]=0;queue.push_back(goal);}
    for (std::size_t head=0; head<queue.size(); ++head) {
        const int c=queue[head];
        for (const auto [dx,dy]:{std::pair{1,0},{-1,0},{0,1},{0,-1}}) {
            const int x=c%w+dx,y=c/w+dy;
            if(x<0 || y<0 || x>=w || y>=h) continue;
            const auto next=static_cast<std::size_t>(y*w+x);
            if(open[next] && result[next]<0) {result[next]=result[static_cast<std::size_t>(c)]+1;queue.push_back(static_cast<int>(next));}
        }
    }
    return result;
}
}

TEST_CASE("Post A-star agrees with shortest paths and obeys first-step masks", "[script][guard][path]") {
    constexpr int w=23,h=19,start=9*w+3;
    rts::Rng rng(31415);
    for(int sample=0;sample<160;++sample) {
        std::vector<std::uint8_t> open(w*h,1);
        for(auto& cell:open) cell=rng.below(100)<32?0:1;
        std::vector<int> goals{8*w+19,10*w+19};
        open[start]=1;for(int goal:goals) open[static_cast<std::size_t>(goal)]=1;
        auto mask=all_moves;
        if(sample%2) mask=static_cast<std::uint16_t>(mask & ~(1u<<static_cast<unsigned>(rts::UnitAction::MoveSE)));
        auto oracle_open=open;
        oracle_open[start]=0; // A valid shortest route never revisits its first cell.
        const auto oracle=distances(oracle_open,w,h,goals);
        int best=100000;
        for(int k=0;k<rts::kMoveDirCount;++k) {
            const auto action=rts::move_of(k);
            if(rts::is_grid_diagonal(action) || !(mask&(1u<<static_cast<unsigned>(action)))) continue;
            const auto delta=rts::move_delta(action);
            const auto next=static_cast<std::size_t>(start+delta.dj*w+delta.di);
            if(oracle[next]>=0) best=std::min(best,oracle[next]+1);
        }
        CAPTURE(sample);
        const auto route=game::detail::route_to_any(open,w,h,start,goals,mask);
        if(best==100000) {REQUIRE(route.goal<0);continue;}
        REQUIRE(std::find(goals.begin(),goals.end(),route.goal)!=goals.end());
        REQUIRE(rts::is_move(route.first));
        REQUIRE_FALSE(rts::is_grid_diagonal(route.first));
        REQUIRE((mask&(1u<<static_cast<unsigned>(route.first)))!=0);
        const auto delta=rts::move_delta(route.first);
        const auto next=static_cast<std::size_t>(start+delta.dj*w+delta.di);
        REQUIRE(oracle[next]==best-1);
    }
}

TEST_CASE("Replanning escapes a deep concave wall and reacts to a newly opened gate", "[script][guard][path]") {
    constexpr int w=40,h=32,start=16*w+24,goal=16*w+30;
    std::vector<std::uint8_t> open(w*h,1);
    for(int y=5;y<=27;++y) open[static_cast<std::size_t>(y*w+26)]=0;
    for(int x=7;x<=26;++x) {open[5*w+x]=0;open[27*w+x]=0;}
    const std::vector<int> goals{goal};
    const auto oracle=distances(open,w,h,goals);
    int c=start;
    for(int step=0;step<oracle[start];++step) {
        const auto route=game::detail::route_to_any(open,w,h,c,goals,all_moves);
        REQUIRE(route.goal==goal);
        REQUIRE(rts::is_move(route.first));
        const auto delta=rts::move_delta(route.first);
        const int next=c+delta.dj*w+delta.di;
        REQUIRE(oracle[static_cast<std::size_t>(next)]==oracle[static_cast<std::size_t>(c)]-1);
        c=next;
    }
    REQUIRE(c==goal);
    for(int y=0;y<h;++y) open[static_cast<std::size_t>(y*w+26)]=0;
    REQUIRE(game::detail::route_to_any(open,w,h,start,goals,all_moves).goal<0);
    open[16*w+26]=1;
    const auto route=game::detail::route_to_any(open,w,h,start,goals,all_moves);
    REQUIRE(route.goal==goal);
    REQUIRE(route.first==rts::UnitAction::MoveSE);
    REQUIRE(game::detail::route_to_any(open,w,h,goal,goals,all_moves).first==rts::UnitAction::Stop);
    REQUIRE(game::detail::route_to_any(open,w,h,start,{},all_moves).goal<0);
}

// Explicit Release diagnostic; timing is not a hardware-specific test threshold.
TEST_CASE("Post A-star diagnostic across obstacle layouts", "[.postbench]") {
    constexpr int w=170,h=170,start=85*w+80,goal=85*w+110,repeats=80;
    const std::vector<int> goals{goal};
    for(int scene=0;scene<5;++scene) {
        std::vector<std::uint8_t> open(w*h,1);
        if(scene==1) for(int y=83;y<=87;++y) open[static_cast<std::size_t>(y*w+82)]=0;
        if(scene==2) {
            for(int y=72;y<=98;++y) open[static_cast<std::size_t>(y*w+82)]=0;
            for(int x=60;x<=82;++x) {open[72*w+x]=0;open[98*w+x]=0;}
        }
        if(scene>=3) for(int y=0;y<h;++y) if(scene==4 || y!=15) open[static_cast<std::size_t>(y*w+90)]=0;
        double elapsed=0;
        int reachable=0;
        for(int repeat=0;repeat<repeats;++repeat) {
            const auto begin=std::chrono::steady_clock::now();
            const auto route=game::detail::route_to_any(open,w,h,start,goals,all_moves);
            elapsed+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-begin).count();
            if(route.goal>=0) ++reachable;
        }
        REQUIRE(reachable==(scene==4?0:repeats));
        std::printf("POST_BENCH algorithm=astar scene=%d calls=%d us_per_call=%.3f\n",scene,repeats,elapsed/repeats);
    }
}
