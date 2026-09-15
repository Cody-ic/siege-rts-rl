#include "game/rl_policy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>

#include "rts/hash.hpp"
#include "rts/batched_env.hpp"
#include "rts/obs.hpp"
#include "rts/utf8_path.hpp"

#if RTS_WITH_ONNX
#include <onnxruntime_cxx_api.h>
#endif

namespace game {

// tactical-policy-1 fixes the otherwise configurable observation defaults.
// Changing these requires a new export/runtime format, not silent tensor drift.
static_assert(rts::ObsNorms{}.cell_capacity==4.0f && rts::ObsNorms{}.aerial_cap==4.0f);
static_assert(rts::FlowTiering{}.mid_from==4 && rts::FlowTiering{}.high_from==8);

struct TacticalPolicy::Impl {
    std::string identity;
    std::vector<int> types;
    int min_level = 1;
    int max_level = 1;
    int period = 6;
    std::uint64_t stats = 0;
    bool macro_goals = false;
#if RTS_WITH_ONNX
    Ort::Env environment{ORT_LOGGING_LEVEL_WARNING, "siege-policy"};
    Ort::Session session{nullptr};
#endif
};

bool TacticalPolicy::runtime_available() noexcept {
#if RTS_WITH_ONNX
    return true;
#else
    return false;
#endif
}

namespace {
std::vector<char> read_policy_bytes(const std::string& path) {
    std::ifstream file(rts::path_from_utf8(path), std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open tactical policy: " + path);
    const auto length = file.tellg();
    if (length <= 0 || length > 64 * 1024 * 1024)
        throw std::runtime_error("Tactical policy file must be between 1 byte and 64 MiB");
    std::vector<char> bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Incomplete tactical policy file");
    return bytes;
}
std::string content_identity(std::span<const char> bytes) {
    rts::StateHash hash;
    hash.feed(bytes.data(), bytes.size());
    return std::to_string(hash.value());
}
} // namespace

std::string TacticalPolicy::file_identity(const std::string& path) {
    return content_identity(read_policy_bytes(path));
}

TacticalPolicy::TacticalPolicy(const std::string& path, std::uint64_t stats_fingerprint)
    : p_(std::make_unique<Impl>()) {
#if RTS_WITH_ONNX
    const auto bytes=read_policy_bytes(path);
    p_->identity=content_identity(bytes);

    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    options.SetInterOpNumThreads(1);
    options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options.AddConfigEntry("session.intra_op.allow_spinning", "0");
    p_->session = Ort::Session(p_->environment, bytes.data(), bytes.size(), options);
    Ort::AllocatorWithDefaultOptions allocator;
    const auto metadata = p_->session.GetModelMetadata();
    const auto field = [&](const char* key) {
        const auto value = metadata.LookupCustomMetadataMapAllocated(key, allocator);
        if (!value) throw std::runtime_error(std::string("Missing policy metadata: ") + key);
        return std::string(value.get());
    };
    const auto require = [&](const char* key, const std::string& expected) {
        if (field(key) != expected)
            throw std::runtime_error(std::string("Incompatible policy metadata: ") + key);
    };
    const auto format=field("rts.format");
    if(format=="tactical-policy-2") {
        require("rts.goal_semantics","macro-flow-v1");
        p_->macro_goals=true;
    } else if(format!="tactical-policy-1") {
        throw std::runtime_error("Unsupported tactical policy format");
    }
    require("rts.obs_version", std::to_string(rts::kObsVersion));
    require("rts.obs_fingerprint", std::to_string(rts::kObsLayoutFingerprint));
    require("rts.stats_fingerprint", std::to_string(stats_fingerprint));
    p_->stats = stats_fingerprint;
    require("rts.cells_layout", "NHWC");
    require("rts.actions", std::to_string(rts::kUnitActionCount));
    require("rts.agent_semantics", "squad-leader-broadcast-v1");
    std::istringstream types(field("rts.unit_types"));
    for (std::string token; std::getline(types, token, ',');) {
        std::size_t used = 0;
        const int type = std::stoi(token, &used);
        if (used != token.size() || type < 0 || type >= rts::kUnitTypeCount ||
            rts::side_of(static_cast<rts::UnitType>(type)) != rts::Side::Attacker)
            throw std::runtime_error("Invalid policy unit type");
        p_->types.push_back(type);
    }
    if (p_->types.empty()) throw std::runtime_error("Policy declares no supported units");
    const auto level = [&](const char* key) {
        const auto value = field(key);
        std::size_t used = 0;
        const int number = std::stoi(value, &used);
        if (used != value.size() || number < 1) throw std::runtime_error("Invalid policy level range");
        return number;
    };
    p_->min_level = level("rts.min_level");
    p_->max_level = level("rts.max_level");
    if (p_->max_level < p_->min_level) throw std::runtime_error("Reversed policy level range");
    p_->period = level("rts.ticks_per_step");
    if (p_->period < rts::kDecisionPeriodMin || p_->period > rts::kDecisionPeriodMax)
        throw std::runtime_error("Unsupported policy decision period");

    constexpr std::array<const char*,3> names{"cells", "own", "global"};
    const std::array<std::vector<std::int64_t>,3> shapes{{
        {-1,rts::kObsK,rts::kObsK,rts::kObsChannelCount}, {-1,rts::kObsSelfCount}, {-1,rts::kObsGlobalFloats}}};
    if (p_->session.GetInputCount() != 3 || p_->session.GetOutputCount() != 1)
        throw std::runtime_error("Policy must have exactly three inputs and one output");
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto name = p_->session.GetInputNameAllocated(i, allocator);
        const auto info = p_->session.GetInputTypeInfo(i);
        const auto type = info.GetTensorTypeAndShapeInfo();
        if (std::string(name.get()) != names[i] || type.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            type.GetShape() != shapes[i]) throw std::runtime_error("Incompatible policy input layout");
    }
    const auto output_name = p_->session.GetOutputNameAllocated(0, allocator);
    const auto output_info = p_->session.GetOutputTypeInfo(0);
    const auto output = output_info.GetTensorTypeAndShapeInfo();
    if (std::string(output_name.get()) != "logits" || output.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        output.GetShape() != std::vector<std::int64_t>{-1,rts::kUnitActionCount})
        throw std::runtime_error("Incompatible policy output layout");
#else
    (void)path;
    (void)stats_fingerprint;
    throw std::runtime_error("This build does not include the ONNX policy runtime");
#endif
}

TacticalPolicy::~TacticalPolicy() = default;
TacticalPolicy::TacticalPolicy(TacticalPolicy&&) noexcept = default;
TacticalPolicy& TacticalPolicy::operator=(TacticalPolicy&&) noexcept = default;
const std::string& TacticalPolicy::identity() const noexcept { return p_->identity; }
int TacticalPolicy::ticks_per_step() const noexcept { return p_->period; }
std::uint64_t TacticalPolicy::stats_fingerprint() const noexcept { return p_->stats; }
bool TacticalPolicy::supports_macro_goals() const noexcept { return p_->macro_goals; }
bool TacticalPolicy::supports(rts::UnitType type, int level) const noexcept {
    return level >= p_->min_level && level <= p_->max_level &&
        std::find(p_->types.begin(),p_->types.end(),static_cast<int>(type)) != p_->types.end();
}

std::vector<float> TacticalPolicy::logits(std::size_t agents, std::span<const float> cells,
                                        std::span<const float> own, std::span<const float> globals) {
    if (agents > rts::BatchedEnv::kMaxUnitsPerEnv || cells.size() != agents*rts::kObsCellFloats ||
        own.size() != agents*rts::kObsSelfFloats || globals.size() != agents*rts::kObsGlobalFloats)
        throw std::runtime_error("Invalid policy observation sizes");
    for (const auto values : {cells,own,globals})
        if (!std::all_of(values.begin(),values.end(),[](float v){return std::isfinite(v);}))
            throw std::runtime_error("Nonfinite policy observation");
    if (agents == 0) return {};
#if RTS_WITH_ONNX
    const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
    const auto n = static_cast<std::int64_t>(agents);
    const std::array<std::vector<std::int64_t>,3> shapes{{
        {n,rts::kObsK,rts::kObsK,rts::kObsChannelCount},{n,rts::kObsSelfFloats},{n,rts::kObsGlobalFloats}}};
    const std::array<std::span<const float>,3> arrays{cells,own,globals};
    std::vector<Ort::Value> inputs;
    for (std::size_t i=0;i<arrays.size();++i)
        inputs.push_back(Ort::Value::CreateTensor<float>(memory,const_cast<float*>(arrays[i].data()),
            arrays[i].size(),shapes[i].data(),shapes[i].size()));
    constexpr const char* names[] = {"cells","own","global"};
    constexpr const char* outputs[] = {"logits"};
    auto values = p_->session.Run(Ort::RunOptions{nullptr},names,inputs.data(),3,outputs,1);
    const auto shape = values[0].GetTensorTypeAndShapeInfo().GetShape();
    if (shape != std::vector<std::int64_t>{n,rts::kUnitActionCount})
        throw std::runtime_error("Policy produced an unexpected output shape");
    const float* data = values[0].GetTensorData<float>();
    std::vector<float> result(data,data+agents*rts::kUnitActionCount);
    if (!std::all_of(result.begin(),result.end(),[](float v){return std::isfinite(v);}))
        throw std::runtime_error("Policy produced nonfinite logits");
    return result;
#else
    throw std::runtime_error("ONNX policy runtime unavailable");
#endif
}

std::vector<rts::UnitAction> TacticalPolicy::argmax(std::span<const float> logits,
                                                  std::span<const std::uint16_t> masks) {
    if (logits.size() != masks.size()*rts::kUnitActionCount) throw std::runtime_error("Invalid policy logits");
    std::vector<rts::UnitAction> result(masks.size(),rts::UnitAction::Stop);
    for (std::size_t i=0;i<masks.size();++i) {
        float best = -std::numeric_limits<float>::infinity();
        for (int a=0;a<rts::kUnitActionCount;++a) {
            const float value = logits[i*rts::kUnitActionCount+static_cast<std::size_t>(a)];
            if (!std::isfinite(value)) throw std::runtime_error("Nonfinite policy logit");
            if ((masks[i] & (1u<<a)) != 0 && value > best) {
                result[i]=static_cast<rts::UnitAction>(a);
                best=value;
            }
        }
        if (!std::isfinite(best)) throw std::runtime_error("Empty policy action mask");
    }
    return result;
}

std::size_t apply_tactical_policy(const rts::World& w, TacticalPolicy& policy,
                                 std::span<const rts::UnitId> ids,
                                 std::span<rts::UnitAction> actions,
                                 std::span<const std::uint8_t> goal_sets,
                                 std::span<const rts::GridPos> economy_goals) {
    if (ids.size()!=actions.size()) throw std::runtime_error("Policy action/ID count mismatch");
    if(!goal_sets.empty() && goal_sets.size()!=ids.size()) throw std::runtime_error("Policy goal/ID count mismatch");
    for(auto group:goal_sets) if(group>1) throw std::runtime_error("Invalid tactical goal set");
    for(auto cell:economy_goals) if(cell.i<0 || cell.j<0 || cell.i>=w.width() || cell.j>=w.height())
        throw std::runtime_error("Tactical goal outside map");
    std::vector<rts::UnitId> leaders;
    w.enumerate_squads(rts::Side::Attacker,leaders);
    std::erase_if(leaders,[&](rts::UnitId id){return !policy.supports(w.unit_type(id),w.unit_level(id));});
    if (leaders.empty()) return 0;
    const auto n=leaders.size();
    std::vector<float> cells(n*rts::kObsCellFloats),own(n*rts::kObsSelfFloats),glob(n*rts::kObsGlobalFloats);
    std::vector<std::uint16_t> masks;
    std::array<std::optional<rts::FlowField>,rts::kUnitTypeCount*rts::kFlowTierCount*2> fields;
    const auto view=w.view(rts::Side::Attacker);
    const rts::GridPos goal[]={w.keep_pos()};
    const rts::FlowTiering tiers;
    for (std::size_t q=0;q<n;++q) {
        const auto id=leaders[q];
        const auto type=w.unit_type(id);
        const int tier=rts::flow_tier_of(w.unit_level(id),tiers);
        std::size_t group=0;
        if(policy.supports_macro_goals() && !goal_sets.empty() && !economy_goals.empty()) {
            const auto found=std::find(ids.begin(),ids.end(),id);
            if(found!=ids.end()) group=goal_sets[static_cast<std::size_t>(found-ids.begin())];
        }
        auto& field=fields[(static_cast<std::size_t>(type)*rts::kFlowTierCount+static_cast<std::size_t>(tier))*2+group];
        const std::span<const rts::GridPos> targets=group==1?economy_goals:std::span<const rts::GridPos>(goal);
        if (!field) field.emplace(rts::FlowField::compute(view,type,tier,targets,tiers));
        rts::pack_unit_obs(view,id,&*field,{},std::span(cells).subspan(q*rts::kObsCellFloats,rts::kObsCellFloats),
            std::span(own).subspan(q*rts::kObsSelfFloats,rts::kObsSelfFloats),
            std::span(glob).subspan(q*rts::kObsGlobalFloats,rts::kObsGlobalFloats));
        masks.push_back(w.action_mask(id));
    }
    // The network shares parameters across independent squads. Real waves are
    // not limited to the training buffer's 32 rows; infer contiguous batches
    // without dropping squads or exceeding the verified runtime batch contract.
    std::vector<rts::UnitAction> selected;
    selected.reserve(n);
    constexpr std::size_t batch = rts::BatchedEnv::kMaxUnitsPerEnv;
    for (std::size_t first=0;first<n;first+=batch) {
        const auto count=std::min(batch,n-first);
        auto logits=policy.logits(count,
            std::span(cells).subspan(first*rts::kObsCellFloats,count*rts::kObsCellFloats),
            std::span(own).subspan(first*rts::kObsSelfFloats,count*rts::kObsSelfFloats),
            std::span(glob).subspan(first*rts::kObsGlobalFloats,count*rts::kObsGlobalFloats));
        auto actions_for_batch=TacticalPolicy::argmax(logits,std::span(masks).subspan(first,count));
        selected.insert(selected.end(),actions_for_batch.begin(),actions_for_batch.end());
    }
    for (std::size_t i=0;i<ids.size();++i) {
        const auto squad=w.unit_squad(ids[i]);
        for (std::size_t q=0;q<n;++q) {
            if (ids[i]==leaders[q] || (squad!=rts::kNoSquad && squad==w.unit_squad(leaders[q]))) {
                actions[i]=selected[q];break;
            }
        }
    }
    return n;
}
}  // namespace game
