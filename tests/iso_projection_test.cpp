#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

#include "game/iso_projection.hpp"

namespace {

rts::GridPos at(int x, int y) {
    return rts::GridPos{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
}

// `_sprite_meta.json` 里的实际值。写死在测试里是对的：这条测试要钉的就是
// 「在真实参数下这套换算成立」，而参数本身由 SpriteAtlas 校验（§7.1）。
constexpr int kPx = 256;

}  // namespace

TEST_CASE("grid_to_screen 与 preview_map.py 的公式一致", "[iso]") {
    const game::IsoProjection proj(kPx);
    REQUIRE(proj.tile_w() == 256);
    REQUIRE(proj.tile_h() == 128);

    // 对照值按 `preview_map.py` 的 `ox + (gi - gj) * TW // 2, oy + (gi + gj) * TH // 2`
    // 手算（ox = oy = 0）。**这几个点刻意选得不对称**：i 与 j 弄反的话
    // (1,0) 与 (0,1) 会互换，而对称的取样看不出来——同 map_loader_test 钉
    // `rows[y][x]` 用非方形夹具的理由。
    REQUIRE(proj.grid_to_screen(at(0, 0)).x == 0.0f);
    REQUIRE(proj.grid_to_screen(at(0, 0)).y == 0.0f);

    REQUIRE(proj.grid_to_screen(at(1, 0)).x == 128.0f);
    REQUIRE(proj.grid_to_screen(at(1, 0)).y == 64.0f);

    REQUIRE(proj.grid_to_screen(at(0, 1)).x == -128.0f);
    REQUIRE(proj.grid_to_screen(at(0, 1)).y == 64.0f);

    REQUIRE(proj.grid_to_screen(at(3, 1)).x == 256.0f);
    REQUIRE(proj.grid_to_screen(at(3, 1)).y == 256.0f);
}

// 反方向。**这是本文件存在的主要理由**——见 iso_projection.hpp 里「那个坑」。
TEST_CASE("screen_to_grid 是 grid_to_screen 的逆", "[iso]") {
    const game::IsoProjection proj(kPx);

    SECTION("格心往返恒等，扫一片含负坐标的范围") {
        for (int j = -3; j <= 40; ++j) {
            for (int i = -3; i <= 40; ++i) {
                const rts::GridPos p = at(i, j);
                const rts::GridPos back = proj.screen_to_grid(proj.grid_to_screen(p));
                REQUIRE(back.i == p.i);
                REQUIRE(back.j == p.j);
            }
        }
    }

    SECTION("格内任意一点都归到该格，不只是格心") {
        // 沿菱形的两条对角方向各取几个偏移，量都小于半格。
        // 这一条是 round / floor 之争的实际内容：用 floor 时**格心本身仍然对**，
        // 只有偏移之后才错——所以只测格心的测试会全绿地放过那个 bug。
        const float dx[] = {0.0f, 60.0f, -60.0f, 0.0f, 0.0f, 40.0f, -40.0f};
        const float dy[] = {0.0f, 0.0f, 0.0f, 30.0f, -30.0f, 20.0f, -20.0f};
        for (int j = 0; j <= 12; ++j) {
            for (int i = 0; i <= 12; ++i) {
                const rts::Vec2 c = proj.grid_to_screen(at(i, j));
                for (int k = 0; k < 7; ++k) {
                    const rts::GridPos got =
                        proj.screen_to_grid(rts::Vec2{c.x + dx[k], c.y + dy[k]});
                    REQUIRE(got.i == i);
                    REQUIRE(got.j == j);
                }
            }
        }
    }

    SECTION("用 floor 会得到什么——把那个坑本身钉住") {
        // 这一段不测被测代码，而是**证明「换成 floor 会错」不是空话**。
        // 没有它，上面那条 SECTION 只是「现在是对的」，读者无从知道它防的是什么，
        // 于是下一个人完全可能出于「取整不该用 round 吧」把它改回去。
        //
        // 取格 (5, 3) 的格心稍微往右一点的一个点：正确答案仍是 (5, 3)。
        const rts::Vec2 c = proj.grid_to_screen(at(5, 3));
        const rts::Vec2 probe{c.x + 20.0f, c.y - 10.0f};

        REQUIRE(proj.screen_to_grid(probe).i == 5);
        REQUIRE(proj.screen_to_grid(probe).j == 3);

        // 同一个点，若实现改成向下取整：
        const float tw = 256.0f;
        const float th = 128.0f;
        const float fi = probe.x / tw + probe.y / th;
        const float fj = probe.y / th - probe.x / tw;
        const int floor_i = static_cast<int>(std::floor(fi));
        const int floor_j = static_cast<int>(std::floor(fj));
        // 至少有一个轴会偏——而偏的方向是固定的（往左下），
        // 所以现象是「点选稳定差一格」，很容易被当成美术锚点没对齐。
        REQUIRE((floor_i != 5 || floor_j != 3));
    }

    SECTION("px_per_tile 为奇数时往返仍然成立") {
        // 整数除法会丢半个像素（`(i-j) * tile_w / 2`），而半个像素远小于半格，
        // 所以 round 吃得下。写这条是因为反过来的假设（「必须是偶数」）
        // 会诱使人加一条其实不需要的限制。
        //
        // 注意这**不**代表奇数没有代价：iso_projection.hpp 里记着 C++ 的截断
        // 与 Python 的 `//` 在奇数下会分岔，那影响的是与 preview_map.py 的逐像素对照，
        // 不是这里的往返。
        for (int px : {100, 101, 64}) {
            const game::IsoProjection odd(px);
            for (int j = 0; j <= 20; ++j) {
                for (int i = 0; i <= 20; ++i) {
                    const rts::GridPos back =
                        odd.screen_to_grid(odd.grid_to_screen(at(i, j)));
                    REQUIRE(back.i == i);
                    REQUIRE(back.j == j);
                }
            }
        }
    }

    SECTION("远在图外的点被夹住，不回绕") {
        // 回绕会把「鼠标在地图外很远处」变成「鼠标落在地图另一角的合法格上」，
        // 那是一个看起来像点选逻辑错的错。
        const rts::GridPos far = proj.screen_to_grid(rts::Vec2{1.0e9f, 1.0e9f});
        REQUIRE(far.i == 32767);
        REQUIRE(far.j >= 0);          // 不该变成负数
        const rts::GridPos neg = proj.screen_to_grid(rts::Vec2{-1.0e9f, -1.0e9f});
        REQUIRE(neg.i == -32768);
    }
}

TEST_CASE("grid_bounds 覆盖整张地图的格心", "[iso]") {
    const game::IsoProjection proj(kPx);

    SECTION("7×5 的夹具尺寸") {
        const game::Rect b = proj.grid_bounds(7, 5);
        // 左极点在 (0, 4)：x = (0-4) * 128 = -512
        // 右极点在 (6, 0)：x = (6-0) * 128 = 768
        REQUIRE(b.x == -512.0f);
        REQUIRE(b.width == 1280.0f);
        // 上极点在 (0, 0)：y = 0；下极点在 (6, 4)：y = 10 * 64 = 640
        REQUIRE(b.y == 0.0f);
        REQUIRE(b.height == 640.0f);
    }

    SECTION("每一格的格心都在包围盒里") {
        // 「极值必在角上」这条推理若错了，会有格子落在盒外，而现象是
        // 「F 重新入画之后边上一列看不见」——一个很容易归因给相机的错。
        const int w = 11;
        const int h = 7;
        const game::Rect b = proj.grid_bounds(w, h);
        for (int j = 0; j < h; ++j) {
            for (int i = 0; i < w; ++i) {
                const rts::Vec2 c = proj.grid_to_screen(at(i, j));
                REQUIRE(c.x >= b.x);
                REQUIRE(c.x <= b.x + b.width);
                REQUIRE(c.y >= b.y);
                REQUIRE(c.y <= b.y + b.height);
            }
        }
    }

    SECTION("1×1 的退化地图不产出负的宽高") {
        const game::Rect b = proj.grid_bounds(1, 1);
        REQUIRE(b.width >= 0.0f);
        REQUIRE(b.height >= 0.0f);
    }
}
