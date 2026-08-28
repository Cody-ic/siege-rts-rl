#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <type_traits>

#include "rts/types.hpp"

TEST_CASE("句柄的索引与代数往返", "[types]") {
    for (std::uint16_t index : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{40},
                               std::uint16_t{1000}, rts::UnitId::kMaxIndex}) {
        for (std::uint16_t gen : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0xFFFF}}) {
            const auto h = rts::UnitId::make(index, gen);
            REQUIRE(h.index() == index);
            REQUIRE(h.generation() == gen);
            REQUIRE(h.valid());
        }
    }
}

TEST_CASE("默认构造的句柄无效", "[types]") {
    // 默认无效很关键：数组里未使用的槽位、以及"没有目标"这个状态都靠它表示。
    // 若默认是 index 0，未初始化的句柄会静默指向第一个实体。
    const rts::UnitId h;
    REQUIRE_FALSE(h.valid());
    REQUIRE(h.raw() == rts::UnitId::kInvalidRaw);
    REQUIRE(h == rts::UnitId{});
}

TEST_CASE("代数使回收槽位上的旧句柄失配", "[types]") {
    // 这是代数存在的全部理由：同一个槽位被回收再分配之后，旧句柄必须不再相等，
    // 于是悬空引用变成一次可检出的失败，而不是静默读到另一个实体。
    const auto before = rts::UnitId::make(7, 1);
    const auto after = rts::UnitId::make(7, 2);
    REQUIRE(before != after);
    REQUIRE(before.index() == after.index());
}

TEST_CASE("句柄的序是索引优先", "[types][determinism]") {
    // 索引放在高 16 位，因此 raw 的大小顺序等于索引顺序。
    // 按句柄排序 == 按数组下标排序，代数变化不会让顺序重排——
    // 排序稳定性直接关系到遍历顺序，而遍历顺序关系到回放能否复现。
    REQUIRE(rts::UnitId::make(1, 0) < rts::UnitId::make(2, 0));
    REQUIRE(rts::UnitId::make(1, 0xFFFF) < rts::UnitId::make(2, 0));
    REQUIRE(rts::UnitId::make(5, 1) < rts::UnitId::make(5, 2));

    // 能安全用作有序容器的 key（顺序由整数决定，与地址无关）。
    std::map<rts::UnitId, int> byid;
    byid[rts::UnitId::make(3, 1)] = 30;
    byid[rts::UnitId::make(1, 1)] = 10;
    byid[rts::UnitId::make(2, 1)] = 20;
    REQUIRE(byid.begin()->first.index() == 1);
}

TEST_CASE("阵营的辅助函数", "[types]") {
    REQUIRE(rts::other(rts::Side::Defender) == rts::Side::Attacker);
    REQUIRE(rts::other(rts::Side::Attacker) == rts::Side::Defender);
    REQUIRE(rts::index_of(rts::Side::Defender) == 0);
    REQUIRE(rts::index_of(rts::Side::Attacker) == 1);
    REQUIRE(rts::kSideCount == 2);
}

TEST_CASE("基元类型的布局约束", "[types][determinism]") {
    // 布局一变，旧回放文件就会静默偏离。把它固定成编译期断言，
    // 使"改了布局"必须是一个显式动作。
    STATIC_REQUIRE(sizeof(rts::Tick) == 4);
    STATIC_REQUIRE(sizeof(rts::Vec2) == 8);
    STATIC_REQUIRE(sizeof(rts::GridPos) == 4);
    STATIC_REQUIRE(sizeof(rts::UnitId) == 4);
    STATIC_REQUIRE(sizeof(rts::BldId) == 4);

    // 句柄无填充字节，故可直接按字节喂给状态哈希（见 hash.hpp 的 feed_pod）。
    STATIC_REQUIRE(std::has_unique_object_representations_v<rts::UnitId>);
    STATIC_REQUIRE(std::has_unique_object_representations_v<rts::GridPos>);

    // 单位句柄与建筑句柄不可互换——两者是两组独立的扁平数组，传错会索引到错误的数组。
    STATIC_REQUIRE_FALSE(std::is_convertible_v<rts::UnitId, rts::BldId>);
    STATIC_REQUIRE_FALSE(std::is_convertible_v<rts::BldId, rts::UnitId>);
}

TEST_CASE("tick 频率是固定的 20 Hz", "[types]") {
    REQUIRE(rts::kTicksPerSecond == 20);
}
