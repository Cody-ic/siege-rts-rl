#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "game/rl_policy.hpp"
#include "game/stats_loader.hpp"
#include "rts/obs.hpp"
#include "rts/utf8_path.hpp"

// Machine-readable deployment verification; never launches a game or trains.
int run(const std::vector<std::string>& argv) {
    try {
        const auto argc=argv.size();
        if (argc != 2 && argc != 4) throw std::runtime_error("Usage: policy_probe STATS [MODEL INPUT_JSON]");
        const auto stats = game::StatsLoader::from_file(argv[1]);
        nlohmann::json result{{"obs_version",rts::kObsVersion},
            {"obs_fingerprint",std::to_string(rts::kObsLayoutFingerprint)},
            {"stats_fingerprint",std::to_string(stats.fingerprint())}};
        if (argc == 4) {
            game::TacticalPolicy policy(argv[2],stats.fingerprint());
            std::ifstream file(rts::path_from_utf8(argv[3]));
            const auto input = nlohmann::json::parse(file);
            const auto c = input.at("cells").get<std::vector<float>>();
            const auto s = input.at("own").get<std::vector<float>>();
            const auto g = input.at("global").get<std::vector<float>>();
            const auto masks = input.at("masks").get<std::vector<std::uint16_t>>();
            auto logits = policy.logits(masks.size(),c,s,g);
            const auto selected = game::TacticalPolicy::argmax(logits,masks);
            std::vector<int> actions;
            for (auto a : selected) actions.push_back(static_cast<int>(a));
            const auto start = std::chrono::steady_clock::now();
            constexpr int repeats = 100;
            for (int i=0;i<repeats;++i) (void)policy.logits(masks.size(),c,s,g);
            const auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            result["logits"]=logits;
            result["actions"]=actions;
            result["identity"]=policy.identity();
            result["mean_inference_ms"]=elapsed/repeats;
        }
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> args;
    for(int i=0;i<argc;++i) {
        const auto utf8=std::filesystem::path(argv[i]).u8string();
        args.emplace_back(reinterpret_cast<const char*>(utf8.data()),utf8.size());
    }
    return run(args);
}
#else
int main(int argc,char** argv) { return run({argv,argv+argc}); }
#endif
