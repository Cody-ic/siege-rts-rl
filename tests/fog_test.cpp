// 迷雾 / 侦查记忆图。
//
// 这一批的核心只有一句：**「从未见过」与「见过、那里是个缺口」必须在每一层
// 都是两回事**——在 `Vis` 上、在观测取值上、在状态哈希上。
// 它们撞在一起的时候不会有任何东西报错，而核心评估指标
// 「AI 是否发现并利用已有缺口」恰好要的就是这两者的区别。

#include <catch2/catch_test_macros.hpp>

#include "rts/fog.hpp"
#include "rts/hash.hpp"

namespace {

std::uint64_t hash_of(const rts::FogLayer& f) {
    rts::StateHash h;
    f.feed_hash(h);
    return h.value();
}

}  // namespace

TEST_CASE("初始全是未探索", "[fog]") {
    const rts::FogLayer f(4, 3);
    REQUIRE(f.width() == 4);
    REQUIRE(f.height() == 3);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            REQUIRE(f.at(x, y) == rts::Vis::Unseen);
            REQUIRE(f.last_seen(x, y) == rts::kNeverSeen);
            REQUIRE_FALSE(f.remembered_bld(x, y, nullptr));
        }
    }
    // Unseen 必须是 0：零初始化的迷雾 = 全图未探索。
    REQUIRE(static_cast<int>(rts::Vis::Unseen) == 0);
}

TEST_CASE("kNeverSeen 不能是 0 或 -1 之外的合法 tick", "[fog]") {
    // 0 是一个合法的 tick（开局那一刻）。拿它当哨兵，`now - last_seen`
    // 会算出一个看起来合理的「刚刚见过」。
    REQUIRE(rts::kNeverSeen < 0);
    REQUIRE(rts::kNeverSeen != 0);
    REQUIRE(rts::kNeverSeen != -1);
}

TEST_CASE("visible 通道是三档且互不相同", "[fog]") {
    REQUIRE(rts::vis_value(rts::Vis::Unseen) != rts::vis_value(rts::Vis::Remembered));
    REQUIRE(rts::vis_value(rts::Vis::Remembered) != rts::vis_value(rts::Vis::Visible));
    REQUIRE(rts::vis_value(rts::Vis::Unseen) != rts::vis_value(rts::Vis::Visible));
    // 单调递增 = 「情报新鲜度」，网络能直接用这个序。
    REQUIRE(rts::vis_value(rts::Vis::Unseen) < rts::vis_value(rts::Vis::Remembered));
    REQUIRE(rts::vis_value(rts::Vis::Remembered) < rts::vis_value(rts::Vis::Visible));
    // 落在 [0, 1]——`obs.hpp` 把这条通道标为 Frac01。
    REQUIRE(rts::vis_value(rts::Vis::Unseen) == 0.0f);
    REQUIRE(rts::vis_value(rts::Vis::Visible) == 1.0f);
}

TEST_CASE("begin_tick 把可见降为记忆，而不是降为未探索", "[fog]") {
    rts::FogLayer f(3, 1);
    f.mark_visible(1, 0, 42);
    REQUIRE(f.at(1, 0) == rts::Vis::Visible);
    REQUIRE(f.last_seen(1, 0) == 42);

    f.begin_tick();
    // **这一条是三态的全部意义。** 降成 Unseen 等于每 tick 把记忆擦掉，
    // 双视图演示与「缺口利用」指标同时失效。
    REQUIRE(f.at(1, 0) == rts::Vis::Remembered);
    REQUIRE(f.last_seen(1, 0) == 42);   // 记忆有多旧，靠它

    // 没见过的格子不受影响。
    REQUIRE(f.at(0, 0) == rts::Vis::Unseen);

    // 再降一次仍是 Remembered，不会继续退化。
    f.begin_tick();
    REQUIRE(f.at(1, 0) == rts::Vis::Remembered);
}

TEST_CASE("记忆里的缺口不等于从未见过", "[fog]") {
    rts::FogLayer f(2, 1);

    // (0,0)：从未见过。
    // (1,0)：见过，而且看到那里没有建筑——这就是「缺口」。
    f.mark_visible(1, 0, 7);
    f.remember_no_bld(1, 0);
    f.begin_tick();

    REQUIRE(f.at(0, 0) == rts::Vis::Unseen);
    REQUIRE(f.at(1, 0) == rts::Vis::Remembered);

    // `remembered_bld` 两处都返回 false——**光看它区分不出来**，
    // 这正是 `at()` 必须一起看的理由，头文件里写明了判缺口的写法。
    REQUIRE_FALSE(f.remembered_bld(0, 0, nullptr));
    REQUIRE_FALSE(f.remembered_bld(1, 0, nullptr));

    // 而观测取值不同：0 vs 0.5。若 `visible` 退回布尔，这两行会相等。
    REQUIRE(rts::vis_value(f.at(0, 0)) != rts::vis_value(f.at(1, 0)));
}

TEST_CASE("记忆里的建筑跨 tick 留存", "[fog]") {
    rts::FogLayer f(2, 1);
    f.mark_visible(0, 0, 10);
    f.remember_bld(0, 0, rts::BldType::Gate, 400);

    rts::RememberedBld got{};
    REQUIRE(f.remembered_bld(0, 0, &got));
    REQUIRE(got.type == rts::BldType::Gate);
    REQUIRE(got.hp_permille == 400);

    // 视野离开之后记忆还在——这才叫记忆图。
    f.begin_tick();
    REQUIRE(f.at(0, 0) == rts::Vis::Remembered);
    rts::RememberedBld still{};
    REQUIRE(f.remembered_bld(0, 0, &still));
    REQUIRE(still.type == rts::BldType::Gate);
    REQUIRE(still.hp_permille == 400);
}

TEST_CASE("重新看见时记忆被刷新，包括「墙不见了」", "[fog]") {
    // 这是「AI 的记忆图里那段墙是破的 → 它调头去打那里」的完整链路：
    // 先记住一段满血墙，再回来看见它已经没了。
    rts::FogLayer f(1, 1);
    f.mark_visible(0, 0, 1);
    f.remember_bld(0, 0, rts::BldType::Wall, rts::kFullPermille);
    f.begin_tick();

    f.mark_visible(0, 0, 99);
    f.remember_no_bld(0, 0);
    f.begin_tick();

    REQUIRE(f.at(0, 0) == rts::Vis::Remembered);
    REQUIRE_FALSE(f.remembered_bld(0, 0, nullptr));
    REQUIRE(f.last_seen(0, 0) == 99);
}

TEST_CASE("哈希区分「记忆里没建筑」与「记忆里有一段 0 血建筑」", "[fog]") {
    // 两者除了 has_bld 那一张表以外字节完全相同。漏掉那张表，
    // 缺口与「有一段血量为 0 的墙」就再次撞在一起——而后者在 1c 加入
    // 「废墟/地基」之后会是一个真实存在的状态。
    rts::FogLayer a(1, 1);
    a.mark_visible(0, 0, 5);
    a.remember_no_bld(0, 0);

    rts::FogLayer b(1, 1);
    b.mark_visible(0, 0, 5);
    b.remember_bld(0, 0, rts::BldType::Wall, 0);

    REQUIRE(hash_of(a) != hash_of(b));
}

TEST_CASE("哈希对可见性与 last_seen 都敏感", "[fog]") {
    rts::FogLayer base(2, 1);
    const std::uint64_t h0 = hash_of(base);

    rts::FogLayer seen(2, 1);
    seen.mark_visible(0, 0, 3);
    REQUIRE(hash_of(seen) != h0);

    // 同一格、同一态，但 last_seen 不同 ⇒ 哈希不同。
    // 记忆有多旧是状态的一部分（双视图要画它，将来的新鲜度通道要读它）。
    rts::FogLayer later(2, 1);
    later.mark_visible(0, 0, 4);
    REQUIRE(hash_of(later) != hash_of(seen));

    // 位置不同 ⇒ 哈希不同（顺序敏感，FNV-1a 被选中的唯一理由）。
    rts::FogLayer other(2, 1);
    other.mark_visible(1, 0, 3);
    REQUIRE(hash_of(other) != hash_of(seen));
}

TEST_CASE("同样的操作序列给同样的哈希", "[fog]") {
    // 哈希若不可复现，它就不能用来比对回放。
    auto build = []() {
        rts::FogLayer f(3, 2);
        f.mark_visible(1, 1, 8);
        f.remember_bld(1, 1, rts::BldType::Tower, 250);
        f.begin_tick();
        f.mark_visible(2, 0, 9);
        f.remember_no_bld(2, 0);
        return f;
    };
    REQUIRE(hash_of(build()) == hash_of(build()));
}

TEST_CASE("vis_bytes 给出的是三档原值，不是布尔", "[fog]") {
    rts::FogLayer f(3, 1);
    f.mark_visible(0, 0, 1);   // Visible
    f.mark_visible(1, 0, 1);
    f.begin_tick();            // (0,0) 与 (1,0) 变 Remembered
    f.mark_visible(1, 0, 2);   // (1,0) 回到 Visible

    const std::uint8_t* p = f.vis_bytes();
    REQUIRE(f.cell_count() == 3);
    REQUIRE(p[0] == static_cast<std::uint8_t>(rts::Vis::Remembered));
    REQUIRE(p[1] == static_cast<std::uint8_t>(rts::Vis::Visible));
    REQUIRE(p[2] == static_cast<std::uint8_t>(rts::Vis::Unseen));
    // 三个值互不相同——打包器写 `!= 0` 就会把前两个压成同一个。
    REQUIRE(p[0] != p[1]);
    REQUIRE(p[0] != p[2]);
}

TEST_CASE("Vis 标识符唯一非空", "[fog]") {
    REQUIRE(rts::ident_of(rts::Vis::Unseen) == "Unseen");
    REQUIRE(rts::ident_of(rts::Vis::Remembered) == "Remembered");
    REQUIRE(rts::ident_of(rts::Vis::Visible) == "Visible");
    REQUIRE(rts::kVisCount == 3);
}
