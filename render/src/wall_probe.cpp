#include "rts/cli_args.hpp"
#include "render/scene_renderer.hpp"
#include "game/battle_scene.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/utf8_path.hpp"
#include <string>

// Live construction on cells absent from the map's initial wall list.
int main(int argc, char** argv) {
    const auto args = rts::utf8_args(argc, argv);
    if (argc != 5) return 2;
    const auto map = game::MapLoader::from_file(args[1].c_str());
    rts::WorldInit init;
    init.width = map.width();
    init.height = map.height();
    init.terrain.assign(static_cast<std::size_t>(init.width * init.height), rts::Terrain::Plain);
    init.keep = {0, 0};
    init.stats = game::StatsLoader::from_file(args[2].c_str());
    init.buildings = {{rts::BldType::Keep, {0, 0}, 10000, 10000}};
    rts::World world(init);
    for (int x = 8; x <= 13; ++x) {
        if (map.wall_at(x, 6)) return 3;
        world.place_bld(x == 10 ? rts::BldType::Gate : rts::BldType::Wall,
                       {static_cast<std::int16_t>(x), 6}, 100, 100, x < 9 ? 20 : 0);
    }
    world.place_bld(rts::BldType::Wall, {13, 7}, 100, 100);
    world.place_bld(rts::BldType::Wall, {13, 8}, 100, 100);
    for (int x = 8; x <= 11; ++x) {
        world.place_bld(rts::BldType::Fence,
                        {static_cast<std::int16_t>(x), 9}, 100, 100);
    }
    world.place_bld(rts::BldType::Fence, {11, 10}, 100, 100);
    world.place_bld(rts::BldType::Fence, {11, 11}, 100, 100);
    const auto before = world.state_hash();
    const auto items = game::BattleScene::sorted(map, world.view(rts::Side::Defender), world.now());
    for (const auto& item : items) {
        if (item.pos.j == 6 && item.pos.i >= 8 && item.pos.i <= 12 &&
            (item.sprite == "Wall" || item.sprite == "Gate") && item.facing != game::Facing::NE) return 4;
    }
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(1200, 800, "live wall regression");
    if (!IsWindowReady()) return 5;
    int result = 0;
    {
        render::SpriteAtlas atlas(args[3].c_str());
        game::IsoProjection proj(atlas.px_per_tile());
        render::SceneRenderer renderer(atlas, proj);
        const auto tiles = game::BattleScene::tiles(map);
        renderer.preload(tiles, items);
        const auto target = proj.grid_to_screen({10, 6});
        const Camera2D camera{{600, 440}, {target.x, target.y}, 0, 0.65f};
        auto texture = LoadRenderTexture(1200, 800);
        renderer.set_screen_scale(1 / camera.zoom);
        BeginTextureMode(texture);
        ClearBackground(Color{24, 33, 35, 255});
        BeginMode2D(camera);
        renderer.draw_ground(tiles);
        renderer.draw_objects(items);
        EndMode2D();
        EndTextureMode();
        auto image = LoadImageFromTexture(texture.texture);
        ImageFlipVertical(&image);
        int length = 0;
        auto* bytes = ExportImageToMemory(image, ".png", &length);
        if (!bytes || length <= 0 || !rts::write_file_bytes(args[4].c_str(), bytes, static_cast<std::size_t>(length))) result = 6;
        if (bytes) MemFree(bytes);
        UnloadImage(image);
        UnloadRenderTexture(texture);
    }
    CloseWindow();
    if (world.state_hash() != before) return 7;
    return result;
}
