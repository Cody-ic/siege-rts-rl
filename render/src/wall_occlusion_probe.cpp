#include "rts/cli_args.hpp"
#include "rts/utf8_path.hpp"
#include "game/battle_scene.hpp"
#include "game/map_loader.hpp"
#include "game/stats_loader.hpp"
#include "render/scene_renderer.hpp"
#include <string>
#include <algorithm>
#include <iostream>
#include <cstring>

namespace {
bool occlusion_contract(render::SceneRenderer& renderer,game::IsoProjection proj,bool legacy) {
    auto target=LoadRenderTexture(640,640);
    const auto at=proj.grid_to_screen({8,8});
    const Camera2D camera{{320,440},{at.x,at.y},0,1};
    renderer.set_presentation(false,0);
    bool ok=true;
    for(auto ident:{"Wall","Gate","Fence"}) for(auto facing:{game::Facing::NE,game::Facing::SE}) {
        for(bool front:{false,true}) for(auto state:{"idle","move"}) {
            game::DrawItem wall{{8,8},ident,facing};
            game::DrawItem worker;worker.sprite="Mason";worker.continuous=true;worker.state=state;
            worker.world=front?rts::Vec2{7.95f,9.02f}:rts::Vec2{9.05f,7.98f};
            if(facing==game::Facing::SE) std::swap(worker.world.x,worker.world.y);
            const auto render=[&](bool oracle) {
                BeginTextureMode(target);ClearBackground({24,33,35,255});BeginMode2D(camera);
                if(oracle) {
                    renderer.draw_objects({front?wall:worker});
                    renderer.draw_objects({front?worker:wall});
                } else {
                    game::DrawLists lists;lists.sorted={wall,worker};
                    std::stable_sort(lists.sorted.begin(),lists.sorted.end(),[](const auto& a,const auto& b){return a.depth_f()<b.depth_f();});
                    if(legacy) for(const auto& item:lists.sorted) renderer.draw_objects({item});
                    else renderer.draw(lists);
                }
                EndMode2D();EndTextureMode();return LoadImageFromTexture(target.texture);
            };
            auto actual=render(false),expected=render(true);
            auto* a=LoadImageColors(actual);auto* b=LoadImageColors(expected);
            int mismatches=0;
            for(int i=0;i<640*640;++i) if(std::memcmp(&a[i],&b[i],sizeof(Color))!=0) ++mismatches;
            if(mismatches) {ok=false;std::cerr<<ident<<" "<<game::to_string(facing)<<" front="<<front<<" "<<state<<" mismatches="<<mismatches<<"\n";}
            UnloadImageColors(a);UnloadImageColors(b);UnloadImage(actual);UnloadImage(expected);
        }
    }
    renderer.set_presentation(true,0);UnloadRenderTexture(target);
    return ok;
}
}

// Read-only visual regression: units stay on passable cells around both wall axes.
int main(int argc, char** argv) {
    const auto args=rts::utf8_args(argc,argv);
    if(argc!=5 && !(argc==6 && args[5]=="--legacy-order")) return 2;
    const auto map=game::MapLoader::from_file(args[1]);
    const auto stats=game::StatsLoader::from_file(args[2]);
    SetTraceLogLevel(LOG_WARNING);SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(1600,1200,"wall occlusion probe");
    if(!IsWindowReady()) return 3;
    int result=0;
    {
        render::SpriteAtlas atlas(args[3]);game::IsoProjection proj(atlas.px_per_tile());
        render::SceneRenderer renderer(atlas,proj);
        if(!occlusion_contract(renderer,proj,argc==6)) result=6;
        const auto tiles=game::BattleScene::tiles(map);
        auto texture=LoadRenderTexture(1600,1200);
        BeginTextureMode(texture);ClearBackground({24,33,35,255});
        for(int panel=0;panel<4;++panel) {
            rts::WorldInit init;init.width=map.width();init.height=map.height();
            init.terrain.assign(static_cast<std::size_t>(init.width*init.height),rts::Terrain::Plain);
            init.keep={0,0};init.stats=stats;
            init.buildings={{rts::BldType::Keep,{0,0},10000,10000}};
            rts::World world(init);
            for(int k=5;k<=11;++k) {
                const rts::GridPos p=panel==1?rts::GridPos{8,static_cast<std::int16_t>(k)}
                    :rts::GridPos{static_cast<std::int16_t>(k),8};
                world.place_bld(panel==3&&k==8?rts::BldType::Gate:rts::BldType::Wall,p,100,100);
            }
            if(panel==2) for(int k=5;k<8;++k)
                world.place_bld(rts::BldType::Wall,{5,static_cast<std::int16_t>(k)},100,100);
            for(float along:{5.05f,7.05f,9.95f,11.95f}) for(float across:{7.98f,9.02f}) {
                const rts::Vec2 p=panel==1?rts::Vec2{across,along}:rts::Vec2{along,across};
                // Skip the occupied side of the corner; never hide simulation penetration.
                if(world.bld_at(rts::grid_of(p)).valid()) continue;
                world.spawn_unit(rts::UnitType::Mason,p,1,100,100);
            }
            if(panel==3) world.spawn_unit(rts::UnitType::Mason,{8.5f,8.55f},1,100,100);
            const auto before=world.state_hash();
            const auto items=game::BattleScene::sorted(map,world.view(rts::Side::Defender),0);
            const int left=(panel%2)*800,top=(panel/2)*600;
            const auto target=proj.grid_to_screen({8,8});
            const Camera2D camera{{static_cast<float>(left+400),static_cast<float>(top+380)},
                                  {target.x,target.y},0,0.62f};
            renderer.set_screen_scale(1/camera.zoom);
            BeginScissorMode(left,top,800,600);BeginMode2D(camera);
            renderer.draw(tiles,items);
            for(const auto& p:world.view(rts::Side::Defender).unit_pos()) {
                const auto q=proj.world_to_screen(p);
                DrawCircleV({q.x,q.y},3,RED);
            }
            EndMode2D();
            const char* labels[]={"Wall along X", "Wall along Y", "Corner", "Gate passage"};
            DrawText(labels[panel],left+20,top+20,24,RAYWHITE);EndScissorMode();
            if(world.state_hash()!=before) result=4;
        }
        EndTextureMode();
        auto image=LoadImageFromTexture(texture.texture);ImageFlipVertical(&image);
        int length=0;auto* bytes=ExportImageToMemory(image,".png",&length);
        if(!bytes || length<=0 || !rts::write_file_bytes(args[4],bytes,static_cast<std::size_t>(length))) result=5;
        if(bytes) MemFree(bytes);UnloadImage(image);UnloadRenderTexture(texture);
    }
    CloseWindow();return result;
}
