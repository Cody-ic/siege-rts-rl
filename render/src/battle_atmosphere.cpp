#include "render/battle_atmosphere.hpp"
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
void BattleAtmosphere::reset() {previous_.clear();bursts_.clear();seconds_=0;wave_=0;wave_since_=0;}
void BattleAtmosphere::observe(const rts::WorldView& v,rts::Tick tick,int wave) {
    seconds_=static_cast<float>(tick)/rts::kTicksPerSecond;
    if(wave!=wave_) {wave_=wave;wave_since_=seconds_;}
    bursts_.erase(std::remove_if(bursts_.begin(),bursts_.end(),[&](const Burst& b){return seconds_-b.born>1.8f;}),bursts_.end());
    for(std::size_t k=0;k<v.bld_pos().size();++k) {
        Building next{v.bld_pos()[k],v.bld_type()[k],v.bld_hp()[k],v.bld_max_hp()[k],
                      v.bld_alive()[k]!=0,v.bld_level()[k],v.bld_upgrade_left()[k]>0,v.bld_built()[k]!=0};
        if(k<previous_.size()) {
            const auto& old=previous_[k];
            if(old.alive && old.pos==next.pos && (next.hp<old.hp || !next.alive) && bursts_.size()<96)
                bursts_.push_back({next.pos,seconds_,!next.alive});
            previous_[k]=next;
        } else previous_.push_back(next);
    }
}
void BattleAtmosphere::draw_world(const game::IsoProjection& proj,SpriteAtlas& atlas,float zoom) const {
    const float w=static_cast<float>(proj.tile_w());
    for(const auto& b:previous_) {
        if(!b.alive) continue;
        if(b.type!=rts::BldType::Keep && !(b.built && b.hp>0 && b.max_hp>0 && b.hp*2<b.max_hp)) continue;
        const auto c=proj.grid_to_screen(b.pos);
        const auto& sprite=atlas.get(rts::ident_of(b.type),"idle","SE");
        if(b.type==rts::BldType::Keep) {
            const float r=w*(0.48f+0.018f*static_cast<float>(std::min(8,b.level)));
            const Vector2 center{c.x,c.y-sprite.ground_anchor.y-w*0.19f};
            const unsigned char alpha=static_cast<unsigned char>(165+35*std::sin(seconds_*1.5f));
            ellipse(center,r,r*0.36f,1.4f/zoom,Color{191,214,209,alpha});
            ellipse(center,r*0.78f,r*0.28f,1.0f/zoom,Color{220,196,131,alpha});
            for(int i=0;i<8;++i) {
                const float a=static_cast<float>(i)*0.785398f;
                const Vector2 p{center.x+std::cos(a)*r,center.y+std::sin(a)*r*0.36f};
                DrawCircleV(p,w*0.018f,Color{232,220,166,alpha});
                if(b.upgrading) DrawLineEx(p,{p.x, p.y-w*(0.12f+0.06f*std::sin(seconds_*3+a))},
                                           std::max(1.2f/zoom,w*0.008f),Color{201,224,218,170});
            }
            DrawLineEx({center.x,center.y-w*0.18f},{center.x,center.y+w*0.18f},w*0.018f,Color{220,196,131,220});
            DrawCircleLines(static_cast<int>(center.x),static_cast<int>(center.y),w*0.09f,Color{220,196,131,220});
        }
        if(b.built && b.hp>0 && b.max_hp>0 && b.hp*2<b.max_hp) {
            for(int i=0;i<3;++i) {
                const float t=std::fmod(seconds_*0.36f+static_cast<float>(i)/3,1.0f);
                DrawCircleV({c.x+w*(t*0.38f-0.05f),c.y-sprite.ground_anchor.y*0.7f-w*t*0.9f},
                            w*(0.06f+t*0.10f),Color{40,40,45,static_cast<unsigned char>((1-t)*95)});
            }
        }
    }
    for(const auto& burst:bursts_) {
        const float age=seconds_-burst.born;
        const auto c=proj.grid_to_screen(burst.pos);
        const float scale=burst.collapse?1.8f:0.7f;
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
