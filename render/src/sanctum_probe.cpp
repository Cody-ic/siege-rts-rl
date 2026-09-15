#include "rts/cli_args.hpp"
#include "render/battle_atmosphere.hpp"
#include "render/scene_renderer.hpp"
#include "game/battle_scene.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/utf8_path.hpp"
#include <cstdio>
#include <string>
int main(int argc,char** argv) {
    const auto args = rts::utf8_args(argc, argv);
    if(argc!=5) return 2;
    const auto map=game::MapLoader::from_file(args[1].c_str());
    rts::WorldInit init;init.width=map.width();init.height=map.height();
    init.terrain.assign(static_cast<std::size_t>(init.width*init.height),rts::Terrain::Plain);
    init.keep={4,6};init.stats=game::StatsLoader::from_file(args[2].c_str());
    init.buildings={{rts::BldType::Keep,{4,6},10000,10000},
        {rts::BldType::Flak,{5,6},10000,10000},{rts::BldType::Tower,{8,6},10000,10000},
        {rts::BldType::Wall,{8,4},90,90}};
    rts::World world(init);
    world.spawn_unit(rts::UnitType::Phoenix,{5.5f,2.5f},1,90,90);
    const auto ram=world.spawn_unit(rts::UnitType::Ram,{8.5f,3.5f},1,100000,100000);
    world.begin_assault();
    SetTraceLogLevel(LOG_WARNING);SetConfigFlags(FLAG_WINDOW_HIDDEN);InitWindow(1500,1000,"sanctum probe");
    if(!IsWindowReady()) return 3;
    int result=0;
    {
        render::SpriteAtlas atlas(args[3].c_str());game::IsoProjection proj(atlas.px_per_tile());
        render::SceneRenderer renderer(atlas,proj);render::BattleAtmosphere atmosphere;
        const auto target=proj.grid_to_screen({7,5});Camera2D camera{{750,650},{target.x,target.y},0,0.52f};
        const auto tiles=game::BattleScene::tiles(map);
        renderer.preload(tiles,game::BattleScene::sorted(map,world.view(rts::Side::Defender),world.now()));
        auto texture=LoadRenderTexture(1500,1000);
        atmosphere.observe(world.view(rts::Side::Defender),world.now(),world.wave());
        for(int tick=1;tick<=60;++tick) {
            std::vector<rts::UnitId> ids;world.enumerate_units(rts::Side::Attacker,ids);
            std::vector<rts::UnitAction> actions;
            for(auto id:ids) actions.push_back(id==ram?rts::UnitAction::AtkWall:rts::UnitAction::Stop);
            world.submit_actions(rts::Side::Attacker,actions.data(),actions.size());world.advance(1);
            const auto before=world.state_hash();
            atmosphere.observe(world.view(rts::Side::Defender),world.now(),world.wave());
            if(tick==7 || tick==32 || tick==48 || tick==60) {
                renderer.set_screen_scale(1/camera.zoom);renderer.set_presentation(true,static_cast<float>(tick)/20);
                BeginTextureMode(texture);ClearBackground(Color{24,33,35,255});BeginMode2D(camera);
                renderer.draw_ground(tiles);atmosphere.draw_ground(proj);
                renderer.draw_objects(game::BattleScene::sorted(map,world.view(rts::Side::Defender),world.now()));
                atmosphere.draw_world(proj,atlas,camera.zoom);EndMode2D();EndTextureMode();
                auto image=LoadImageFromTexture(texture.texture);ImageFlipVertical(&image);
                int length=0;auto* bytes=ExportImageToMemory(image,".png",&length);
                if(!bytes || length<=0 || !rts::write_file_bytes(std::string(args[4].c_str())+"-"+std::to_string(tick)+".png",bytes,static_cast<std::size_t>(length))) result=4;
                if(bytes) MemFree(bytes);UnloadImage(image);
            }
            if(world.state_hash()!=before) result=5;
        }
        if(world.view(rts::Side::Defender).bld_alive()[3]) result=6;
        UnloadRenderTexture(texture);
    }
    CloseWindow();return result;
}
