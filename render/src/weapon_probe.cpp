// 可重复的建筑射击画面：使用真实 World 与 SceneRenderer，输出前摇/释放/飞行帧。
#include "render/scene_renderer.hpp"
#include "game/battle_scene.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "rts/world_view.hpp"
#include "rts/utf8_path.hpp"
#include <algorithm>
#include <cstdio>
#include <string>

int main(int argc,char** argv) {
    if(argc!=5) return 2;
    const auto map=game::MapLoader::from_file(argv[1]);
    rts::WorldInit init;init.width=map.width();init.height=map.height();
    init.terrain.assign(static_cast<std::size_t>(init.width*init.height),rts::Terrain::Plain);
    init.keep={4,6};init.stats=game::StatsLoader::from_file(argv[2]);
    init.buildings={{rts::BldType::Keep,{4,6},10000,10000},
                    {rts::BldType::Flak,{5,6},10000,10000},
                    {rts::BldType::Tower,{8,6},10000,10000}};
    rts::World world(init);
    world.spawn_unit(rts::UnitType::Phoenix,{5.5f,2.5f},1,100000,100000);
    world.spawn_unit(rts::UnitType::Ghoul,{11.5f,6.5f},1,100000,100000);
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);InitWindow(1200,850,"weapon probe");
    if(!IsWindowReady()) return 3;
    int result=0;
    {
        render::SpriteAtlas atlas(argv[3]);game::IsoProjection proj(atlas.px_per_tile());
        render::SceneRenderer renderer(atlas,proj);
        const auto target=proj.grid_to_screen({7,5});
        Camera2D camera{{600,550},{target.x,target.y},0,0.55f};
        const auto tiles=game::BattleScene::tiles(map);
        auto texture=LoadRenderTexture(1200,850);
        const int fire=init.stats.of(rts::BldType::Flak).windup_ticks+1;
        const int ticks[]={1,fire,fire+3,fire+8};
        for(int tick:ticks) {
            world.advance(tick-world.now());
            const auto scene=game::BattleScene::sorted(map,world.view(rts::Side::Defender),world.now());
            BeginTextureMode(texture);ClearBackground(Color{32,37,39,255});BeginMode2D(camera);
            renderer.draw(tiles,scene);EndMode2D();
            DrawText(("Tower / Flak: tick "+std::to_string(tick)).c_str(),24,24,26,RAYWHITE);
            EndTextureMode();
            auto image=LoadImageFromTexture(texture.texture);ImageFlipVertical(&image);
            int length=0;auto* bytes=ExportImageToMemory(image,".png",&length);
            const auto file=std::string(argv[4])+"-"+std::to_string(tick)+".png";
            if(!bytes || length<=0 || !rts::write_file_bytes(file,bytes,static_cast<std::size_t>(length))) result=4;
            if(bytes) MemFree(bytes);UnloadImage(image);
        }
        UnloadRenderTexture(texture);
    }
    CloseWindow();return result;
}
