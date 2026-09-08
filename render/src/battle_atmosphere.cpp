#include "render/battle_atmosphere.hpp"
#include "render/scene_renderer.hpp"
#include <algorithm>
#include <cmath>
#include <string>
namespace render {
namespace {
void ellipse(Vector2 center,float rx,float ry,float thickness,Color color) {
    for(int i=0;i<64;++i) {
        const float a=static_cast<float>(i)*0.09817477f,b=a+0.09817477f;
        DrawLineEx({center.x+rx*std::cos(a),center.y+ry*std::sin(a)},
                   {center.x+rx*std::cos(b),center.y+ry*std::sin(b)},thickness,color);
    }
}
}
void BattleAtmosphere::reset() {previous_.clear();bursts_.clear();scars_.clear();feathers_.clear();signals_.reset();seconds_=0;wave_=0;wave_since_=0;}
void BattleAtmosphere::observe(const rts::WorldView& v,rts::Tick tick,int wave) {
    if(static_cast<float>(tick)/rts::kTicksPerSecond<seconds_) reset();
    seconds_=static_cast<float>(tick)/rts::kTicksPerSecond;
    signals_.observe(v);
    feathers_.erase(std::remove_if(feathers_.begin(),feathers_.end(),[&](const auto& f){return seconds_-f.born>3;}),feathers_.end());
    for(const auto& e:signals_.events()) if(e.cue==BattleCue::WingFall && feathers_.size()<16) feathers_.push_back({e.pos,seconds_});
    if(wave!=wave_) {wave_=wave;wave_since_=seconds_;}
    bursts_.erase(std::remove_if(bursts_.begin(),bursts_.end(),[&](const Burst& b){return seconds_-b.born>1.8f;}),bursts_.end());
    for(std::size_t k=0;k<v.bld_pos().size();++k) {
        Building next{v.bld_pos()[k],v.bld_type()[k],v.bld_hp()[k],v.bld_max_hp()[k],
                      v.bld_alive()[k]!=0,v.bld_level()[k],v.bld_upgrade_left()[k]>0,v.bld_built()[k]!=0};
        if(k<previous_.size()) {
            const auto& old=previous_[k];
            const auto p=next.alive?next.pos:old.pos;
            const bool visible=v.side()==rts::Side::Defender || (v.fog().in_bounds(p.i,p.j)&&v.fog().at(p.i,p.j)==rts::Vis::Visible);
            if(visible && old.alive && (!next.alive || (old.type==next.type && old.pos==next.pos && next.hp<old.hp)) && bursts_.size()<96) {
                bursts_.push_back({p,seconds_,!next.alive});
                if(!next.alive && std::find(scars_.begin(),scars_.end(),p)==scars_.end()) {
                    if(scars_.size()==128) scars_.erase(scars_.begin());
                    scars_.push_back(p);
                }
            }
            previous_[k]=next;
        } else previous_.push_back(next);
    }
}
void BattleAtmosphere::draw_world(const game::IsoProjection& proj,SpriteAtlas& atlas,float /*zoom*/) const {
    const float w=static_cast<float>(proj.tile_w());
    for(const auto& b:previous_) {
        if(!b.alive) continue;
        if(b.type!=rts::BldType::Keep && !(b.built && b.hp>0 && b.max_hp>0 && b.hp*2<b.max_hp)) continue;
        const auto c=proj.grid_to_screen(b.pos);
        const auto& sprite=atlas.get(rts::ident_of(b.type),"idle","SE");
        if(b.type==rts::BldType::Keep && b.upgrading) {
            for(int i=0;i<8;++i) {
                const float t=std::fmod(seconds_*0.3f+static_cast<float>(i)/8,1.0f);
                DrawCircleV({c.x+w*0.42f*std::sin(static_cast<float>(i)*2.4f),c.y-sprite.ground_anchor.y*SceneRenderer::presentation_scale("Keep",true)*t},
                    w*0.014f,Color{226,214,169,static_cast<unsigned char>((1-t)*180)});
            }
        }
        if(b.built && b.hp>0 && b.max_hp>0 && b.hp*2<b.max_hp) {
            for(int i=0;i<3;++i) {
                const float t=std::fmod(seconds_*0.36f+static_cast<float>(i)/3,1.0f);
                DrawCircleV({c.x+w*(t*0.38f-0.05f),c.y-sprite.ground_anchor.y*0.7f-w*t*0.9f},
                            w*(0.06f+t*0.10f),Color{40,40,45,static_cast<unsigned char>((1-t)*95)});
            }
        }
    }
    for(const auto& feather:feathers_) {
        const auto c=proj.world_to_screen(feather.pos);const float age=seconds_-feather.born;
        for(int i=0;i<14;++i) {
            const float a=static_cast<float>(i)*2.39996f;
            const float spread=w*(0.04f+age*0.24f);
            const Vector2 p{c.x+std::cos(a)*spread+age*w*0.06f,c.y-w*0.6f+std::sin(a)*spread*0.42f+age*w*0.15f};
            const float angle=a+std::sin(age*2+a);
            const Vector2 end{p.x+std::cos(angle)*w*0.08f,p.y+std::sin(angle)*w*0.04f};
            const auto alpha=static_cast<unsigned char>(std::max(0.0f,1-age/3)*235);
            DrawLineEx(p,end,w*0.016f,Color{241,241,222,alpha});
        }
    }
    for(const auto& burst:bursts_) {
        const float age=seconds_-burst.born;
        const auto c=proj.grid_to_screen(burst.pos);
        const float scale=burst.collapse?1.8f:0.7f;
        if(burst.collapse) {
            for(int cloud=0;cloud<5;++cloud) {
                const float angle=static_cast<float>(cloud)*2.4f;
                DrawCircleV({c.x+std::cos(angle)*w*age*0.27f,c.y+std::sin(angle)*w*age*0.12f-w*age*0.18f},
                    w*(0.13f+age*0.19f),Color{139,137,118,static_cast<unsigned char>(std::max(0.0f,1-age/1.8f)*60)});
            }
        }
        for(int i=0;i<10;++i) {
            const float a=static_cast<float>(i)*2.39996f;
            const float d=w*age*(0.3f+static_cast<float>(i%3)*0.13f)*scale;
            const Vector2 p{c.x+std::cos(a)*d,c.y+std::sin(a)*d*0.42f-w*std::sin(std::min(1.0f,age)*3.14159f)*0.24f};
            const auto alpha=static_cast<unsigned char>(std::max(0.0f,1-age/1.8f)*190);
            DrawRectanglePro({p.x,p.y,w*0.035f*scale,w*0.035f*scale},{0,0},a*57.3f+age*80,Color{213,186,139,alpha});
        }
        if(age<0.28f) DrawEllipseLines(static_cast<int>(c.x),static_cast<int>(c.y),w*(0.22f+age),w*(0.10f+age*0.4f),Color{255,225,173,200});
    }
}
void BattleAtmosphere::draw_ground(const game::IsoProjection& proj) const {
    const float w=static_cast<float>(proj.tile_w());
    for(const auto p:scars_) {
        const auto c=proj.grid_to_screen(p);
        for(int i=4;i>0;--i) DrawEllipse(static_cast<int>(c.x),static_cast<int>(c.y),
            w*(0.26f+static_cast<float>(i)*0.075f),w*(0.11f+static_cast<float>(i)*0.025f),Color{39,40,33,20});
        for(int i=0;i<9;++i) {
            const float a=static_cast<float>(i)*2.39996f;
            const float d=w*(0.1f+static_cast<float>(i%3)*0.12f);
            DrawRectanglePro({c.x+std::cos(a)*d,c.y+std::sin(a)*d*0.45f,w*0.07f,w*0.035f},{0,0},a*57.3f,Color{128,123,104,190});
        }
        ellipse({c.x,c.y},w*0.40f,w*0.18f,w*0.006f,Color{57,55,43,50});
    }
}
void BattleAtmosphere::draw_screen(Vector2 vp,const FontSet& font) const {
    // 极淡的斜照与高处雾带只作用于画面；HUD 随后绘制，保持清晰。
    for(int i=0;i<3;++i) {
        const float x=vp.x*(0.10f+static_cast<float>(i)*0.20f);
        DrawTriangle({x,-20},{x+vp.x*0.58f,vp.y*0.72f},
                     {x+vp.x*0.15f,-20},Color{245,193,111,7});
    }
    for(int band=0;band<3;++band) {
        const float drift=std::sin(seconds_*0.05f+static_cast<float>(band))*vp.x*0.08f;
        for(int layer=0;layer<5;++layer) {
            const float radius=vp.x*(0.26f-static_cast<float>(layer)*0.018f);
            DrawEllipse(static_cast<int>(vp.x*(0.20f+static_cast<float>(band)*0.34f)+drift),
                        static_cast<int>(vp.y*(0.14f+static_cast<float>(band%2)*0.06f)),
                        radius,vp.y*(0.055f-static_cast<float>(layer)*0.005f),Color{163,179,180,3});
        }
    }
    DrawRectangleGradientH(0,0,static_cast<int>(vp.x),static_cast<int>(vp.y),Color{221,133,59,21},Color{34,45,77,27});
    DrawRectangleGradientV(0,0,static_cast<int>(vp.x),static_cast<int>(vp.y*0.20f),Color{25,32,46,48},Color{25,32,46,0});
    DrawRectangleGradientV(0,static_cast<int>(vp.y*0.77f),static_cast<int>(vp.x),static_cast<int>(vp.y*0.23f),Color{15,22,33,0},Color{15,22,33,76});
    const float age=seconds_-wave_since_;
    if(wave_>0 && age<4.0f) {
        const float width=std::min(590.0f,vp.x-32),x=(vp.x-width)/2,y=vp.y*0.22f;
        const auto alpha=static_cast<unsigned char>(std::min(1.0f,4-age)*235);
        DrawRectangleRec({x,y,width,72},Color{22,29,34,alpha});
        DrawLineEx({x,y},{x+width,y},1,Color{188,157,96,alpha});
        const std::string title="第 "+std::to_string(wave_)+" 波  /  圣城守望";
        font.draw(title,{(vp.x-font.measure(title,30).x)/2,y+19},30,Color{238,220,175,alpha});
    }
}
}
