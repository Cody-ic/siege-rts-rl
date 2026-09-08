#include "game/macro_policy.hpp"
#include "game/macro_observation.hpp"
#include "game/stats_loader.hpp"
#include "rts/hash.hpp"
#include "rts/utf8_path.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>
#if RTS_WITH_ONNX
#include <nlohmann/json.hpp>
#include <onnxruntime_cxx_api.h>
#endif

namespace game {
struct MacroPolicy::Impl {
    int period=20,hidden=64;
    std::uint64_t stats=0;
    std::string identity;
#if RTS_WITH_ONNX
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING,"siege-macro"};
    Ort::Session encoder{nullptr},decoder{nullptr};
#endif
};
MacroPolicy::~MacroPolicy()=default;
int MacroPolicy::period() const noexcept { return p_->period; }
std::uint64_t MacroPolicy::stats_fingerprint() const noexcept { return p_->stats; }
const std::string& MacroPolicy::identity() const noexcept { return p_->identity; }

std::size_t MacroPolicy::sample_index(std::span<const float> logits,double uniform) {
    if(logits.empty() || !std::isfinite(uniform) || uniform<0 || uniform>=1 ||
       !std::all_of(logits.begin(),logits.end(),[](float v){return std::isfinite(v);}))
        throw std::runtime_error("Invalid categorical sampling input");
    const double peak=*std::max_element(logits.begin(),logits.end());
    double total=0;
    for(float value:logits) total+=std::exp(static_cast<double>(value)-peak);
    const double target=uniform*total;
    double cumulative=0;std::size_t last_positive=0;
    for(std::size_t i=0;i<logits.size();++i) {
        const double weight=std::exp(static_cast<double>(logits[i])-peak);
        if(weight>0) last_positive=i;
        cumulative+=weight;
        if(target<cumulative) return i;
    }
    return last_positive;
}

#if RTS_WITH_ONNX
namespace {
std::vector<char> bytes(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file || file.tellg()<=0 || file.tellg()>64*1024*1024)
        throw std::runtime_error("Invalid macro model file size");
    std::vector<char> result(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    if(!file.read(result.data(),static_cast<std::streamsize>(result.size())))
        throw std::runtime_error("Incomplete macro model file");
    return result;
}
std::string identity(std::span<const char> data) {
    rts::StateHash hash;hash.feed(data.data(),data.size());return std::to_string(hash.value());
}
std::vector<float> floats(const Ort::Value& value,const std::vector<std::int64_t>& shape) {
    const auto info=value.GetTensorTypeAndShapeInfo();
    if(info.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || info.GetShape()!=shape)
        throw std::runtime_error("Macro inference output contract mismatch");
    const auto* data=value.GetTensorData<float>();
    std::vector<float> result(data,data+info.GetElementCount());
    if(!std::all_of(result.begin(),result.end(),[](float x){return std::isfinite(x);}))
        throw std::runtime_error("Nonfinite macro inference output");
    return result;
}
Ort::Value tensor(std::vector<float>& data,const std::vector<std::int64_t>& shape) {
    return Ort::Value::CreateTensor<float>(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault),
        data.data(),data.size(),shape.data(),shape.size());
}
int field(const rts::Command& c,int column) {
    if(column==0) return static_cast<int>(c.kind);
    if(column==1) return c.slot;
    if(column==2) return c.what;
    return c.level;
}
}
#endif

MacroPolicy::MacroPolicy(const std::string& directory,const std::string& stats_path)
    : p_(std::make_unique<Impl>()) {
#if RTS_WITH_ONNX
    const auto root=rts::path_from_utf8(directory);
    const auto manifest_bytes=bytes(root/"manifest.json");
    const auto meta=nlohmann::json::parse(manifest_bytes.begin(),manifest_bytes.end());
    std::vector<std::string> commands;
    for(int i=0;i<rts::kCommandKindCount;++i) commands.emplace_back(rts::ident_of(static_cast<rts::CommandKind>(i)));
    if(meta.at("format")!="defender-macro-onnx-v2" || meta.at("obs_version")!=kMacroObsVersion ||
       meta.at("cells")!=macro_cell_names() || meta.at("global_names")!=macro_global_names() ||
       meta.at("detail")!=macro_detail_names() || meta.at("commands")!=commands ||
       meta.at("detail_precision")!="float16-before-float32-inference" ||
       meta.at("selection")!="conditional-greedy" || meta.at("no_slot")!=rts::kNoSlot ||
       meta.at("stats_fnv64")!=game::identity(bytes(rts::path_from_utf8(stats_path))))
        throw std::runtime_error("Incompatible macro model manifest");
    p_->period=meta.at("period").get<int>();p_->hidden=meta.at("hidden").get<int>();
    if(p_->period<1 || p_->period>10000 || p_->hidden<1 || p_->hidden>1024)
        throw std::runtime_error("Invalid macro model dimensions or period");
    p_->stats=StatsLoader::from_file(stats_path).fingerprint();
    p_->identity=game::identity(manifest_bytes);
    Ort::SessionOptions options;options.SetIntraOpNumThreads(1);options.SetInterOpNumThreads(1);
    options.AddConfigEntry("session.intra_op.allow_spinning","0");
    auto enc=bytes(root/"encoder.onnx"),dec=bytes(root/"decoder.onnx");
    if(meta.at("graphs_fnv64").at("encoder.onnx")!=game::identity(enc) ||
       meta.at("graphs_fnv64").at("decoder.onnx")!=game::identity(dec))
        throw std::runtime_error("Macro model graph identity mismatch");
    p_->encoder=Ort::Session(p_->env,enc.data(),enc.size(),options);
    p_->decoder=Ort::Session(p_->env,dec.data(),dec.size(),options);
#else
    (void)directory;(void)stats_path;
    throw std::runtime_error("Macro ONNX runtime unavailable; build with RTS_WITH_ONNX=ON");
#endif
}

rts::Command MacroPolicy::decide(const rts::WorldView& defender,bool summon_allowed,rts::Rng* rng) {
#if RTS_WITH_ONNX
    std::optional<rts::Rng> pending;
    if(rng) {
        const auto state=rng->state();
        if(std::all_of(state.begin(),state.end(),[](auto v){return v==0;}))
            throw std::runtime_error("Invalid all-zero policy RNG state");
        pending=*rng;
    }
    if(defender.side()!=rts::Side::Defender || defender.stats().fingerprint()!=p_->stats)
        throw std::runtime_error("Macro policy requires matching defender view");
    const int height=defender.height(),width=defender.width(),hidden=p_->hidden;
    if(height<1 || width<1 || static_cast<std::int64_t>(height)*width>rts::kNoSlot)
        throw std::runtime_error("Invalid macro map shape");
    auto observation=pack_macro_observation(defender);
    std::vector<float> globals(observation.global.begin(),observation.global.end());
    auto detail=pack_macro_detail(defender);
    for(auto& v:detail) v=Ort::Float16_t(v).ToFloat();
    std::array<Ort::Value,3> input{tensor(observation.cells,{kMacroGrid,kMacroGrid,kMacroChannels}),
        tensor(globals,{kMacroGlobals}),tensor(detail,{height,width,kMacroDetailChannels})};
    constexpr const char* enc_inputs[]={"cells","global","detail"};
    constexpr const char* enc_outputs[]={"spatial","context","fine","value"};
    auto encoded=p_->encoder.Run(Ort::RunOptions{nullptr},enc_inputs,input.data(),3,enc_outputs,4);
    auto spatial=floats(encoded[0],{hidden,kMacroGrid,kMacroGrid});
    auto context=floats(encoded[1],{hidden});
    auto fine=floats(encoded[2],{8,height,width});
    (void)floats(encoded[3],{});
    auto remaining=macro_candidates(defender,summon_allowed);
    constexpr std::array<int,4> columns{0,2,3,1};
    std::array<std::int64_t,3> prefix{0,0,1};
    for(int stage=0;stage<4;++stage) {
        const int column=columns[static_cast<std::size_t>(stage)];
        std::vector<int> choices;
        for(const auto& c:remaining) choices.push_back(field(c,column));
        std::sort(choices.begin(),choices.end());choices.erase(std::unique(choices.begin(),choices.end()),choices.end());
        if(choices.empty()) throw std::runtime_error("Macro policy has no legal command");
        const auto n=stage==3?choices.size():std::size_t{1};
        std::vector<float> local(n*static_cast<std::size_t>(hidden)),features(n*8),coords(n*3);
        for(std::size_t i=0;i<n;++i) {
            const int slot=stage==3?choices[i]:rts::kNoSlot;
            if(slot==rts::kNoSlot) {coords[i*3+2]=1;continue;}
            const int x=slot%width,y=slot/width,gx=x*kMacroGrid/width,gy=y*kMacroGrid/height;
            coords[i*3]=(static_cast<float>(x)+.5f)/static_cast<float>(width);
            coords[i*3+1]=(static_cast<float>(y)+.5f)/static_cast<float>(height);
            for(int c=0;c<hidden;++c) local[i*static_cast<std::size_t>(hidden)+static_cast<std::size_t>(c)]=
                spatial[static_cast<std::size_t>((c*kMacroGrid+gy)*kMacroGrid+gx)];
            for(int c=0;c<8;++c) features[i*8+static_cast<std::size_t>(c)]=fine[static_cast<std::size_t>((c*height+y)*width+x)];
        }
        const auto count=static_cast<std::int64_t>(n);
        const std::array<std::int64_t,1> prefix_shape{3};
        std::array<Ort::Value,5> inputs{tensor(context,{hidden}),tensor(local,{count,hidden}),
            tensor(features,{count,8}),tensor(coords,{count,3}),
            Ort::Value::CreateTensor<std::int64_t>(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault),
                prefix.data(),prefix.size(),prefix_shape.data(),1)};
        constexpr const char* names[]={"context","local","fine","coords","prefix"};
        constexpr const char* outputs[]={"kind","what","level","position"};
        auto decoded=p_->decoder.Run(Ort::RunOptions{nullptr},names,inputs.data(),5,outputs,4);
        const std::array<std::int64_t,4> sizes{rts::kCommandKindCount,256,256,count};
        // Validate every head, including unused heads, before indexing any output.
        std::array<std::vector<float>,4> all;
        for(std::size_t i=0;i<4;++i) all[i]=floats(decoded[i],{sizes[i]});
        const auto& logits=all[static_cast<std::size_t>(stage)];
        std::size_t best=0;float score=-std::numeric_limits<float>::infinity();
        for(std::size_t i=0;i<choices.size();++i) {
            const float v=logits[stage==3?i:static_cast<std::size_t>(choices[i])];
            if(v>score) {score=v;best=i;}
        }
        if(pending) {
            std::vector<float> legal_logits;
            for(std::size_t i=0;i<choices.size();++i)
                legal_logits.push_back(logits[stage==3?i:static_cast<std::size_t>(choices[i])]);
            best=sample_index(legal_logits,pending->unit_float());
        }
        const int chosen=choices[best];
        std::erase_if(remaining,[&](const rts::Command& c){return field(c,column)!=chosen;});
        if(stage<3) prefix[static_cast<std::size_t>(stage)]=chosen;
    }
    if(remaining.size()!=1) throw std::runtime_error("Macro command decoding is ambiguous");
    if(pending) rng->set_state(pending->state());
    return remaining.front();
#else
    (void)defender;(void)summon_allowed;(void)rng;throw std::runtime_error("Macro ONNX runtime unavailable");
#endif
}
}
