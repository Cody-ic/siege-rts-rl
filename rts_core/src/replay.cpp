#include "rts/replay.hpp"

#include <string>
#include <utility>

#include "rts/hash.hpp"
#include "rts/utf8_path.hpp"

namespace rts {
namespace {

// ——小端读写——
//
// 两个目标平台都是小端 x86-64，所以 memcpy 结构体在**今天**是对的。
// 仍然逐字节写，理由是这段代码总共不到三十行，而它换掉的是一整类
// 「在我机器上是对的」——包括将来某人在 arm 上跑一次，以及
// 结构体填充字节悄悄进文件（`Command` 现在无填充，但那是被断言钉住的，
// 不是天生的）。

void put_u8(std::vector<unsigned char>& b, std::uint8_t v) { b.push_back(v); }

void put_u16(std::vector<unsigned char>& b, std::uint16_t v) {
    b.push_back(static_cast<unsigned char>(v & 0xFFu));
    b.push_back(static_cast<unsigned char>((v >> 8) & 0xFFu));
}

void put_u32(std::vector<unsigned char>& b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        b.push_back(static_cast<unsigned char>((v >> (8 * k)) & 0xFFu));
    }
}

void put_u64(std::vector<unsigned char>& b, std::uint64_t v) {
    for (int k = 0; k < 8; ++k) {
        b.push_back(static_cast<unsigned char>((v >> (8 * k)) & 0xFFu));
    }
}

void put_i32(std::vector<unsigned char>& b, std::int32_t v) {
    put_u32(b, static_cast<std::uint32_t>(v));
}

void put_str(std::vector<unsigned char>& b, std::string_view s) {
    if (s.size() > 0xFFFFu) throw ContractError("回放头里的字符串超过 65535 字节");
    put_u16(b, static_cast<std::uint16_t>(s.size()));
    for (const char c : s) b.push_back(static_cast<unsigned char>(c));
}

void put_command(std::vector<unsigned char>& b, const Command& c) {
    put_u16(b, c.slot);
    put_u8(b, static_cast<std::uint8_t>(c.kind));
    put_u8(b, static_cast<std::uint8_t>(c.side));
    put_u8(b, c.what);
    put_u8(b, c.level);
    // `_reserved[2]` 只在内存布局里存在（防编译器填充），不写进文件——
    // 所以载荷是 6 字节/条，不是 sizeof(Command) 的 8。
}

// 读游标。**一旦 `ok` 变 false 就不再前进**，于是所有后续读取返回 0
// 而不是越界——调用方可以一路读完再统一查一次，不必每行判。
struct Cursor {
    const unsigned char* p = nullptr;
    std::size_t n = 0;
    std::size_t i = 0;
    bool ok = true;

    bool need(std::size_t k) noexcept {
        if (!ok) return false;
        if (k > n - i) {          // i <= n 恒成立，这么写不会溢出
            ok = false;
            return false;
        }
        return true;
    }

    std::uint8_t u8() noexcept {
        if (!need(1)) return 0;
        return p[i++];
    }

    std::uint16_t u16() noexcept {
        if (!need(2)) return 0;
        const std::uint16_t v = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(p[i]) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[i + 1]) << 8));
        i += 2;
        return v;
    }

    std::uint32_t u32() noexcept {
        if (!need(4)) return 0;
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            v |= static_cast<std::uint32_t>(p[i + static_cast<std::size_t>(k)])
                 << (8 * k);
        }
        i += 4;
        return v;
    }

    std::uint64_t u64() noexcept {
        if (!need(8)) return 0;
        std::uint64_t v = 0;
        for (int k = 0; k < 8; ++k) {
            v |= static_cast<std::uint64_t>(p[i + static_cast<std::size_t>(k)])
                 << (8 * k);
        }
        i += 8;
        return v;
    }

    std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }

    std::string str() {
        const std::size_t len = static_cast<std::size_t>(u16());
        if (!need(len)) return {};
        std::string s(reinterpret_cast<const char*>(p + i), len);
        i += len;
        return s;
    }
};

std::uint64_t fnv_over(const unsigned char* p, std::size_t n) noexcept {
    StateHash h;
    h.feed(p, n);
    return h.value();
}

std::string with_u64(std::string_view prefix, std::uint64_t v) {
    return std::string(prefix) + std::to_string(v);
}

std::string hex64(std::uint64_t v) {
    static const char* kDigits = "0123456789abcdef";
    std::string s = "0x";
    for (int k = 15; k >= 0; --k) {
        s.push_back(kDigits[(v >> (4 * k)) & 0xFu]);
    }
    return s;
}

}  // namespace

// ——Replay——

Replay Replay::begin(const World& w, std::uint32_t hash_period) {
    if (hash_period == 0) {
        throw ContractError("hash_period 必须 >= 1：0 会录出一份没有哈希点的回放，"
                            "而那种回放验证不了任何东西");
    }
    if (w.now() != 0) {
        throw ContractError("只能从刚建好的世界开始录（now() 必须为 0）——"
                            "中途起录需要状态快照，那是另一件事，见 rts/replay.hpp");
    }
    Replay r;
    r.map_id_ = w.map_id();
    r.map_content_hash_ = w.map_content_hash();
    r.stats_fp_ = w.stats_fingerprint();
    r.seed_ = w.seed();
    r.nominal_level_ = w.nominal_level();
    r.hash_period_ = hash_period;
    r.total_ticks_ = 0;
    return r;
}

int Replay::hash_count() const noexcept {
    int c = 0;
    for (const ReplayRecord& rec : records_) {
        if (rec.kind == ReplayRecordKind::Hash) ++c;
    }
    return c;
}

void Replay::push_commands(Tick t, Side s, const Command* p, std::size_t n) {
    if (n > 0xFFFFu) throw ContractError("一次提交的命令数超过 65535");
    ReplayRecord rec;
    rec.tick = t;
    rec.kind = ReplayRecordKind::Commands;
    rec.side = s;
    rec.count = static_cast<std::uint16_t>(n);
    rec.offset = static_cast<std::uint32_t>(commands_.size());
    commands_.insert(commands_.end(), p, p + n);
    records_.push_back(rec);
}

void Replay::push_actions(Tick t, Side s, const UnitAction* p, std::size_t n) {
    if (n > 0xFFFFu) throw ContractError("一次提交的动作数超过 65535");
    ReplayRecord rec;
    rec.tick = t;
    rec.kind = ReplayRecordKind::Actions;
    rec.side = s;
    rec.count = static_cast<std::uint16_t>(n);
    rec.offset = static_cast<std::uint32_t>(actions_.size());
    actions_.insert(actions_.end(), p, p + n);
    records_.push_back(rec);
}

void Replay::push_garrison_wishes(Tick t, Side s, const std::uint16_t* p,
                                  std::size_t n) {
    if (n > 0xFFFFu) throw ContractError("一次提交的登墙意愿数超过 65535");
    ReplayRecord rec;
    rec.tick = t;
    rec.kind = ReplayRecordKind::GarrisonWishes;
    rec.side = s;
    rec.count = static_cast<std::uint16_t>(n);
    rec.offset = static_cast<std::uint32_t>(wishes_.size());
    wishes_.insert(wishes_.end(), p, p + n);
    records_.push_back(rec);
}

void Replay::push_hash(Tick t, std::uint64_t h) {
    ReplayRecord rec;
    rec.tick = t;
    rec.kind = ReplayRecordKind::Hash;
    rec.hash = h;
    records_.push_back(rec);
}

std::vector<unsigned char> Replay::to_bytes() const {
    // 头部主体先单独序列化，好把它的长度写进 `header_bytes`。
    // 那个长度的作用是让**读侧能自查**：解析完头部之后游标必须正好落在
    // 头部末尾，落不上就说明读写两侧对格式的理解不一致——
    // 而那种不一致的另一种表现是「把头部剩下的字节当成第一条记录」，
    // 解析出一堆看似合法的垃圾。
    std::vector<unsigned char> head;
    put_str(head, platform_fp_);
    put_str(head, hash_tag_);
    put_str(head, map_id_);
    for (const unsigned char c : map_content_hash_) head.push_back(c);
    put_u64(head, stats_fp_);
    put_u64(head, seed_);
    put_i32(head, nominal_level_);
    put_u32(head, hash_period_);
    put_i32(head, total_ticks_);
    put_u32(head, static_cast<std::uint32_t>(records_.size()));
    put_u32(head, static_cast<std::uint32_t>(commands_.size()));
    put_u32(head, static_cast<std::uint32_t>(actions_.size()));
    put_u32(head, static_cast<std::uint32_t>(wishes_.size()));

    std::vector<unsigned char> b;
    for (const char c : kReplayMagic) b.push_back(static_cast<unsigned char>(c));
    put_u16(b, format_version_);
    put_u32(b, static_cast<std::uint32_t>(head.size()));
    b.insert(b.end(), head.begin(), head.end());

    for (const ReplayRecord& rec : records_) {
        put_i32(b, rec.tick);
        put_u8(b, static_cast<std::uint8_t>(rec.kind));
        put_u8(b, static_cast<std::uint8_t>(rec.side));
        put_u16(b, rec.count);
        put_u64(b, rec.hash);
    }
    for (const Command& c : commands_) put_command(b, c);
    for (const UnitAction& a : actions_) put_u8(b, static_cast<std::uint8_t>(a));
    for (const std::uint16_t w : wishes_) put_u16(b, w);

    put_u64(b, fnv_over(b.data(), b.size()));
    return b;
}

bool Replay::from_bytes(const unsigned char* data, std::size_t size, Replay* out,
                        std::string* err) {
    if (out == nullptr || err == nullptr) return false;
    *out = Replay{};
    err->clear();

    // 尾部 8 字节是全文校验和。先查它，于是**截断或损坏报「文件坏了」**，
    // 而不是在某个字段上报一个看起来像格式错的错。
    if (size < kReplayMagic.size() + 2 + 4 + 8) {
        *err = "回放文件太短，连文件头都装不下";
        return false;
    }
    Cursor tail{data, size, size - 8, true};
    const std::uint64_t want = tail.u64();
    const std::uint64_t got = fnv_over(data, size - 8);
    if (want != got) {
        *err = "回放文件校验和不符（文件被截断或损坏）：文件里是 " + hex64(want) +
               "，实算 " + hex64(got);
        return false;
    }

    Cursor c{data, size - 8, 0, true};   // 校验和不参与后续解析
    for (const char m : kReplayMagic) {
        if (c.u8() != static_cast<unsigned char>(m)) {
            *err = "不是回放文件（magic 不符）";
            return false;
        }
    }
    const std::uint16_t ver = c.u16();
    if (ver != kReplayFormatVersion) {
        *err = with_u64("回放格式版本不支持：文件是 ", ver) +
               with_u64("，本二进制只认 ", kReplayFormatVersion);
        return false;
    }
    const std::uint32_t header_bytes = c.u32();
    const std::size_t header_start = c.i;

    Replay r;
    r.format_version_ = ver;
    r.platform_fp_ = c.str();
    r.hash_tag_ = c.str();
    r.map_id_ = c.str();
    for (unsigned char& h : r.map_content_hash_) h = c.u8();
    r.stats_fp_ = c.u64();
    r.seed_ = c.u64();
    r.nominal_level_ = c.i32();
    r.hash_period_ = c.u32();
    r.total_ticks_ = c.i32();
    const std::uint32_t record_count = c.u32();
    const std::uint32_t cmd_count = c.u32();
    const std::uint32_t act_count = c.u32();
    const std::uint32_t wish_count = c.u32();

    if (!c.ok) {
        *err = "回放文件头读不完整";
        return false;
    }
    if (c.i != header_start + header_bytes) {
        *err = with_u64("回放文件头长度不符：头里声明 ", header_bytes) +
               with_u64("，实际解析了 ", c.i - header_start) +
               " —— 读写两侧对格式的理解不一致";
        return false;
    }
    if (r.hash_period_ == 0) {
        *err = "回放的 hash_period 为 0";
        return false;
    }
    if (r.total_ticks_ < 0) {
        *err = "回放的 total_ticks 为负";
        return false;
    }

    // 记录是定长 16 字节，所以这一条能在读第一条之前就发现截断。
    if (record_count > (c.n - c.i) / 16u) {
        *err = with_u64("回放声明 ", record_count) + " 条记录，但剩下的字节装不下";
        return false;
    }

    r.records_.reserve(record_count);
    Tick last_tick = 0;
    std::uint32_t seen_cmd = 0;
    std::uint32_t seen_act = 0;
    std::uint32_t seen_wish = 0;
    for (std::uint32_t k = 0; k < record_count; ++k) {
        ReplayRecord rec;
        rec.tick = c.i32();
        const std::uint8_t kind = c.u8();
        const std::uint8_t side = c.u8();
        rec.count = c.u16();
        rec.hash = c.u64();

        if (kind >= kReplayRecordKindCount) {
            *err = with_u64("回放里有非法的记录类型 ", kind);
            return false;
        }
        if (side >= kSideCount) {
            *err = with_u64("回放里有非法的 side ", side);
            return false;
        }
        rec.kind = static_cast<ReplayRecordKind>(kind);
        rec.side = static_cast<Side>(side);

        if (rec.tick < 0) {
            *err = "回放里有负的 tick";
            return false;
        }
        // **必须非递减**：录制天然是按时间顺序的，乱序只能来自写坏或人为拼接，
        // 而重放算法（「推进到该记录的 tick，再应用」）碰上倒退的 tick
        // 会静默地把它当成同 tick 处理。
        if (rec.tick < last_tick) {
            *err = "回放里的 tick 倒退了";
            return false;
        }
        last_tick = rec.tick;
        if (rec.tick > r.total_ticks_) {
            *err = "回放里有超出 total_ticks 的记录";
            return false;
        }

        switch (rec.kind) {
            case ReplayRecordKind::Commands:
                rec.offset = seen_cmd;
                seen_cmd += static_cast<std::uint32_t>(rec.count);
                break;
            case ReplayRecordKind::Actions:
                rec.offset = seen_act;
                seen_act += static_cast<std::uint32_t>(rec.count);
                break;
            case ReplayRecordKind::GarrisonWishes:
                rec.offset = seen_wish;
                seen_wish += static_cast<std::uint32_t>(rec.count);
                break;
            case ReplayRecordKind::Hash:
                if (rec.count != 0) {
                    *err = "Hash 记录的 count 必须为 0";
                    return false;
                }
                break;
        }
        r.records_.push_back(rec);
    }
    if (seen_cmd != cmd_count || seen_act != act_count || seen_wish != wish_count) {
        *err = "回放的载荷条数与记录里声明的总数不符";
        return false;
    }

    r.commands_.reserve(cmd_count);
    for (std::uint32_t k = 0; k < cmd_count; ++k) {
        Command cmd;
        cmd.slot = c.u16();
        const std::uint8_t kind = c.u8();
        const std::uint8_t side = c.u8();
        cmd.what = c.u8();
        cmd.level = c.u8();
        if (kind >= kCommandKindCount) {
            *err = with_u64("回放里有非法的 CommandKind ", kind);
            return false;
        }
        if (side >= kSideCount) {
            *err = with_u64("回放里有非法的命令 side ", side);
            return false;
        }
        cmd.kind = static_cast<CommandKind>(kind);
        cmd.side = static_cast<Side>(side);
        r.commands_.push_back(cmd);
    }

    r.actions_.reserve(act_count);
    for (std::uint32_t k = 0; k < act_count; ++k) {
        const std::uint8_t a = c.u8();
        if (a >= kUnitActionCount) {
            *err = with_u64("回放里有非法的 UnitAction ", a);
            return false;
        }
        r.actions_.push_back(static_cast<UnitAction>(a));
    }

    r.wishes_.reserve(wish_count);
    for (std::uint32_t k = 0; k < wish_count; ++k) {
        // 意愿是格线性下标，合法取值是 < 格数或 kNoSlot——格数要读地图才知道，
        // 这里只挡「既不是 kNoSlot 又顶着 uint16 上界之外」的不可能值……
        // uint16 没有「上界之外」，所以这里**刻意不查范围**：越界校验在
        // `World::submit_garrison_wishes`（重放应用时），那里才拿得到格数。
        r.wishes_.push_back(c.u16());
    }

    if (!c.ok) {
        *err = "回放文件读不完整";
        return false;
    }
    // 必须**正好**读完。多出来的字节说明格式对不上，而多出来的字节
    // 不会引起任何别的错误——这是唯一会报它的地方。
    if (c.i != c.n) {
        *err = with_u64("回放文件末尾有 ", c.n - c.i) + " 个多余字节";
        return false;
    }

    *out = std::move(r);
    return true;
}

bool Replay::save(std::string_view utf8_path) const {
    const std::vector<unsigned char> b = to_bytes();
    return write_file_bytes(utf8_path, b.data(), b.size());
}

bool Replay::load(std::string_view utf8_path, Replay* out, std::string* err) {
    if (out == nullptr || err == nullptr) return false;
    bool ok = false;
    const std::vector<unsigned char> b = read_file_bytes(utf8_path, &ok);
    if (!ok) {
        *out = Replay{};
        *err = "打不开回放文件：" + std::string(utf8_path);
        return false;
    }
    return from_bytes(b.data(), b.size(), out, err);
}

// ——ReplayRecorder——

ReplayRecorder::ReplayRecorder(World& w, std::uint32_t hash_period)
    : w_(&w), r_(Replay::begin(w, hash_period)) {
    // tick 0 的哈希点**一定要有**：它把「初始状态就不一样」（地图或 `WorldInit`
    // 给错了）与「跑着跑着歪了」分成两种诊断。少了它，前者会伪装成
    // 「第一个哈希点对不上」，而那个 tick 与真正的病因无关。
    r_.push_hash(w.now(), w.state_hash());
}

void ReplayRecorder::submit(Side side, const Command* cmds, std::size_t count) {
    // 先转发。`World::submit` 是全有或全无的，抛出即一条都没入队，
    // 所以**抛了就不该记**——记了会让重放时凭空多出一批命令。
    w_->submit(side, cmds, count);
    r_.push_commands(w_->now(), side, cmds, count);
}

void ReplayRecorder::submit_actions(Side side, const UnitAction* actions,
                                    std::size_t count) {
    w_->submit_actions(side, actions, count);
    r_.push_actions(w_->now(), side, actions, count);
}

void ReplayRecorder::submit_garrison_wishes(Side side, const std::uint16_t* targets,
                                            std::size_t count) {
    // 先转发后记录，理由同 `submit`：抛了就不该记。
    w_->submit_garrison_wishes(side, targets, count);
    r_.push_garrison_wishes(w_->now(), side, targets, count);
}

void ReplayRecorder::advance(int ticks) {
    if (ticks < 0) throw ContractError("advance 的 tick 数不得为负");
    for (int k = 0; k < ticks; ++k) {
        w_->advance(1);
        const Tick t = w_->now();
        if (static_cast<std::uint32_t>(t) % r_.hash_period() == 0) {
            r_.push_hash(t, w_->state_hash());
        }
    }
}

Replay ReplayRecorder::finish() {
    if (finished_) throw ContractError("ReplayRecorder::finish() 只能调用一次");
    finished_ = true;
    // 末尾一定记一个哈希点，除非最后一条记录已经是同一 tick 上的哈希点
    // （周期正好整除时会这样）。重复一条无害，但它会让「哈希点条数」
    // 这个数字对不上直觉，而那个数字要出现在报错信息里。
    const std::vector<ReplayRecord>& recs = r_.records();
    const bool dup = !recs.empty() && recs.back().kind == ReplayRecordKind::Hash &&
                     recs.back().tick == w_->now();
    if (!dup) r_.push_hash(w_->now(), w_->state_hash());
    r_.set_total_ticks(w_->now());
    return std::move(r_);
}

// ——replay_verify——

ReplayResult replay_verify(const Replay& r, World& fresh) {
    ReplayResult res;
    res.platform_matches = (r.platform_fingerprint() == kPlatformFingerprint);
    res.hash_tag_matches = (r.hash_tag() == kWorldHashTag);
    res.map_matches = (r.map_id() == fresh.map_id() &&
                       r.map_content_hash() == fresh.map_content_hash());
    res.stats_matches = (r.stats_fp() == fresh.stats_fingerprint());

    if (fresh.now() != 0) {
        res.verdict = ReplayVerdict::BadInput;
        res.message = "给 replay_verify 的世界不是刚建好的（now() != 0）";
        return res;
    }
    // **一条哈希点都没有的回放判失败，不判通过。**
    // 与确定性守卫「扫到 0 个文件即失败」同一条：一个什么都没检查的检查
    // 若报绿灯，它就变成了摆设，而摆设比缺席更危险——缺席看得见。
    if (r.hash_count() == 0) {
        res.verdict = ReplayVerdict::Vacuous;
        res.message = "这份回放里一条状态哈希都没有，验证不了任何东西";
        return res;
    }

    // ——三项头部核对，两项在这里就拦、一项留到分歧之后再说——
    //
    // 这个不对称是有理由的，因为三者的**契约不同**：
    //
    //   * `hash_tag` 的契约是「改了状态布局就改这个串」。所以它不同 ⇒
    //     两个二进制对「状态是什么」的定义都不一样，**比对本身没有意义**。
    //     这种情况下即便哈希碰巧相等也不构成证据，报 `Match` 就是在断言
    //     数据支撑不了的东西。所以当场拦。
    //   * 地图身份不同同理：那是调用方拿错了地图，不是仿真的发现。
    //     跑八个 tick 再报「第 0 tick 对不上」只是把一句清楚的话说成一句难懂的话。
    //   * `platform_fp` 的契约窄得多——它只管**浮点**可复现性。
    //     一个纯整数的仿真跨平台确实会给出同一个结果，所以那里的 `Match`
    //     是一句真话，硬拦会挡掉有用的比对。它因此只在真的对不上时才升格成成因。
    if (!res.hash_tag_matches) {
        res.verdict = ReplayVerdict::HashTagMismatch;
        res.message = "状态哈希的口径不同（回放是 " + r.hash_tag() +
                      " 录的，本二进制是 " + std::string(kWorldHashTag) +
                      "）：两边对「状态是什么」的定义都不一样，比对没有意义，重录。";
        return res;
    }
    if (!res.map_matches) {
        res.verdict = ReplayVerdict::MapMismatch;
        res.message = "地图身份不同（回放录在 \"" + r.map_id() +
                      "\"，给的世界是 \"" + fresh.map_id() +
                      "\"）——换对的地图，这不是仿真的问题。";
        return res;
    }
    if (!res.stats_matches) {
        res.verdict = ReplayVerdict::StatsMismatch;
        res.message = "数值表不同（回放录制时指纹 " + hex64(r.stats_fp()) +
                      "，给的世界是 " + hex64(fresh.stats_fingerprint()) +
                      "）——表在录完之后改过（或形状变了，见 kStatsShapeTag）。"
                      "两局跑在不同的规则下，比对没有意义，重录。这不是确定性缺陷。";
        return res;
    }

    Tick cur = 0;
    for (const ReplayRecord& rec : r.records()) {
        while (cur < rec.tick) {
            fresh.advance(1);
            ++cur;
        }
        switch (rec.kind) {
            case ReplayRecordKind::Commands:
            case ReplayRecordKind::Actions:
            case ReplayRecordKind::GarrisonWishes: {
                // 世界一旦分叉，这里就会抛——最典型的是动作数组长度不再等于
                // 活着的单位数。**必须接住**：让它逃出去，回放测试里最有价值的
                // 那一类失败就会表现为一个未捕获异常，而不是一句
                // 「第 N tick 分歧」。
                try {
                    if (rec.kind == ReplayRecordKind::Commands) {
                        fresh.submit(rec.side, r.commands().data() + rec.offset,
                                     rec.count);
                    } else if (rec.kind == ReplayRecordKind::Actions) {
                        fresh.submit_actions(rec.side, r.actions().data() + rec.offset,
                                             rec.count);
                    } else {
                        fresh.submit_garrison_wishes(
                            rec.side, r.wishes().data() + rec.offset, rec.count);
                    }
                } catch (const ContractError& e) {
                    res.verdict = res.platform_matches ? ReplayVerdict::Diverged
                                                       : ReplayVerdict::PlatformMismatch;
                    res.first_divergent_tick = rec.tick;
                    res.message = std::string("第 ") + std::to_string(rec.tick) +
                                  " tick 上回放里的" +
                                  std::string(ident_of(rec.kind)) +
                                  "应用不上，说明世界已经分叉：" + e.what();
                    return res;
                }
                break;
            }
            case ReplayRecordKind::Hash: {
                ++res.checked_hashes;
                const std::uint64_t actual = fresh.state_hash();
                if (actual != rec.hash) {
                    // 哈希口径与地图身份已在上面拦掉，所以到这里只剩两种成因：
                    // 换了平台（预期），或者真的跑歪了（要去查代码）。
                    res.verdict = res.platform_matches ? ReplayVerdict::Diverged
                                                       : ReplayVerdict::PlatformMismatch;
                    res.first_divergent_tick = rec.tick;
                    res.expected = rec.hash;
                    res.actual = actual;
                    res.message =
                        std::string("第 ") + std::to_string(rec.tick) +
                        " tick 状态不一致：期望 " + hex64(rec.hash) + "，实际 " +
                        hex64(actual);
                    if (res.verdict == ReplayVerdict::PlatformMismatch) {
                        res.message += "。平台指纹不同（回放录在 " +
                                       r.platform_fingerprint() + "，本二进制是 " +
                                       std::string(kPlatformFingerprint) +
                                       "）——回放在两套工具链之间不可复现"
                                       "（CLAUDE.md），换回录制平台再比。";
                    } else {
                        res.message += "。地图、数值表、哈希口径、平台指纹四项都一致，"
                                       "所以这是一个真的确定性缺陷。";
                    }
                    return res;
                }
                break;
            }
        }
    }
    // 流里最后一条记录之后可能还有 tick 要推（录制时 `advance` 完就 `finish` 了，
    // 那种情况下末尾哈希点正好在 `total_ticks` 上，这个循环不会转）。
    while (cur < r.total_ticks()) {
        fresh.advance(1);
        ++cur;
    }

    res.verdict = ReplayVerdict::Match;
    res.message = with_u64("逐 tick 一致，比对了 ",
                           static_cast<std::uint64_t>(res.checked_hashes)) +
                  with_u64(" 个哈希点，覆盖 ",
                           static_cast<std::uint64_t>(r.total_ticks())) +
                  " 个 tick";
    if (!res.platform_matches) {
        res.message += "。注意平台指纹不同（回放录在 " + r.platform_fingerprint() +
                       "，本二进制是 " + std::string(kPlatformFingerprint) +
                       "）：这次对上了，但不要据此建立跨平台的回放工作流"
                       "——本版仿真尚无浮点运算，加进来之后就不成立了。";
    }
    return res;
}

}  // namespace rts
