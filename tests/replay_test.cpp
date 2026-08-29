// 确定性回放：格式、录制器、比对。
//
// `CLAUDE.md` 把回放列为**主要的防 bug 手段**，所以这一批测试要防的第一件事
// 是回放自己变成摆设。三条围着这个转：
//
//   * 一条哈希点都没有的回放**判失败**，不判通过（`Vacuous`）
//   * 录制器**包住** `World`，漏记一次输入是写不出来的
//   * 「对不上」要能区分四种成因，只有一种需要去查代码
//
// 第三条是这份格式的主要价值。回放对不上时最贵的不是修 bug，
// 是分辨这到底是不是 bug——所以「地图换了」「哈希口径变了」「换了平台」
// 这三种预期情况必须报成它们自己，而不是报成第四种。

#include <array>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "rts/hash.hpp"
#include "rts/replay.hpp"
#include "rts/world.hpp"

namespace {

// 一张最小地图 + 三个单位（守方一个、攻方两个）。
//
// **单位是必须的**：`submit_actions` 要求动作数组长度恰好等于活着的单位数，
// 所以没有单位就走不到那条通道——而它是两条输入通道里字节量占九成的一条。
// 这也正是 `WorldInit::units` 存在的理由（见 `rts/world.hpp`）。
//
// 一切数值都是驱动测试用的占位值（CLAUDE.md「关于数值」）。
rts::WorldInit demo_init() {
    rts::WorldInit init;
    init.width = 5;
    init.height = 4;
    init.terrain.assign(20, rts::Terrain::Plain);
    init.keep = rts::GridPos{2, 2};
    init.buildings.push_back(rts::BldInit{rts::BldType::Keep, init.keep, 10, 10});
    init.spawns.push_back(rts::SpawnSite{rts::GridPos{0, 0}});
    init.spawns.push_back(rts::SpawnSite{rts::GridPos{4, 3}});
    // 槽位 0 = 守方，槽位 1、2 = 攻方。`enumerate_units` 按槽位升序，
    // 所以攻方的动作数组顺序是 [Ghoul, Shade]。
    init.units.push_back(rts::UnitInit{rts::UnitType::Archer, rts::Vec2{2.5f, 1.5f}, 1, 8, 8});
    init.units.push_back(rts::UnitInit{rts::UnitType::Ghoul, rts::Vec2{0.5f, 0.5f}, 1, 12, 12});
    init.units.push_back(rts::UnitInit{rts::UnitType::Shade, rts::Vec2{1.5f, 0.5f}, 2, 6, 6});
    init.seed = 0x5EED;
    init.map_id = "replay-demo";
    init.map_content_hash[0] = static_cast<unsigned char>(0xAB);
    init.map_content_hash[31] = static_cast<unsigned char>(0xCD);
    init.nominal_level = 3;
    return init;
}

rts::Command cmd(rts::CommandKind k, rts::Side s) {
    rts::Command c;
    c.kind = k;
    c.side = s;
    return c;
}

// 一段固定的对局：两侧都下命令、两侧都提交动作、中间穿插推进。
// **它是本文件绝大多数用例的输入**，所以它要走到尽可能多的分支。
rts::Replay record_session(rts::World& w, std::uint32_t period = 2) {
    rts::ReplayRecorder rec(w, period);

    const rts::Command summon = cmd(rts::CommandKind::Summon, rts::Side::Defender);
    rec.submit(rts::Side::Defender, &summon, 1);
    rec.advance(2);

    const rts::UnitAction atk_actions[2] = {rts::UnitAction::MoveSE,
                                            rts::UnitAction::AtkWall};
    rec.submit_actions(rts::Side::Attacker, atk_actions, 2);
    rec.advance(3);

    rts::Command macro[2];
    macro[0] = cmd(rts::CommandKind::PickSpawn, rts::Side::Attacker);
    macro[0].slot = 1;
    macro[1] = cmd(rts::CommandKind::Composition, rts::Side::Attacker);
    macro[1].slot = 5;   // 编成位权重（Composition 借用 slot，见 rts/command.hpp）
    macro[1].what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);
    rec.submit(rts::Side::Attacker, macro, 2);
    rec.advance(2);

    const rts::UnitAction def_action = rts::UnitAction::MoveN;
    rec.submit_actions(rts::Side::Defender, &def_action, 1);
    rec.advance(1);

    return rec.finish();
}

// ——字节层的手术刀——
//
// 改坏一份合法文件是唯一能测出「坏文件报的是对的错」的办法。
// 每次改动之后**必须重算校验和**，否则校验和那条检查会先跳出来，
// 把真正想测的那条挡住——这正是先做校验和检查的代价，也是它值得的地方。

void reseal(std::vector<unsigned char>& b) {
    REQUIRE(b.size() >= 8);
    rts::StateHash h;
    h.feed(b.data(), b.size() - 8);
    std::uint64_t v = h.value();
    for (std::size_t k = 0; k < 8; ++k) {
        b[b.size() - 8 + k] = static_cast<unsigned char>((v >> (8 * k)) & 0xFFu);
    }
}

std::uint32_t read_u32(const std::vector<unsigned char>& b, std::size_t at) {
    std::uint32_t v = 0;
    for (std::size_t k = 0; k < 4; ++k) {
        v |= static_cast<std::uint32_t>(b[at + k]) << (8 * k);
    }
    return v;
}

void write_u32(std::vector<unsigned char>& b, std::size_t at, std::uint32_t v) {
    for (std::size_t k = 0; k < 4; ++k) {
        b[at + k] = static_cast<unsigned char>((v >> (8 * k)) & 0xFFu);
    }
}

void write_u64(std::vector<unsigned char>& b, std::size_t at, std::uint64_t v) {
    for (std::size_t k = 0; k < 8; ++k) {
        b[at + k] = static_cast<unsigned char>((v >> (8 * k)) & 0xFFu);
    }
}

// 记录区起点：magic(8) + version(2) + header_bytes(4) + 头部主体。
std::size_t records_begin(const std::vector<unsigned char>& b) {
    return std::size_t{14} + static_cast<std::size_t>(read_u32(b, 10));
}

// 头部主体最后三个 u32 是 record_count / cmd_count / act_count，
// 所以 record_count 在记录区起点往前数 12 字节处。
std::uint32_t declared_record_count(const std::vector<unsigned char>& b) {
    return read_u32(b, records_begin(b) - 12);
}

// 第 `i` 条记录里 hash 字段的字节偏移（记录定长 16，hash 在其内偏移 8）。
std::size_t record_hash_at(const std::vector<unsigned char>& b, std::size_t i) {
    return records_begin(b) + i * 16 + 8;
}

// 第 `i` 条记录里 tick 字段的偏移。
std::size_t record_tick_at(const std::vector<unsigned char>& b, std::size_t i) {
    return records_begin(b) + i * 16;
}

// 把一段 ASCII 换成同样长的另一段。**要求恰好出现一次**——出现多次时
// 改哪一处会变成运气，而那种测试哪天悄悄失效都不会有人知道。
void patch_ascii(std::vector<unsigned char>& b, const std::string& from,
                 const std::string& to) {
    REQUIRE(from.size() == to.size());
    std::size_t found = 0;
    std::size_t at = 0;
    for (std::size_t i = 0; i + from.size() <= b.size(); ++i) {
        bool eq = true;
        for (std::size_t k = 0; k < from.size(); ++k) {
            if (b[i + k] != static_cast<unsigned char>(from[k])) {
                eq = false;
                break;
            }
        }
        if (eq) {
            ++found;
            at = i;
        }
    }
    REQUIRE(found == 1);
    for (std::size_t k = 0; k < to.size(); ++k) {
        b[at + k] = static_cast<unsigned char>(to[k]);
    }
    reseal(b);
}

rts::Replay reload(const std::vector<unsigned char>& b) {
    rts::Replay out;
    std::string err;
    REQUIRE(rts::Replay::from_bytes(b.data(), b.size(), &out, &err));
    REQUIRE(err.empty());
    return out;
}

// 加载必须失败，且报错里要含 `hint`。**检查报错内容而不只是「失败了」**：
// 十几种坏法若都报同一句话，那这些检查就退化成了一条。
void expect_load_error(const std::vector<unsigned char>& b, const std::string& hint) {
    rts::Replay out;
    std::string err;
    REQUIRE_FALSE(rts::Replay::from_bytes(b.data(), b.size(), &out, &err));
    REQUIRE_FALSE(err.empty());
    REQUIRE(err.find(hint) != std::string::npos);
    // 失败时交出来的必须是一份**空**回放。它看起来像合法回放，
    // 所以还得靠「零哈希点即失败」兜住——两道一起才封得死。
    REQUIRE(out.hash_count() == 0);
}

}  // namespace

// ——录制器——

TEST_CASE("录制器不改变仿真", "[replay]") {
    // 录制若有任何副作用，回放测出来的就不是原来那局。
    rts::World a(demo_init());
    rts::World b(demo_init());

    const rts::Replay r = record_session(a);
    {
        // 手工把同一串输入喂给 b，一模一样地走一遍。
        const rts::Command summon = cmd(rts::CommandKind::Summon, rts::Side::Defender);
        b.submit(rts::Side::Defender, &summon, 1);
        b.advance(2);
        const rts::UnitAction atk[2] = {rts::UnitAction::MoveSE, rts::UnitAction::AtkWall};
        b.submit_actions(rts::Side::Attacker, atk, 2);
        b.advance(3);
        rts::Command macro[2];
        macro[0] = cmd(rts::CommandKind::PickSpawn, rts::Side::Attacker);
        macro[0].slot = 1;
        macro[1] = cmd(rts::CommandKind::Composition, rts::Side::Attacker);
        macro[1].slot = 5;
        macro[1].what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);
        b.submit(rts::Side::Attacker, macro, 2);
        b.advance(2);
        const rts::UnitAction def = rts::UnitAction::MoveN;
        b.submit_actions(rts::Side::Defender, &def, 1);
        b.advance(1);
    }
    REQUIRE(a.now() == b.now());
    REQUIRE(a.state_hash() == b.state_hash());
    REQUIRE(r.total_ticks() == a.now());
}

TEST_CASE("录制器拒绝中途起录与零周期", "[replay]") {
    rts::World w(demo_init());
    // 多一层括号：宏参数里的逗号会被当成参数分隔。
    REQUIRE_THROWS_AS((rts::ReplayRecorder(w, 0)), rts::ContractError);
    w.advance(1);
    REQUIRE_THROWS_AS((rts::ReplayRecorder(w, 1)), rts::ContractError);
}

TEST_CASE("提交被拒时不进流", "[replay]") {
    // `World::submit` 是全有或全无的，抛出即一条都没入队。
    // 若这里照记，重放时就会凭空多出一批命令——而那是一个假阳性，
    // 假阳性会让人开始不相信整套回放测试。
    rts::World w(demo_init());
    rts::ReplayRecorder rec(w, 1);
    const std::size_t before = rec.replay().records().size();

    rts::Command bad = cmd(rts::CommandKind::Composition, rts::Side::Defender);
    bad.what = static_cast<std::uint8_t>(rts::UnitType::Ghoul);
    REQUIRE_THROWS_AS(rec.submit(rts::Side::Defender, &bad, 1), rts::ContractError);
    REQUIRE(rec.replay().records().size() == before);
    REQUIRE(rec.replay().commands().empty());
}

TEST_CASE("finish 只能调一次", "[replay]") {
    rts::World w(demo_init());
    rts::ReplayRecorder rec(w, 1);
    rec.advance(1);
    const rts::Replay r = rec.finish();
    REQUIRE(r.total_ticks() == 1);
    REQUIRE_THROWS_AS(rec.finish(), rts::ContractError);
}

TEST_CASE("哈希点按周期落，且 tick 0 与末尾一定有", "[replay]") {
    SECTION("周期 3、推进 7 tick") {
        rts::World w(demo_init());
        rts::ReplayRecorder rec(w, 3);
        rec.advance(7);
        const rts::Replay r = rec.finish();
        // 0、3、6 由 advance 记，7 由 finish 记。
        std::vector<rts::Tick> ticks;
        for (const rts::ReplayRecord& x : r.records()) {
            if (x.kind == rts::ReplayRecordKind::Hash) ticks.push_back(x.tick);
        }
        const std::vector<rts::Tick> want{0, 3, 6, 7};
        REQUIRE(ticks == want);
    }
    SECTION("周期整除时末尾不重复记") {
        rts::World w(demo_init());
        rts::ReplayRecorder rec(w, 3);
        rec.advance(6);
        const rts::Replay r = rec.finish();
        REQUIRE(r.hash_count() == 3);   // 0、3、6，没有第二个 6
    }
    SECTION("一个 tick 都不推也有 tick 0 那一个") {
        // tick 0 的哈希点把「初始状态就不一样」与「跑着跑着歪了」分成两种诊断。
        rts::World w(demo_init());
        rts::ReplayRecorder rec(w, 5);
        const rts::Replay r = rec.finish();
        REQUIRE(r.hash_count() == 1);
        REQUIRE(r.records().front().tick == 0);
    }
}

// ——格式往返——

TEST_CASE("字节往返之后每一个字段都一样", "[replay]") {
    rts::World w(demo_init());
    const rts::Replay r = record_session(w);
    const std::vector<unsigned char> bytes = r.to_bytes();
    const rts::Replay back = reload(bytes);

    REQUIRE(back.format_version() == r.format_version());
    REQUIRE(back.platform_fingerprint() == r.platform_fingerprint());
    REQUIRE(back.hash_tag() == r.hash_tag());
    REQUIRE(back.map_id() == r.map_id());
    REQUIRE(back.map_content_hash() == r.map_content_hash());
    REQUIRE(back.seed() == r.seed());
    REQUIRE(back.nominal_level() == r.nominal_level());
    REQUIRE(back.hash_period() == r.hash_period());
    REQUIRE(back.total_ticks() == r.total_ticks());
    REQUIRE(back.commands() == r.commands());
    REQUIRE(back.actions() == r.actions());

    REQUIRE(back.records().size() == r.records().size());
    for (std::size_t i = 0; i < r.records().size(); ++i) {
        const rts::ReplayRecord& x = r.records()[i];
        const rts::ReplayRecord& y = back.records()[i];
        REQUIRE(y.tick == x.tick);
        REQUIRE(y.kind == x.kind);
        REQUIRE(y.count == x.count);
        REQUIRE(y.hash == x.hash);
        // `offset` **不入文件**，是读侧按 count 累出来的。这一行测的正是那段累加。
        REQUIRE(y.offset == x.offset);
        if (x.kind != rts::ReplayRecordKind::Hash) {
            REQUIRE(y.side == x.side);
        }
    }

    // 再序列化一次必须逐字节相同——否则「回放文件」这个概念本身不稳定。
    REQUIRE(back.to_bytes() == bytes);
}

TEST_CASE("载荷偏移把多条记录分得开", "[replay]") {
    // 两条 Commands 记录共用一个平坦数组，靠 offset + count 切开。
    // 切错的症状是「重放时某一批命令用了邻批的字节」——不报错、只是对不上。
    rts::World w(demo_init());
    rts::ReplayRecorder rec(w, 100);
    rts::Command a[1];
    a[0] = cmd(rts::CommandKind::SelectForce, rts::Side::Defender);
    a[0].force = 3;
    rec.submit(rts::Side::Defender, a, 1);
    rec.advance(1);
    rts::Command b[2];
    b[0] = cmd(rts::CommandKind::Repair, rts::Side::Defender);
    b[0].slot = 7;
    b[1] = cmd(rts::CommandKind::Cancel, rts::Side::Defender);
    b[1].slot = 8;
    rec.submit(rts::Side::Defender, b, 2);
    const rts::Replay r = reload(rec.finish().to_bytes());

    REQUIRE(r.commands().size() == 3);
    std::vector<rts::ReplayRecord> cmds;
    for (const rts::ReplayRecord& x : r.records()) {
        if (x.kind == rts::ReplayRecordKind::Commands) cmds.push_back(x);
    }
    REQUIRE(cmds.size() == 2);
    REQUIRE(cmds[0].offset == 0);
    REQUIRE(cmds[0].count == 1);
    REQUIRE(cmds[1].offset == 1);
    REQUIRE(cmds[1].count == 2);
    REQUIRE(r.commands()[cmds[1].offset + 1].slot == 8);
}

TEST_CASE("存到含中文的路径再读回来", "[replay]") {
    // `rts/utf8_path.hpp` 点名了三个消费者，回放是第三个（当时还没实现）。
    // 这条把它接上：写与读都必须经 `path_from_utf8`，否则在
    // `.../计算机程序设计大作业/` 这样的目录下全线失败，而症状是
    // 「打不开这个文件」而那个文件明明就在。
    rts::World w(demo_init());
    const rts::Replay r = record_session(w);

    const std::string path = std::string(GAME_TESTDATA_UTF8_DIR) + "/回放往返.rtsr";
    REQUIRE(r.save(path));

    rts::Replay back;
    std::string err;
    REQUIRE(rts::Replay::load(path, &back, &err));
    REQUIRE(err.empty());
    REQUIRE(back.to_bytes() == r.to_bytes());
}

TEST_CASE("打不开的文件报打不开，不报格式错", "[replay]") {
    rts::Replay out;
    std::string err;
    REQUIRE_FALSE(rts::Replay::load(
        std::string(GAME_TESTDATA_UTF8_DIR) + "/没有这个文件.rtsr", &out, &err));
    REQUIRE(err.find("打不开") != std::string::npos);
}

// ——坏文件：每一种坏法报它自己那句——

TEST_CASE("坏掉的回放文件报的是对的错", "[replay]") {
    rts::World w(demo_init());
    const std::vector<unsigned char> good = record_session(w).to_bytes();

    SECTION("太短") {
        std::vector<unsigned char> b(4, 0);
        expect_load_error(b, "太短");
    }
    SECTION("magic 不符") {
        std::vector<unsigned char> b = good;
        b[0] = static_cast<unsigned char>('X');
        reseal(b);
        expect_load_error(b, "magic");
    }
    SECTION("版本不符") {
        std::vector<unsigned char> b = good;
        b[8] = static_cast<unsigned char>(rts::kReplayFormatVersion + 1);
        reseal(b);
        expect_load_error(b, "版本");
    }
    SECTION("校验和不符（改了一个字节没重算）") {
        // **这一条是其余各条的前提**：改坏文件而不重算校验和时，
        // 报出来的必须是「文件坏了」，不是某个字段的格式错。
        std::vector<unsigned char> b = good;
        b[b.size() / 2] = static_cast<unsigned char>(b[b.size() / 2] ^ 0xFFu);
        expect_load_error(b, "校验和");
    }
    SECTION("截得只剩两条记录的位置 ⇒ 定长记录那条检查先跳出来") {
        // 定长记录换来的就是这个：截断在读第一条记录**之前**就被发现。
        std::vector<unsigned char> b = good;
        REQUIRE(declared_record_count(good) > 2);
        b.resize(records_begin(good) + 2 * 16 + 8);
        reseal(b);
        expect_load_error(b, "装不下");
    }
    SECTION("只截掉几个载荷字节 ⇒ 报读不完整") {
        std::vector<unsigned char> b = good;
        b.resize(b.size() - 4);
        reseal(b);
        expect_load_error(b, "读不完整");
    }
    SECTION("末尾有多余字节") {
        std::vector<unsigned char> b = good;
        b.insert(b.end() - 8, 4, static_cast<unsigned char>(0));
        reseal(b);
        expect_load_error(b, "多余字节");
    }
    SECTION("头部长度声明错") {
        std::vector<unsigned char> b = good;
        write_u32(b, 10, read_u32(b, 10) + 1);
        reseal(b);
        expect_load_error(b, "头长度不符");
    }
    SECTION("非法的记录类型") {
        std::vector<unsigned char> b = good;
        b[records_begin(b) + 4] = static_cast<unsigned char>(0x7F);
        reseal(b);
        expect_load_error(b, "记录类型");
    }
    SECTION("tick 倒退") {
        // 把**最后**一条记录的 tick 改成 0。改前面那条不行——第 0 条本来就在
        // tick 0，往后几条也可能同 tick，那样构不成倒退，检查会放过去。
        std::vector<unsigned char> b = good;
        const std::uint32_t n = declared_record_count(b);
        REQUIRE(n >= 3);
        write_u32(b, record_tick_at(b, n - 1), 0u);
        reseal(b);
        expect_load_error(b, "倒退");
    }
    SECTION("Hash 记录带了非零 count") {
        std::vector<unsigned char> b = good;
        // 第 0 条一定是 tick 0 的哈希点。
        b[records_begin(b) + 6] = static_cast<unsigned char>(1);
        reseal(b);
        expect_load_error(b, "count 必须为 0");
    }
    SECTION("非法的 UnitAction") {
        std::vector<unsigned char> b = good;
        // 动作载荷在最末尾（校验和之前）。
        b[b.size() - 9] = static_cast<unsigned char>(rts::kUnitActionCount);
        reseal(b);
        expect_load_error(b, "UnitAction");
    }
}

// ——比对——

TEST_CASE("同样的输入流重放一致", "[replay]") {
    rts::World rec_world(demo_init());
    const rts::Replay r = record_session(rec_world);

    rts::World fresh(demo_init());
    const rts::ReplayResult res = rts::replay_verify(r, fresh);
    REQUIRE(res.verdict == rts::ReplayVerdict::Match);
    REQUIRE(res.ok());
    REQUIRE(res.platform_matches);
    REQUIRE(res.hash_tag_matches);
    REQUIRE(res.map_matches);
    // **比对了至少一个哈希点**——否则「通过」只说明没崩。
    REQUIRE(res.checked_hashes > 0);
    REQUIRE(res.first_divergent_tick == -1);
    // 重放完必须落在同一个 tick、同一个状态。
    REQUIRE(fresh.now() == rec_world.now());
    REQUIRE(fresh.state_hash() == rec_world.state_hash());
}

TEST_CASE("经过文件的一圈之后仍然一致", "[replay]") {
    // 序列化若丢了任何一条输入，这里就会红。它与上一条不重复：
    // 上一条测的是重放算法，这一条测的是格式没有丢东西。
    rts::World rec_world(demo_init());
    const rts::Replay r = reload(record_session(rec_world).to_bytes());
    rts::World fresh(demo_init());
    REQUIRE(rts::replay_verify(r, fresh).ok());
}

TEST_CASE("一条哈希点都没有的回放判失败", "[replay]") {
    // **这一条是本文件的地基。** 一个什么都没检查的检查若报绿灯，
    // 它就变成了摆设，而摆设比缺席更危险——缺席看得见。
    // 同「确定性守卫扫到 0 个文件即失败」那条。
    rts::Replay empty;
    empty.set_total_ticks(0);
    REQUIRE(empty.hash_count() == 0);

    rts::World fresh(demo_init());
    const rts::ReplayResult res = rts::replay_verify(empty, fresh);
    REQUIRE(res.verdict == rts::ReplayVerdict::Vacuous);
    REQUIRE_FALSE(res.ok());
}

TEST_CASE("给的世界不是新建的就判 BadInput", "[replay]") {
    rts::World rec_world(demo_init());
    const rts::Replay r = record_session(rec_world);
    rts::World used(demo_init());
    used.advance(1);
    const rts::ReplayResult res = rts::replay_verify(r, used);
    REQUIRE(res.verdict == rts::ReplayVerdict::BadInput);
}

TEST_CASE("初始条件不同就报分歧，且报在 tick 0", "[replay]") {
    // 这是「回放能抓到真问题」最直接的证明：输入流一字不改，
    // 只把建局参数动一下，回放必须红。三种各测一次，因为它们进哈希的路径不同。
    rts::World rec_world(demo_init());
    const rts::Replay r = record_session(rec_world);

    SECTION("种子不同") {
        rts::WorldInit init = demo_init();
        init.seed = 0xBEEF;
        rts::World fresh(std::move(init));
        const rts::ReplayResult res = rts::replay_verify(r, fresh);
        REQUIRE(res.verdict == rts::ReplayVerdict::Diverged);
        REQUIRE(res.first_divergent_tick == 0);
        REQUIRE(res.expected != res.actual);
        // 三项头部核对都过 ⇒ 这是一个真的确定性差异，报错里要这么说。
        REQUIRE(res.message.find("真的确定性缺陷") != std::string::npos);
    }
    SECTION("名义等级不同") {
        rts::WorldInit init = demo_init();
        init.nominal_level = 4;
        rts::World fresh(std::move(init));
        REQUIRE(rts::replay_verify(r, fresh).verdict == rts::ReplayVerdict::Diverged);
    }
    SECTION("初始单位的等级不同") {
        rts::WorldInit init = demo_init();
        init.units[1].level = 9;
        rts::World fresh(std::move(init));
        REQUIRE(rts::replay_verify(r, fresh).verdict == rts::ReplayVerdict::Diverged);
    }
    SECTION("初始单位少一个") {
        rts::WorldInit init = demo_init();
        init.units.pop_back();
        rts::World fresh(std::move(init));
        const rts::ReplayResult res = rts::replay_verify(r, fresh);
        REQUIRE(res.verdict == rts::ReplayVerdict::Diverged);
        // 单位数进哈希，所以这一条**在 tick 0 就报**，还没走到那批动作。
        // 「动作应用不上」那条路径要另外造，见下一个用例。
        REQUIRE(res.first_divergent_tick == 0);
    }
}

TEST_CASE("输入应用不上时接住异常，不让它逃出去", "[replay]") {
    // 分叉的一种表现是 `submit_actions` 抛 `ContractError`（动作数组长度
    // 不再等于活着的单位数）。若不接住，回放测试里最有价值的那一类失败
    // 就会表现为一个未捕获异常，而不是一句「第 N tick 分歧」。
    //
    // **这条用例是破坏性验证逼出来的。** 原先是「初始单位少一个」那个
    // SECTION 兼任的，但那种改动会让 tick 0 的哈希先对不上，比对在到达
    // 那批动作之前就返回了——把 `catch` 换成一个接不住 `ContractError`
    // 的类型，全套测试照样全绿。
    //
    // 所以这里**手工造一份自相矛盾的回放**：哈希点是对的，但那批动作的
    // 条数不对。录制器造不出这种文件（它照实记），而回放文件可以来自任何地方。
    rts::World rec_world(demo_init());
    rts::Replay r = rts::Replay::begin(rec_world, 100);
    r.push_hash(0, rec_world.state_hash());
    const rts::UnitAction one = rts::UnitAction::Stop;
    r.push_actions(0, rts::Side::Attacker, &one, 1);   // 攻方有 2 个单位
    r.set_total_ticks(0);

    rts::World fresh(demo_init());
    const rts::ReplayResult res = rts::replay_verify(r, fresh);
    REQUIRE(res.verdict == rts::ReplayVerdict::Diverged);
    REQUIRE(res.first_divergent_tick == 0);
    REQUIRE(res.message.find("应用不上") != std::string::npos);
    // tick 0 的哈希点是对的，所以它被数进去了——报错不该说「一个都没比」。
    REQUIRE(res.checked_hashes == 1);
}

TEST_CASE("地图身份不同当场拦，不跑完再报确定性缺陷", "[replay]") {
    // 拿错地图是调用方的错，不是仿真的发现。跑八个 tick 再报
    // 「第 0 tick 对不上」只是把一句清楚的话说成一句难懂的话。
    rts::World rec_world(demo_init());
    const rts::Replay r = record_session(rec_world);

    SECTION("map_id 不同") {
        rts::WorldInit init = demo_init();
        init.map_id = "another-map";
        rts::World fresh(std::move(init));
        const rts::ReplayResult res = rts::replay_verify(r, fresh);
        REQUIRE(res.verdict == rts::ReplayVerdict::MapMismatch);
        REQUIRE_FALSE(res.map_matches);
        REQUIRE(res.message.find("换对的地图") != std::string::npos);
        // 当场拦 ⇒ 一个哈希点都没比，世界一个 tick 都没推。
        REQUIRE(res.checked_hashes == 0);
        REQUIRE(fresh.now() == 0);
    }
    SECTION("content_hash 不同（同名地图被改过）") {
        rts::WorldInit init = demo_init();
        init.map_content_hash[5] = static_cast<unsigned char>(0x99);
        rts::World fresh(std::move(init));
        REQUIRE(rts::replay_verify(r, fresh).verdict == rts::ReplayVerdict::MapMismatch);
    }
}

TEST_CASE("哈希口径不同当场拦，报重录", "[replay]") {
    // 1c 若把在途弹丸做成第四组实体，`state_hash` 的喂入清单就变了。
    // 只要 `kWorldHashTag` 一起进一格，失效就以「重录」的形式报出来
    // （刻意不在这里写具体版本号——它已经从 1 走到 3 了，写死必然过期）。
    // **这条替掉了原先打算给弹丸预留一组空实体位的兜底。**
    //
    // 注意这里**不需要**同时改坏一个哈希点。口径不同意味着两边对
    // 「状态是什么」的定义都不一样，比对本身没有意义——哈希碰巧相等
    // 也不构成证据，所以判决不该等分歧发生。
    rts::World rec_world(demo_init());
    std::vector<unsigned char> b = record_session(rec_world).to_bytes();
    patch_ascii(b, std::string(rts::kWorldHashTag), "World/9");

    const rts::Replay r = reload(b);
    REQUIRE(r.hash_tag() == "World/9");

    rts::World fresh(demo_init());
    const rts::ReplayResult res = rts::replay_verify(r, fresh);
    REQUIRE(res.verdict == rts::ReplayVerdict::HashTagMismatch);
    REQUIRE_FALSE(res.hash_tag_matches);
    REQUIRE(res.message.find("重录") != std::string::npos);
    REQUIRE(res.checked_hashes == 0);
    REQUIRE(fresh.now() == 0);
}

TEST_CASE("平台指纹不同则分歧是预期的", "[replay]") {
    // CLAUDE.md：回放在 Windows 与 Linux 之间不可复现。没有这个字段，
    // 跨平台比对会表现为「第 N tick 状态不一致」，然后有人花一天
    // 去找一个不存在的缺陷。
    rts::World rec_world(demo_init());
    std::vector<unsigned char> b = record_session(rec_world).to_bytes();

    std::string fake(rts::kPlatformFingerprint);
    fake.back() = (fake.back() == 'Z') ? 'Y' : 'Z';
    patch_ascii(b, std::string(rts::kPlatformFingerprint), fake);

    SECTION("指纹不同但状态照样对上 ⇒ 判 Match，但要提醒") {
        // 本版仿真一次浮点运算都没有，所以这才是**当前**的实际情形。
        // 硬拒绝会挡掉有用的比对，如实报出来才对。
        const rts::Replay r = reload(b);
        rts::World fresh(demo_init());
        const rts::ReplayResult res = rts::replay_verify(r, fresh);
        REQUIRE(res.verdict == rts::ReplayVerdict::Match);
        REQUIRE_FALSE(res.platform_matches);
        REQUIRE(res.message.find("跨平台") != std::string::npos);
    }
    SECTION("指纹不同且真的对不上 ⇒ 判 PlatformMismatch") {
        // 改掉最后一个哈希点的值，制造一处分歧。
        std::vector<unsigned char> b2 = b;
        std::size_t last_hash = 0;
        {
            const rts::Replay probe = reload(b);
            for (std::size_t i = 0; i < probe.records().size(); ++i) {
                if (probe.records()[i].kind == rts::ReplayRecordKind::Hash) last_hash = i;
            }
        }
        write_u64(b2, record_hash_at(b2, last_hash), 0xDEADBEEFu);
        reseal(b2);

        const rts::Replay r = reload(b2);
        rts::World fresh(demo_init());
        const rts::ReplayResult res = rts::replay_verify(r, fresh);
        REQUIRE(res.verdict == rts::ReplayVerdict::PlatformMismatch);
        REQUIRE(res.message.find("换回录制平台") != std::string::npos);
    }
}

TEST_CASE("报的是**第一个**分歧的 tick", "[replay]") {
    // 回放测试的价值恰好在于「它指的那个 tick 就是第一个错的 tick」。
    // 若实现改成一路跑完再报最后一处，定位就退化成二分查找。
    rts::World rec_world(demo_init());
    std::vector<unsigned char> b = record_session(rec_world).to_bytes();

    const rts::Replay probe = reload(b);
    std::vector<std::size_t> hash_idx;
    for (std::size_t i = 0; i < probe.records().size(); ++i) {
        if (probe.records()[i].kind == rts::ReplayRecordKind::Hash) hash_idx.push_back(i);
    }
    REQUIRE(hash_idx.size() >= 3);

    // 同时改坏第二个和最后一个哈希点，报出来的必须是第二个那个 tick。
    const std::size_t mid = hash_idx[1];
    const std::size_t last = hash_idx.back();
    write_u64(b, record_hash_at(b, mid), 0x1111u);
    write_u64(b, record_hash_at(b, last), 0x2222u);
    reseal(b);

    const rts::Replay r = reload(b);
    rts::World fresh(demo_init());
    const rts::ReplayResult res = rts::replay_verify(r, fresh);
    REQUIRE(res.verdict == rts::ReplayVerdict::Diverged);
    REQUIRE(res.first_divergent_tick == probe.records()[mid].tick);
    REQUIRE(res.expected == 0x1111u);
    // 只比到分歧处就返回，所以后面那个坏点没被数进去。
    REQUIRE(res.checked_hashes == 2);
}

// ——命令队列进哈希（写回放时发现的一处漏项）——

TEST_CASE("待排空的命令队列进状态哈希", "[replay]") {
    // 队列跨 `submit` / `advance` 边界存活，所以它是仿真状态。
    // 漏掉它，哈希的承诺「同一个哈希 ⇒ 同一个未来」就不成立：
    // 两个队列不同的世界会算出同一个数，然后在下一次 advance 分叉——
    // 分叉最终仍会被发现，但报出来的 tick 比真正出问题的地方晚，
    // 而那正好毁掉回放测试唯一的定位能力。
    rts::World a(demo_init());
    rts::World b(demo_init());
    REQUIRE(a.state_hash() == b.state_hash());

    const rts::Command c = cmd(rts::CommandKind::Summon, rts::Side::Defender);
    a.submit(rts::Side::Defender, &c, 1);
    REQUIRE(a.state_hash() != b.state_hash());

    // 排空之后两者仍然不同（Summon 改了阶段），但这条测的是**排空之前**那一步。
    a.advance(1);
    b.advance(1);
    REQUIRE(a.state_hash() != b.state_hash());

    // 长度也要进：「守方一条、攻方零条」与「守方零条、攻方一条」
    // 拼接之后字节相同，只有长度能分开它们。
    rts::World p(demo_init());
    rts::World q(demo_init());
    const rts::Command def_none = cmd(rts::CommandKind::None, rts::Side::Defender);
    const rts::Command atk_none = cmd(rts::CommandKind::None, rts::Side::Attacker);
    p.submit(rts::Side::Defender, &def_none, 1);
    q.submit(rts::Side::Attacker, &atk_none, 1);
    REQUIRE(p.state_hash() != q.state_hash());
}

// ——初始单位（这一版为回放补上的字段）——

TEST_CASE("WorldInit::units 建出的单位可被枚举与操作", "[replay]") {
    rts::World w(demo_init());
    REQUIRE(w.live_unit_count() == 3);
    REQUIRE(w.live_unit_count(rts::Side::Defender) == 1);
    REQUIRE(w.live_unit_count(rts::Side::Attacker) == 2);

    std::vector<rts::UnitId> ids;
    w.enumerate_units(rts::Side::Attacker, ids);
    REQUIRE(ids.size() == 2);
    // **顺序即槽位升序**，也就是 `WorldInit::units` 里的相对顺序。
    // 动作数组按它排，错位的症状是每个单位都拿到邻居的动作——不报错、只是学不动。
    REQUIRE(w.unit_type(ids[0]) == rts::UnitType::Ghoul);
    REQUIRE(w.unit_type(ids[1]) == rts::UnitType::Shade);
    REQUIRE(w.unit_level(ids[1]) == 2);
}

TEST_CASE("初始单位的校验与 spawn_unit 同一份", "[replay]") {
    // 抄一份校验的后果是两处迟早不一致，而不一致的那一侧
    // 会让某种非法初始状态从回放里悄悄进来。
    SECTION("等级低于下限") {
        rts::WorldInit init = demo_init();
        init.units[0].level = 0;
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("血量超过上限") {
        rts::WorldInit init = demo_init();
        init.units[0].hp = init.units[0].max_hp + 1;
        REQUIRE_THROWS_AS(rts::World(std::move(init)), rts::ContractError);
    }
    SECTION("单位顺序换了就是另一份状态") {
        rts::WorldInit a = demo_init();
        rts::WorldInit b = demo_init();
        std::swap(b.units[1], b.units[2]);
        REQUIRE(rts::World(std::move(a)).state_hash() !=
                rts::World(std::move(b)).state_hash());
    }
}

// ——名字——

TEST_CASE("回放相关枚举的标识符唯一非空", "[replay]") {
    std::set<std::string> seen;
    for (int i = 0; i < rts::kReplayRecordKindCount; ++i) {
        const std::string s(
            rts::ident_of(static_cast<rts::ReplayRecordKind>(i)));
        REQUIRE_FALSE(s.empty());
        REQUIRE(seen.insert(s).second);
    }
    REQUIRE(seen.size() == static_cast<std::size_t>(rts::kReplayRecordKindCount));

    std::set<std::string> verdicts;
    for (int i = 0; i <= static_cast<int>(rts::ReplayVerdict::BadInput); ++i) {
        const std::string s(rts::ident_of(static_cast<rts::ReplayVerdict>(i)));
        REQUIRE_FALSE(s.empty());
        REQUIRE(verdicts.insert(s).second);
    }

    REQUIRE_FALSE(rts::kPlatformFingerprint.empty());
    // 指纹里不该出现「未知」——出现了说明这台机器上三个探测宏有一个没命中，
    // 而那会让指纹失去区分能力（所有未知平台都长得一样）。
    REQUIRE(rts::kPlatformFingerprint.find("unknown") == std::string_view::npos);
}
