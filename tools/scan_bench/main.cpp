// Read-only Release diagnostic: same visibility contract, different enumeration.
// No candidate is installed in the game by building/running this executable.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "game/defender_script.hpp"
#include "game/save_game.hpp"
#include "game/stats_loader.hpp"
#include "rts/cli_args.hpp"
#include "rts/utf8_path.hpp"
#include "rts/visibility.hpp"

namespace {
using Clock=std::chrono::steady_clock;
using Json=nlohmann::json;
using Flags=std::vector<std::uint8_t>;
std::uint64_t sink=0;
struct Entity {rts::GridPos cell{}; bool alive=true,job=false; std::uint32_t identity=0;};
struct Observer {rts::Vec2 pos{}; float radius=8; bool aerial=false;};
struct Spec {const char* name; int size,buildings,units,observers,workers; bool clustered=false;};
struct Scene {
    Spec spec;
    std::vector<rts::Terrain> terrain;
    std::unique_ptr<rts::TerrainMasks> masks;
    std::vector<Entity> entities;
    std::vector<Observer> observers;
};
rts::GridPos cell(int x,int y) {return {static_cast<std::int16_t>(x),static_cast<std::int16_t>(y)};}
int offset(rts::GridPos p,int width) {return int(p.j)*width+int(p.i);}
float d2(rts::Vec2 a,rts::Vec2 b) {const auto x=a.x-b.x,y=a.y-b.y;return x*x+y*y;}
bool sees(const Scene& s,const Observer& o,rts::GridPos p) {
    const auto from=rts::grid_of(o.pos);
    if(from==p) return true;
    return d2(o.pos,rts::center_of(p))<=o.radius*o.radius &&
        (o.aerial || rts::line_of_sight(*s.masks,from,p));
}
Scene make_scene(Spec spec,std::uint64_t seed) {
    Scene s{spec,{},{},{},{}};rts::Rng rng(seed);
    s.terrain.assign(static_cast<std::size_t>(spec.size*spec.size),rts::Terrain::Plain);
    // Patches of forest plus a rock ridge with gaps exercise exact LOS.
    for(int y=0;y<spec.size;++y) for(int x=0;x<spec.size;++x) {
        auto& t=s.terrain[static_cast<std::size_t>(y*spec.size+x)];
        if(rng.below(100)<7) t=rts::Terrain::Forest;
        if(x==spec.size/2 && y%17>3) t=rts::Terrain::Rock;
    }
    std::vector<std::uint8_t> used(s.terrain.size(),0);
    auto location=[&](int index) {
        // Four separated settlement/contact zones; no enclosing city rectangle.
        const int cx=(index%2?3:1)*spec.size/4;
        const int cy=(index%4>=2?3:1)*spec.size/4;
        const int spread=spec.clustered?10:spec.size/4;
        for(int tries=0;;++tries) {
            const int x=tries<100?std::clamp(cx+int(rng.below(static_cast<std::uint32_t>(spread)))-spread/2,0,spec.size-1):int(rng.below(static_cast<std::uint32_t>(spec.size)));
            const int y=tries<100?std::clamp(cy+int(rng.below(static_cast<std::uint32_t>(spread)))-spread/2,0,spec.size-1):int(rng.below(static_cast<std::uint32_t>(spec.size)));
            const auto k=static_cast<std::size_t>(y*spec.size+x);
            if(used[k]) continue;
            used[k]=1;s.terrain[k]=rts::Terrain::Plain;return cell(x,y);
        }
    };
    for(int i=0;i<spec.buildings+spec.units;++i)
        s.entities.push_back({location(i),true,i<spec.buildings && i%13==0,static_cast<std::uint32_t>(i+1)});
    s.masks=std::make_unique<rts::TerrainMasks>(spec.size,spec.size,s.terrain.data(),nullptr);
    for(int i=0;i<spec.observers;++i) {
        const auto at=s.entities[static_cast<std::size_t>(i)%s.entities.size()].cell;
        constexpr std::array radii{6.0f,7.0f,8.0f,9.0f,10.0f,12.0f};
        s.observers.push_back({{float(at.i)+0.15f+0.7f*rng.unit_float(),float(at.j)+0.15f+0.7f*rng.unit_float()},radii[static_cast<std::size_t>(i)%radii.size()],i%5==0});
    }
    return s;
}

Flags full_entities(const Scene& s) {
    Flags visible(s.entities.size(),0);
    for(const auto& o:s.observers) for(std::size_t k=0;k<s.entities.size();++k)
        if(s.entities[k].alive && !visible[k] && sees(s,o,s.entities[k].cell)) visible[k]=1;
    return visible;
}
Flags shared_grid(const Scene& s) {
    const int w=s.spec.size;
    Flags grid(static_cast<std::size_t>(w*w),0),visible(s.entities.size(),0);
    for(const auto& o:s.observers) {
        grid[static_cast<std::size_t>(offset(rts::grid_of(o.pos),w))]=1;
        for(int y=std::max(0,int(std::floor(o.pos.y-o.radius)));y<=std::min(w-1,int(std::floor(o.pos.y+o.radius)));++y)
            for(int x=std::max(0,int(std::floor(o.pos.x-o.radius)));x<=std::min(w-1,int(std::floor(o.pos.x+o.radius)));++x) {
                const auto k=static_cast<std::size_t>(y*w+x);
                if(!grid[k] && sees(s,o,cell(x,y))) grid[k]=1;
            }
    }
    for(std::size_t k=0;k<s.entities.size();++k)
        if(s.entities[k].alive) visible[k]=grid[static_cast<std::size_t>(offset(s.entities[k].cell,w))];
    return visible;
}
Flags rebuilt_buckets(const Scene& s,int side) {
    const int bins=(s.spec.size+side-1)/side;
    // Build included in timing, every sample; no stale cross-tick index.
    std::vector<std::vector<std::size_t>> index(static_cast<std::size_t>(bins*bins));
    for(std::size_t k=0;k<s.entities.size();++k) if(s.entities[k].alive) {
        const auto p=s.entities[k].cell;
        index[static_cast<std::size_t>((p.j/side)*bins+p.i/side)].push_back(k);
    }
    Flags visible(s.entities.size(),0);
    for(const auto& o:s.observers) {
        const int x0=std::max(0,int(std::floor(o.pos.x-o.radius))/side);
        const int x1=std::min(bins-1,int(std::floor(o.pos.x+o.radius))/side);
        const int y0=std::max(0,int(std::floor(o.pos.y-o.radius))/side);
        const int y1=std::min(bins-1,int(std::floor(o.pos.y+o.radius))/side);
        for(int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x)
            for(const auto k:index[static_cast<std::size_t>(y*bins+x)])
                if(!visible[k] && sees(s,o,s.entities[k].cell)) visible[k]=1;
    }
    return visible;
}
Flags evaluate(const Scene& s,int method) {
    if(method==0) return full_entities(s);
    if(method==1) return shared_grid(s);
    return rebuilt_buckets(s,std::array{4,8,16}[static_cast<std::size_t>(method-2)]);
}
void require(bool ok,const std::string& message) {if(!ok) throw std::runtime_error(message);}
void consume(const Flags& values) {for(auto v:values) sink=sink*31+v;}
Json timings(std::vector<double> values) {
    std::sort(values.begin(),values.end());
    return { {"samples",values.size()},{"median_ms",values[values.size()/2]},
             {"p95_ms",values[static_cast<std::size_t>(std::ceil(double(values.size())*0.95))-1]},
             {"max_ms",values.back()} };
}
void mutate(Scene& s,int frame) {
    // Destruction, slot reuse/new identity, jobs changing, movement across buckets.
    const auto k=static_cast<std::size_t>((frame*37)%int(s.entities.size()));
    auto& e=s.entities[k];
    e.alive=frame%3!=0;e.identity+=100000;e.job=!e.job;
    e.cell=cell((e.cell.i+17)%s.spec.size,(e.cell.j+11)%s.spec.size);
    for(std::size_t n=static_cast<std::size_t>(s.spec.buildings);n<s.entities.size();n+=7) {
        auto& p=s.entities[n].cell;p.i=static_cast<std::int16_t>((p.i+1)%s.spec.size);
    }
    for(auto& o:s.observers) {
        o.pos.x=std::fmod(o.pos.x+0.37f,float(s.spec.size));
        o.pos.y=std::fmod(o.pos.y+0.21f,float(s.spec.size));
    }
}
std::size_t check_scene(Scene& s,int frames) {
    std::size_t checks=0;
    for(int f=0;f<frames;++f) {
        mutate(s,f);
        if(f%4==0) s.observers.push_back({rts::center_of(s.entities.front().cell),12,false});
        if(f%4==1) s.observers.pop_back();
        if(f%7==0) {
            const auto k=static_cast<std::size_t>((f*193)%int(s.terrain.size()));
            s.terrain[k]=s.terrain[k]==rts::Terrain::Forest?rts::Terrain::Plain:rts::Terrain::Forest;
            s.masks=std::make_unique<rts::TerrainMasks>(s.spec.size,s.spec.size,s.terrain.data(),nullptr);
        }
        const auto oracle=full_entities(s);
        for(int m=1;m<5;++m) {
            require(evaluate(s,m)==oracle,"visibility mismatch: "+std::string(s.spec.name)+" frame="+std::to_string(f)+" method="+std::to_string(m));++checks;
        }
    }
    return checks;
}
Json measure_scene(Scene s,int samples) {
    constexpr std::array names{"full_entities","shared_grid","buckets4","buckets8","buckets16"};
    std::array<std::vector<double>,5> times;
    for(int repeat=-3;repeat<samples;++repeat) {
        if(repeat>=0) mutate(s,repeat);
        const auto oracle=full_entities(s);
        for(int turn=0;turn<5;++turn) {
            const int method=(turn+repeat+5)%5;
            const auto begin=Clock::now();
            const auto result=evaluate(s,method);
            const double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
            require(result==oracle,"timed visibility result mismatch");consume(result);
            if(repeat>=0) times[static_cast<std::size_t>(method)].push_back(ms);
        }
    }
    Json result={{"name",s.spec.name},{"map_size",s.spec.size},{"building_slots",s.spec.buildings},
                 {"unit_slots",s.spec.units},{"observers",s.spec.observers},{"workers",s.spec.workers}};
    for(std::size_t m=0;m<names.size();++m) result["visibility"][names[m]]=timings(times[m]);
    return result;
}

#ifdef _MSC_VER
#define BENCH_NOINLINE __declspec(noinline)
#else
#define BENCH_NOINLINE __attribute__((noinline))
#endif
BENCH_NOINLINE std::vector<std::uint32_t> jobs(const Scene& s) {
    std::vector<std::uint32_t> out;
    for(int i=0;i<s.spec.buildings;++i) {
        const auto& e=s.entities[static_cast<std::size_t>(i)];
        if(e.alive && e.job) out.push_back(e.identity);
    }
    return out;
}
Json measure_jobs(const Scene& s,int samples) {
    std::array<std::vector<double>,2> times;
    std::vector<std::uint32_t> work;
    for(int repeat=-3;repeat<samples;++repeat) for(int turn=0;turn<2;++turn) {
        const int method=(repeat+3+turn)%2;
        const auto begin=Clock::now();
        std::uint64_t total=0;
        for(int worker=0;worker<s.spec.workers;++worker) {
            if(method==0 || worker==0) work=jobs(s);
            total+=work.size(); // Consumers can share the same immutable list.
        }
        const double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        sink+=total;
        require(total==jobs(s).size()*static_cast<std::size_t>(s.spec.workers),"job mismatch");
        if(repeat>=0) times[static_cast<std::size_t>(method)].push_back(ms);
    }
    return {{"per_worker_full_scan",timings(times[0])},{"one_shared_full_scan",timings(times[1])}};
}

struct WorkerInputs {
    Flags open;
    std::vector<float> risk;
    std::vector<std::size_t> jobs;
    bool operator==(const WorkerInputs&) const = default;
};
BENCH_NOINLINE WorkerInputs worker_inputs(const rts::WorldView& v) {
    const int w=v.width(),h=v.height();
    WorkerInputs data{Flags(static_cast<std::size_t>(w*h),0),std::vector<float>(static_cast<std::size_t>(w*h),0),{}};
    for(int c=0;c<w*h;++c) if(v.terrain().passable(c%w,c/w)) data.open[static_cast<std::size_t>(c)]=1;
    for(std::size_t b=0;b<v.bld_pos().size();++b) if(v.bld_alive()[b]) {
        if(!(v.bld_type()[b]==rts::BldType::Gate && v.bld_built()[b]))
            data.open[static_cast<std::size_t>(offset(v.bld_pos()[b],w))]=0;
        if(v.bld_work_left()[b]>0 || v.bld_upgrade_left()[b]>0) data.jobs.push_back(b);
    }
    for(std::size_t b=0;b<v.obstacle_pos().size();++b) if(v.obstacle_alive()[b])
        data.open[static_cast<std::size_t>(offset(v.obstacle_pos()[b],w))]=0;
    for(std::size_t e=0;e<v.unit_pos().size();++e) {
        if(!v.unit_alive()[e] || rts::side_of(v.unit_type()[e])!=rts::Side::Attacker || !rts::is_combat(v.unit_type()[e])) continue;
        const auto p=v.unit_pos()[e];const float radius=v.stats().of(v.unit_type()[e]).range+3;
        for(int y=std::max(0,int(p.y-radius));y<std::min(h,int(p.y+radius)+1);++y)
            for(int x=std::max(0,int(p.x-radius));x<std::min(w,int(p.x+radius)+1);++x) {
                const float d=std::sqrt(d2({float(x)+0.5f,float(y)+0.5f},p));
                if(d<radius) data.risk[static_cast<std::size_t>(y*w+x)]+=radius-d;
            }
    }
    return data;
}
Json measure_worker_inputs(const rts::World& world,int count,int samples) {
    const auto view=world.view(rts::Side::Defender);
    const auto reference=worker_inputs(view);
    std::array<std::vector<double>,2> times;
    for(int repeat=-3;repeat<samples;++repeat) for(int turn=0;turn<2;++turn) {
        const int method=(repeat+3+turn)%2;
        const auto begin=Clock::now();
        WorkerInputs inputs;
        for(int worker=0;worker<count;++worker) {
            if(method==0 || worker==0) inputs=worker_inputs(view);
            sink+=inputs.jobs.size();
        }
        const double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        require(inputs==reference,"shared worker inputs mismatch");
        if(repeat>=0) times[static_cast<std::size_t>(method)].push_back(ms);
    }
    return {{"per_worker_rebuild",timings(times[0])},{"one_shared_rebuild",timings(times[1])},
            {"scope","Terrain/building/obstacle passability, current threat field, job list; excludes per-worker start-cell exception, masks, route and job choice."}};
}

std::unique_ptr<rts::World> make_worker_world(const Scene& s,const rts::StatsTable& stats) {
    rts::WorldInit init;init.width=s.spec.size;init.height=s.spec.size;
    init.terrain=s.terrain;init.stats=stats;init.seed=7;init.keep={1,1};init.map_id="scan-bench";
    init.terrain[static_cast<std::size_t>(offset(init.keep,init.width))]=rts::Terrain::Plain;
    init.buildings.push_back({rts::BldType::Keep,init.keep,2400,2400});
    std::vector<std::uint8_t> occupied(s.terrain.size(),0);
    occupied[static_cast<std::size_t>(offset(init.keep,init.width))]=1;
    auto world=std::make_unique<rts::World>(init);
    for(int i=0;i<s.spec.buildings;++i) {
        const auto& e=s.entities[static_cast<std::size_t>(i)];
        const auto k=static_cast<std::size_t>(offset(e.cell,init.width));
        if(!e.alive || occupied[k]) continue;
        // Traversable gates, blocking walls, and jobs at four distant sites.
        const auto type=i%9==0?rts::BldType::Gate:i%3==0?rts::BldType::Wall:rts::BldType::Tower;
        world->place_bld(type,e.cell,1000,1000,e.job?200:0);occupied[k]=1;
    }
    for(int i=0;i<s.spec.units;++i) {
        const auto p=s.entities[static_cast<std::size_t>(s.spec.buildings+i)].cell;
        world->spawn_unit(i%5==0?rts::UnitType::Shade:rts::UnitType::Ghoul,rts::center_of(p),1,220,220);
        occupied[static_cast<std::size_t>(offset(p,init.width))]=1;
    }
    for(int i=0;i<s.spec.workers;++i) {
        auto p=s.entities[static_cast<std::size_t>((i*13)%s.spec.buildings)].cell;
        for(int attempt=0;attempt<init.width*init.height;++attempt) {
            const auto k=static_cast<std::size_t>(offset(p,init.width));
            if(!occupied[k] && rts::is_passable(s.terrain[k])) break;
            p=cell((p.i+1)%init.width,(p.j+(p.i+1==init.width?1:0))%init.height);
        }
        world->spawn_unit(rts::UnitType::Mason,rts::center_of(p),1,100,100);
        occupied[static_cast<std::size_t>(offset(p,init.width))]=1;
    }
    return world;
}
Json measure_workers(const rts::World& world,int repeats) {
    std::vector<rts::UnitId> all,workers;world.enumerate_units(rts::Side::Defender,all);
    for(auto id:all) if(world.unit_type(id)==rts::UnitType::Mason) workers.push_back(id);
    if(workers.empty()) return {{"workers",0}};
    std::vector<double> times;
    const auto original_hash=world.state_hash();
    std::vector<rts::UnitAction> expected;
    for(int r=-1;r<repeats;++r) {
        game::DefenderScript script({},7);
        std::vector<rts::UnitAction> actions;std::vector<std::uint16_t> wishes;
        const auto begin=Clock::now();
        script.decide(world.view(rts::Side::Defender),workers,actions,wishes);
        const double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
        if(r<0) expected=actions;
        require(actions==expected,"non-deterministic worker batch");
        for(auto a:actions) sink+=static_cast<unsigned>(a);
        if(r>=0) times.push_back(ms);
    }
    require(original_hash==world.state_hash(),"benchmark mutated the world");
    return {{"workers",workers.size()},{"full_decision",timings(times)},
            {"scope","Existing worker script, including risk field, Dijkstra, job selection; static snapshot, not whole-game frame time."}};
}
std::size_t edge_checks() {
    auto s=make_scene({"edges",33,100,80,16,4},71);
    s.observers={{{0.5f,0.5f},6,false},{{16.5f,16.5f},8,false}};
    s.entities={{{6,0},true,false,1},{{7,0},true,false,2},{{16,24},true,false,3},
                {{32,32},true,false,4},{{0,0},true,false,5},{{0,0},false,true,6}};
    s.terrain.assign(33*33,rts::Terrain::Plain);
    s.masks=std::make_unique<rts::TerrainMasks>(33,33,s.terrain.data(),nullptr);
    require(full_entities(s)==Flags{1,0,1,0,1,0},"radius edge oracle");
    s.terrain[3]=rts::Terrain::Forest;
    s.masks=std::make_unique<rts::TerrainMasks>(33,33,s.terrain.data(),nullptr);
    require(full_entities(s)==Flags{0,0,1,0,1,0},"ground LOS oracle");
    s.observers[0].aerial=true;
    require(full_entities(s)==Flags{1,0,1,0,1,0},"aerial LOS oracle");
    const auto saved_observers=s.observers;s.observers.clear();
    for(int m=0;m<5;++m) require(evaluate(s,m)==Flags(s.entities.size(),0),"empty observers");
    s.observers=saved_observers;
    const auto saved_entities=s.entities;s.entities.clear();
    for(int m=0;m<5;++m) require(evaluate(s,m).empty(),"empty entities");
    s.entities=saved_entities;
    // Restore logical building count before dynamic churn checks.
    s.spec.buildings=3;
    return check_scene(s,100)+13;
}
}

int main(int argc,char** argv) {
    try {
        const auto args=rts::utf8_args(argc,argv);
        int samples=31;std::uint64_t seed=7;bool check_only=false;
        std::string stats_file="game/data/stats_placeholder.json",output,archive;
        for(std::size_t i=1;i<args.size();++i) {
            const std::string arg=args[i];
            if(arg=="--check") {check_only=true;continue;}
            require(i+1<args.size(),"missing value: "+arg);const std::string value=args[++i];
            if(arg=="--samples") samples=std::stoi(value);
            else if(arg=="--seed") seed=std::stoull(value);
            else if(arg=="--stats") stats_file=value;
            else if(arg=="--output") output=value;
            else if(arg=="--archive") archive=value;
            else throw std::runtime_error("unknown argument: "+arg);
        }
        require(samples>=5 && samples<=1000,"samples must be 5..1000");
        const std::array specs{
            Spec{"opening",170,120,80,12,4},Spec{"expanded",170,600,240,40,24},
            Spec{"dense",170,1600,600,80,60},Spec{"clustered",170,1600,600,80,60,true},
            Spec{"all_attackers",170,1600,600,600,60},
            Spec{"tower_forest",170,1600,600,1600,60},
            Spec{"stress",255,4000,1600,1600,120},
            Spec{"full_sources",255,4000,1600,5600,120}};
        Json report={{"seed",seed},{"samples",samples},{"units","milliseconds per complete batch"},
                     {"observer_radii",{6,7,8,9,10,12}},{"aerial_fraction",0.2},
                     {"methods_include","Allocation, full bucket rebuild, exact radius/LOS, union and output; no persistent cache."},
                     {"scope","Visibility query prototypes and job-list preparation, not a deployed scouting redesign."}};
        std::size_t checks=edge_checks();
        const auto stats=game::StatsLoader::from_file(stats_file);
        for(const auto& spec:specs) {
            auto scene=make_scene(spec,seed);
            auto validation=make_scene(spec,seed);
            checks+=check_scene(validation,100);
            if(check_only) continue;
            auto row=measure_scene(make_scene(spec,seed),samples);
            row["jobs"]=measure_jobs(scene,samples);
            auto world=make_worker_world(scene,stats);
            row["worker_inputs"]=measure_worker_inputs(*world,spec.workers,samples);
            row["current_worker_script"]=measure_workers(*world,5);
            report["scenarios"].push_back(row);
            std::fprintf(stderr,"completed %s\n",spec.name);
        }
        if(!archive.empty()) {
            auto battle=game::restore_battle(game::read_archive(rts::path_from_utf8(archive)));
            report["player_checkpoint"]={{"tick",battle->world().now()},{"worker_script",measure_workers(battle->world(),samples)}};
        }
        report["exact_equivalence_checks"]=checks;report["sink"]=sink;
        report["build_mode"]=
#ifdef NDEBUG
            "Release";
#else
            "Debug";
#endif
        const auto text=report.dump(2);
        if(output.empty()) std::puts(text.c_str());
        else {std::ofstream out(rts::path_from_utf8(output));require(bool(out),"cannot write output");out<<text<<'\n';}
        return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"scan_bench: %s\n",e.what());return 1;}
}
