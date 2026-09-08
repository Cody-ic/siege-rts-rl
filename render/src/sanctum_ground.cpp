#include "render/sanctum_ground.hpp"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
namespace render {
namespace {
std::uint32_t grain(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
Texture2D material(bool stone) {
    constexpr int n=128;
    Image image=GenImageColor(n,n,WHITE);
    auto* pixels=static_cast<Color*>(image.data);
    for(int y=0;y<n;++y) for(int x=0;x<n;++x) {
        const auto hash=grain(static_cast<std::uint32_t>(y*n+x)+711u);
        const int noise=static_cast<int>(hash%19u)-9;
        const bool joint=(y%32<2 || (x+(y/32%2)*32)%64<2);
        const int base=stone?(joint?142:224):232;
        const auto v=static_cast<unsigned char>(std::clamp(base+noise,0,255));
        pixels[y*n+x]={v,v,v,255};
    }
    auto texture=LoadTextureFromImage(image);UnloadImage(image);
    SetTextureFilter(texture,TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(texture,TEXTURE_WRAP_REPEAT);
    return texture;
}
Color grass_color(float x,float y) {
    // Shared corner samples give continuous shading across adjacent diamonds.
    const float shade=std::sin(x*0.31f+y*0.19f)*7+std::cos(y*0.43f-x*0.16f)*5;
    return {static_cast<unsigned char>(91+shade),static_cast<unsigned char>(105+shade),
            static_cast<unsigned char>(80+shade*0.7f),255};
}
}
SanctumGround::~SanctumGround() {
    if(IsWindowReady()) {if(grass_.id) UnloadTexture(grass_);if(stone_.id) UnloadTexture(stone_);}
}
void SanctumGround::prepare(const std::vector<game::DrawItem>& tiles,
                            const std::vector<game::DrawItem>& objects) {
    width_=height_=0;
    for(const auto& t:tiles) {width_=std::max(width_,static_cast<int>(t.pos.i)+1);height_=std::max(height_,static_cast<int>(t.pos.j)+1);}
    if(width_<=0 || height_<=0) return;
    cells_.assign(static_cast<std::size_t>(width_*height_),{});
    std::vector<bool> walkable(cells_.size(),false),wall(cells_.size(),false);
    int keep=-1;std::vector<int> gates;
    const auto index=[&](rts::GridPos p) {return static_cast<int>(p.j)*width_+p.i;};
    for(const auto& t:tiles) {
        auto& c=cells_[static_cast<std::size_t>(index(t.pos))];
        c.surface=t.sprite.starts_with("Plain")?1:0;
        walkable[static_cast<std::size_t>(index(t.pos))]=c.surface==1;
        const float x=static_cast<float>(t.pos.i),y=static_cast<float>(t.pos.j);
        c.colors={grass_color(x-0.5f,y-0.5f),grass_color(x-0.5f,y+0.5f),
                  grass_color(x+0.5f,y+0.5f),grass_color(x+0.5f,y-0.5f)};
    }
    for(const auto& o:objects) {
        const int idx=index(o.pos);if(idx<0 || idx>=width_*height_) continue;
        if(o.sprite=="Keep") keep=idx;
        if(o.sprite=="Gate") gates.push_back(idx);
        if(o.sprite=="Wall" || o.sprite=="Gate") wall[static_cast<std::size_t>(idx)]=true;
        if(o.sprite=="Rock" || o.sprite=="Forest" || o.sprite=="Wall") walkable[static_cast<std::size_t>(idx)]=false;
    }
    if(keep>=0) {
        std::vector<int> parent(cells_.size(),-1);std::queue<int> todo;
        parent[static_cast<std::size_t>(keep)]=keep;todo.push(keep);
        while(!todo.empty()) {
            const int at=todo.front();todo.pop();
            const int x=at%width_,y=at/width_;
            const int dx[]={0,1,0,-1},dy[]={-1,0,1,0};
            for(int d=0;d<4;++d) {
                const int nx=x+dx[d],ny=y+dy[d];
                if(nx<0||ny<0||nx>=width_||ny>=height_) continue;
                const int n=ny*width_+nx;
                if(walkable[static_cast<std::size_t>(n)] && parent[static_cast<std::size_t>(n)]<0) {
                    parent[static_cast<std::size_t>(n)]=at;todo.push(n);
                }
            }
        }
        for(int gate:gates) {
            if(parent[static_cast<std::size_t>(gate)]<0) continue;
            for(int p=gate;p!=keep;p=parent[static_cast<std::size_t>(p)]) cells_[static_cast<std::size_t>(p)].surface=2;
        }
        // A paved court and an inner patrol walk, confined to existing plain cells.
        for(int y=0;y<height_;++y) for(int x=0;x<width_;++x) {
            const int at=y*width_+x;
            if(!walkable[static_cast<std::size_t>(at)]) continue;
            if(std::abs(x-keep%width_)<=2 && std::abs(y-keep/width_)<=2) cells_[static_cast<std::size_t>(at)].surface=3;
            const int tx=x+(x<keep%width_?-1:1),ty=y+(y<keep/width_?-1:1);
            if((tx>=0&&tx<width_&&wall[static_cast<std::size_t>(y*width_+tx)]) ||
               (ty>=0&&ty<height_&&wall[static_cast<std::size_t>(ty*width_+x)])) cells_[static_cast<std::size_t>(at)].surface=2;
        }
    }
    if(!grass_.id) grass_=material(false);
    if(!stone_.id) stone_=material(true);
}
bool SanctumGround::draw(const game::DrawItem& tile,const game::IsoProjection& proj) const {
    if(tile.pos.i<0||tile.pos.j<0||tile.pos.i>=width_||tile.pos.j>=height_) return false;
    const auto& cell=cells_[static_cast<std::size_t>(tile.pos.j*width_+tile.pos.i)];
    if(!cell.surface || !grass_.id) return false;
    const auto c=proj.grid_to_screen(tile.pos);
    const float w=static_cast<float>(proj.tile_w())*0.5f,h=static_cast<float>(proj.tile_h())*0.5f;
    const Vector2 points[]={{c.x,c.y-h},{c.x-w,c.y},{c.x,c.y+h},{c.x+w,c.y}};
    const Vector2 uv[]={{0,0},{0,1},{1,1},{1,0}};
    rlSetTexture(cell.surface>1?stone_.id:grass_.id);
    rlBegin(RL_QUADS);
    for(int i=0;i<4;++i) {
        const Color color=cell.surface==3?Color{182,172,143,255}:cell.surface==2?Color{147,143,123,255}:cell.colors[static_cast<std::size_t>(i)];
        rlColor4ub(color.r,color.g,color.b,color.a);rlTexCoord2f(uv[i].x,uv[i].y);rlVertex2f(points[i].x,points[i].y);
    }
    rlEnd();rlSetTexture(0);return true;
}
}
