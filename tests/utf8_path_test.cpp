#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <fstream>
#include <string>

#include "game/map_loader.hpp"
#include "rts/utf8_path.hpp"

#ifndef GAME_TESTDATA_DIR
#error "GAME_TESTDATA_DIR 未定义，见 tests/CMakeLists.txt"
#endif
#ifndef GAME_TESTDATA_UTF8_DIR
#error "GAME_TESTDATA_UTF8_DIR 未定义，见 tests/CMakeLists.txt"
#endif

// 这个文件钉的是一个**真实发生过**的缺陷，而不是一个假想的边界情况：
//
// 队友把仓库放在 `.../计算机程序设计大作业/`，于是他本地 4 条 ctest 全红，
// 而同一份代码在纯 ASCII 路径下全绿。症状是「打不开这个文件」而那个文件明明就在
// ——最不容易联想到编码的一种报错。病因见 `rts/utf8_path.hpp`。
//
// **路径来自编译期宏，与真实链路一致**：CMake 写出 UTF-8 字节 → 宏 → C++ 打开文件。
// 若改成在测试里拼一个中文路径，就少验了「CMake 那一侧给的到底是什么编码」。

TEST_CASE("含非 ASCII 字符的路径能打开文件", "[utf8path]") {
    const std::string dir = GAME_TESTDATA_UTF8_DIR;
    const std::string file = dir + "/fixture_min.json";

    SECTION("宏里真的带了非 ASCII 字节——否则这条测试什么都没测") {
        // 这一段不是仪式。若 CMake 那侧的目录名被转义、或某天有人把中文去掉，
        // 下面几条会在**纯 ASCII 路径**上全绿通过，于是这个文件变成一条永远绿的测试
        // ——正是这个仓库反复要防的那种。
        bool has_non_ascii = false;
        for (char c : dir) {
            if (static_cast<unsigned char>(c) >= 0x80) { has_non_ascii = true; break; }
        }
        INFO("GAME_TESTDATA_UTF8_DIR = " << dir);
        REQUIRE(has_non_ascii);
    }

    SECTION("path_from_utf8 + ifstream 能打开") {
        std::ifstream in(rts::path_from_utf8(file), std::ios::binary);
        REQUIRE(in.good());
    }

    SECTION("read_file_bytes 读到了非空内容") {
        bool ok = false;
        const auto bytes = rts::read_file_bytes(file, &ok);
        REQUIRE(ok);
        REQUIRE(bytes.size() > 100);
        REQUIRE(bytes.front() == static_cast<unsigned char>('{'));
    }

    SECTION("MapLoader::from_file 走得通——这是队友那 4 条测试红的地方") {
        const game::MapData map = game::MapLoader::from_file(file);
        REQUIRE(map.map_id() == "fixture_min");
        REQUIRE(map.width() == 7);
        REQUIRE(map.height() == 5);
    }

    SECTION("write_file_bytes 也能写进中文目录") {
        // 截图导出与将来的回放文件都要**写**，而 raylib 的写入同样走窄 fopen。
        const std::string out = dir + "/写入探针.bin";
        const unsigned char payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
        REQUIRE(rts::write_file_bytes(out, payload, sizeof(payload)));

        bool ok = false;
        const auto back = rts::read_file_bytes(out, &ok);
        REQUIRE(ok);
        REQUIRE(back.size() == sizeof(payload));
        REQUIRE(back[0] == 0xDE);
        REQUIRE(back[3] == 0xEF);
    }

    SECTION("打不开时报错里的路径不是乱码") {
        // 报错信息是这类问题**唯一**的线索。若把 path 用 MSVC 的窄 `.string()`
        // 回显，中文会按 ANSI 代码页转、变成乱码，等于把线索也弄没了。
        const std::string missing = dir + "/没有这个文件.json";
        REQUIRE_THROWS_AS(game::MapLoader::from_file(missing), game::MapFormatError);

        const std::string round_trip = rts::utf8_from_path(rts::path_from_utf8(missing));
        REQUIRE(round_trip == missing);
    }
}

TEST_CASE("纯 ASCII 路径不受影响", "[utf8path]") {
    // 上面那些改动**不能**把原本能用的情形弄坏。这条与 [map] 那组重叠，
    // 但重叠是有意的：它验的是同一份文件经**新的**打开路径仍然读得出来。
    const std::string file = std::string(GAME_TESTDATA_DIR) + "/fixture_min.json";
    const game::MapData map = game::MapLoader::from_file(file);
    REQUIRE(map.map_id() == "fixture_min");

    bool ok = false;
    const auto bytes = rts::read_file_bytes(file, &ok);
    REQUIRE(ok);
    REQUIRE(!bytes.empty());
}

TEST_CASE("读不存在的文件要失败，而不是给一个空的成功", "[utf8path]") {
    // `read_file_bytes` 的 `*ok` 若在失败时被漏掉，调用方拿到的是一个空 vector，
    // 而空 vector 在 `LoadImageFromMemory` 那侧的表现是「解不开这张图」
    // ——一个指向错误方向的报错。
    bool ok = true;
    const auto bytes = rts::read_file_bytes(
        std::string(GAME_TESTDATA_DIR) + "/根本没有这个文件.dat", &ok);
    REQUIRE_FALSE(ok);
    REQUIRE(bytes.empty());
}
