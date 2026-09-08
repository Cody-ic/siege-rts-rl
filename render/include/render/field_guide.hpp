#pragma once
#include <algorithm>
#include <array>
#include <string>
#include <vector>
#include <sstream>
#include <map>
#include <memory>
#include "rts/world.hpp"
#include "rts/combat_math.hpp"
#include <cstdio>
#include "game/display_names.hpp"
#include "rts/stats.hpp"
#include "render/sprite_atlas.hpp"
#include "render/text.hpp"

namespace render {
inline constexpr std::array<std::string_view,4> developer_strings={
    "开发者模式", "资源无限 · 人口无限 · 本局不保存、不解锁剧情",
    "下一波：", "输入波数（1–9999），回车应用；Esc 关闭面板"};
// Restrained procedural materials: no external asset or shader dependency.
inline void archive_backdrop(Vector2 vp) {
    DrawRectangleGradientV(0,0,static_cast<int>(vp.x),static_cast<int>(vp.y),Color{29,29,26,255},Color{10,17,20,255});
    for(int x=0;x<static_cast<int>(vp.x);x+=5) DrawLine(x,0,x,static_cast<int>(vp.y),Color{122,105,73,9});
    DrawRectangleLinesEx({12,12,vp.x-24,vp.y-24},1,Color{118,102,69,200});
    DrawRectangleLinesEx({17,17,vp.x-34,vp.y-34},1,Color{62,62,50,180});
}
inline void archive_paper(Rectangle r) {
    DrawRectangleRec({r.x+6,r.y+7,r.width,r.height},Color{4,9,10,150});
    DrawRectangleGradientV(static_cast<int>(r.x),static_cast<int>(r.y),static_cast<int>(r.width),static_cast<int>(r.height),Color{223,208,173,255},Color{190,168,126,255});
    for(int y=static_cast<int>(r.y)+3;y<static_cast<int>(r.y+r.height);y+=6) DrawLine(static_cast<int>(r.x),y,static_cast<int>(r.x+r.width),y,Color{117,88,47,9});
    DrawRectangleLinesEx({r.x+6,r.y+6,r.width-12,r.height-12},1,Color{115,91,56,100});
}
inline Rectangle developer_apply_button(Vector2 vp) {return {(vp.x-860)/2+320,(vp.y-430)/2+326,220,52};}
inline void draw_developer(const FontSet& font,const std::string& wave) {
    const Vector2 vp{static_cast<float>(GetScreenWidth()),static_cast<float>(GetScreenHeight())};
    archive_backdrop(vp);
    const Rectangle panel{(vp.x-860)/2,(vp.y-430)/2,860,430};
    DrawRectangleRec(panel,Color{28,34,33,255});DrawRectangleLinesEx(panel,1,Color{157,132,87,255});
    archive_paper({panel.x+18,panel.y+18,245,panel.height-36});
    const Color ink{59,52,40,255},gold{228,201,148,255};
    font.draw("SANCTUM",{panel.x+43,panel.y+47},22,ink);
    font.draw("CONTROL / 047",{panel.x+43,panel.y+80},14,Color{117,95,61,255});
    DrawCircleLines(static_cast<int>(panel.x+139),static_cast<int>(panel.y+202),69,Color{135,107,62,150});
    DrawPoly({panel.x+139,panel.y+202},6,52,30,Color{97,51,41,255});
    DrawPolyLinesEx({panel.x+139,panel.y+202},6,46,30,2,Color{190,147,92,255});
    // The covenant seal: a radiant sun around a suspended crystal.
    const Vector2 seal{panel.x+139,panel.y+202};
    const Color sigil{232,207,159,255};
    DrawCircleLines(static_cast<int>(seal.x),static_cast<int>(seal.y),24,sigil);
    DrawCircleLines(static_cast<int>(seal.x),static_cast<int>(seal.y),28,Color{186,143,89,255});
    DrawLineEx({seal.x,seal.y-38},{seal.x,seal.y-29},2,sigil);
    DrawLineEx({seal.x,seal.y+29},{seal.x,seal.y+38},2,sigil);
    DrawLineEx({seal.x-38,seal.y},{seal.x-29,seal.y},2,sigil);
    DrawLineEx({seal.x+29,seal.y},{seal.x+38,seal.y},2,sigil);
    for(float dx:{-1.0f,1.0f}) for(float dy:{-1.0f,1.0f})
        DrawLineEx({seal.x+dx*22,seal.y+dy*22},{seal.x+dx*27,seal.y+dy*27},1,sigil);
    DrawLineEx({seal.x,seal.y-19},{seal.x-10,seal.y},2,sigil);
    DrawLineEx({seal.x-10,seal.y},{seal.x,seal.y+19},2,sigil);
    DrawLineEx({seal.x,seal.y+19},{seal.x+10,seal.y},2,sigil);
    DrawLineEx({seal.x+10,seal.y},{seal.x,seal.y-19},2,sigil);
    DrawLineEx({seal.x,seal.y-10},{seal.x,seal.y+10},1,sigil);
    font.draw("指挥官权限",{panel.x+67,panel.y+303},25,ink);
    font.draw("界限之外",{panel.x+91,panel.y+347},18,Color{110,85,51,255});
    const float x=panel.x+300;
    font.draw("DEVELOPER / SANDBOX",{x,panel.y+35},14,Color{151,157,138,255});
    font.draw(std::string(developer_strings[0]),{x,panel.y+66},38,gold);
    font.draw("资源与人口无限 · 正式进度隔离",{x,panel.y+124},20,Color{188,198,187,255});
    font.draw(std::string(developer_strings[2]),{x,panel.y+181},21,gold);
    const Rectangle input{x,panel.y+213,512,70};
    DrawRectangleRec(input,Color{13,22,25,255});DrawRectangleLinesEx(input,1,Color{205,169,104,255});
    font.draw(wave.empty()?"1–9999":wave,{x+20,input.y+17},34,wave.empty()?Color{116,130,125,255}:gold);
    font.draw("不保存对局，不触发剧情",{x,panel.y+295},18,Color{149,164,157,255});
    const auto button=developer_apply_button(vp);const bool hover=CheckCollisionPointRec(GetMousePosition(),button);
    DrawRectangleRec(button,hover?Color{139,76,55,255}:Color{112,55,42,255});DrawRectangleLinesEx(button,1,Color{195,155,95,255});
    font.draw("应用波数  ENTER",{button.x+22,button.y+15},21,gold);
    font.draw("Esc 关闭面板",{x+312,panel.y+345},19,Color{169,182,171,255});
}
inline constexpr std::array<std::string_view,4> guide_tabs={"守方单位","守方建筑","攻方单位","资源点"};
inline constexpr std::array<std::string_view,11> unit_notes={
    "驻墙远程支援。上墙获得高度优势，地面近战不能直接攻击已登墙的弓手；仍需提防远程、飞行单位与攻城锤溅射。",
    "守住城门和缺口的近战主力，擅长抵御骑士。需要弓手和塔楼提供远程支援。",
    "城内快速截击，优先压制幽影弓手；低血量、敌众我寡或接近骑士时回撤。避免单独深入敌阵。",
    "高速侦查，无战斗力。到敌方集结点侦查编成，可能阵亡；不要当作前线士兵使用。",
    "负责施工、升级和维修，无战斗力。优先城内安全位置，敌人靠近时撤退；危险期间工程可能暂停。",
    "耐打的近战步兵，攻击守军并破坏建筑。容易遭到墙头弓手和箭楼集中射击。",
    "远程压制守军，能够攻击城墙上的弓手。可用猎骑接近截击，避免让它持续输出。",
    "快速冲锋，助跑距离影响冲锋伤害。用枪卫堵住突破口，保护后排和工匠。",
    "飞行突袭单位，攻击守军和普通建筑，不能伤害堡垒、城墙与城门。蔽空弩楼负责拦截。",
    "飞行侦查，无战斗力。亲眼看到防御建筑后记录情报，供下一波调整编队；侦查成功会显示提示。",
    "贴身破墙并造成范围伤害，能波及邻墙与驻墙单位。需要尽早集中火力拦截。"};
inline constexpr std::array<std::string_view,11> building_notes={
    "城市核心，被摧毁即失败。提供基础金币收入；升级提高人口、建筑与兵种等级上限，也可征兵。",
    "阻挡攻方，提供驻墙位置。出现缺口后敌人可以进入城内，及时安排工匠维修。",
    "守方可通行的出入口，也是防线薄弱处。保持内侧通道畅通，由枪卫保护。",
    "对地齐射，适合压制密集步兵；不能防空。选中时可看到实际攻击范围。",
    "仅对空，拦截不死鸟和窥使；不能攻击地面敌人。选中时可看到实际攻击范围。",
    "提供广阔视野，提前发现攻方与窥使；自身没有攻击能力。",
    "招募守方单位。金币和人口共同限制征兵，训练等级上限由堡垒决定。",
    "廉价应急屏障，可在战斗中布置，为守军争取时间。不能代替完整城墙。",
    "建在石材资源点上，持续提供石材，用于建设和升级。城外采石场需要保护。",
    "建在木材资源点上，持续提供木材，用于建筑、维修和升级。城外伐木场容易遭到袭击。",
    "建在金币资源点上，持续提供金币，用于招募守军。失去金矿场会削弱补兵能力。"};
inline constexpr std::array<std::string_view,3> resource_notes={
    "石材点需要建设采石场才能持续产出。资源点本身不是可攻击建筑；敌人可摧毁其上的采集建筑，中断收入。",
    "木材点需要建设伐木场才能持续产出。地图上的可清除树木是另一种一次性来源，不等同于长期采集点。",
    "金币点需要建设金矿场才能持续产出。资源点有城内与城外之分，向外扩张时要同时规划守军与撤退路线。"};
inline std::vector<std::string_view> guide_strings() {
    std::vector<std::string_view> result={"图鉴","本级 下一级 点击等级输入 回车确认 预览范围 1–9999 等级预览 级 相对一级 增量 需要堡垒 当前 满足 未满足 无堡垒等级限制 下一次升级 石 木 工时 属性可滚动 资源点无等级 产出不随等级增长 到 建筑等级 不含战场加成 预览上限", "圣城军备档案","指挥官权限","界限之外","资源与人口无限 · 正式进度隔离","不保存对局，不触发剧情","应用波数  ENTER","战术备忘","基础档案","返回","属性记录","名录","当前数值表 · 一级基础属性","血量 伤害 射程 格 视野 移速 格/秒 攻击间隔 秒 招募 金币 训练 建造 石材 木材 工时 基础周期产出 周期 滚轮阅读 方向键选择","石材点","木材点","金币点"};
    for(auto t:guide_tabs) result.push_back(t);
    for(auto t:unit_notes) result.push_back(t);
    for(auto t:building_notes) result.push_back(t);
    for(auto t:resource_notes) result.push_back(t);
    return result;
}
// All layout, pointer input and clipping share one logical coordinate system.
// A 960x540 baseline grows uniformly on large screens; ultrawide content stays centered.
struct GuideViewport {
    float zoom;
    Vector2 size,offset;
    explicit GuideViewport(Vector2 pixels) : zoom(std::max(0.01f,std::min(pixels.x/960.0f,pixels.y/540.0f))),
        size{std::min(pixels.x/zoom,1280.0f),std::min(pixels.y/zoom,800.0f)},
        offset{(pixels.x-size.x*zoom)/2,(pixels.y-size.y*zoom)/2} {}
    Vector2 pointer(Vector2 p) const {return {(p.x-offset.x)/zoom,(p.y-offset.y)/zoom};}
    Rectangle pixels(Rectangle r) const {return {offset.x+r.x*zoom,offset.y+r.y*zoom,r.width*zoom,r.height*zoom};}
    Camera2D camera() const {return {offset,{0,0},0,zoom};}
};
inline Rectangle guide_back_button(Vector2 vp) {return {vp.x-120,25,90,42};}
class FieldGuide {
    int tab_=0,selected_=0,level_=1;
    bool bottom_preview_=false;
    bool editing_=false;std::string level_text_;
    float stat_scroll_=0,stat_max_=0;
    std::unique_ptr<rts::World> rules_;
    std::map<unsigned int,Rectangle> bounds_;
    float scroll_=0,max_scroll_=0;
    int count() const {return tab_==0?5:tab_==1?11:tab_==2?6:3;}
public:
    void preview_bottom(bool value) {bottom_preview_=value;}
    void preview_level(int level) {level_=std::clamp(level,1,9999);}
    static Rectangle level_box(Vector2 vp) {return {vp.x-224,100,140,36};}
    void preview(int index) {tab_=index<5?0:index<16?1:index<22?2:3;selected_=index-(tab_==0?0:tab_==1?5:tab_==2?16:22);}
    bool input(Vector2 vp) {
        const GuideViewport viewport(vp);vp=viewport.size;
        const auto mouse=viewport.pointer(GetMousePosition());
        if(tab_!=3) {
            if(editing_) {
                for(int ch=GetCharPressed();ch>0;ch=GetCharPressed()) if(ch>='0'&&ch<='9'&&level_text_.size()<4) level_text_+=static_cast<char>(ch);
                if(IsKeyPressed(KEY_BACKSPACE)&&!level_text_.empty()) level_text_.pop_back();
                if(IsKeyPressed(KEY_ENTER)) {if(!level_text_.empty()) level_=std::clamp(std::stoi(level_text_),1,9999);editing_=false;stat_scroll_=0;}
                if(IsKeyPressed(KEY_ESCAPE)) {editing_=false;return false;}
            }
            if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if(CheckCollisionPointRec(mouse,level_box(vp))) {editing_=true;level_text_.clear();}
                else {editing_=false;
                    if(CheckCollisionPointRec(mouse,{vp.x-270,100,36,36})) {level_=std::max(1,level_-1);stat_scroll_=0;}
                    if(CheckCollisionPointRec(mouse,{vp.x-74,100,36,36})) {level_=std::min(9999,level_+1);stat_scroll_=0;}
                }
            }
        }
        if(IsKeyPressed(KEY_ESCAPE) || (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)&&CheckCollisionPointRec(mouse,guide_back_button(vp)))) return true;
        for(int t=0;t<4;++t) if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)&&CheckCollisionPointRec(mouse,{24+static_cast<float>(t)*160,100,150,36})) {tab_=t;selected_=0;scroll_=0;stat_scroll_=0;editing_=false;}
        int next=selected_;
        if(IsKeyPressed(KEY_DOWN)) next=(next+1)%count();
        if(IsKeyPressed(KEY_UP)) next=(next+count()-1)%count();
        for(int i=0;i<count();++i) if(IsMouseButtonPressed(MOUSE_BUTTON_LEFT)&&CheckCollisionPointRec(mouse,{24,153+static_cast<float>(i)*29,210,28})) next=i;
        if(next!=selected_) {selected_=next;scroll_=0;stat_scroll_=0;}
        if(mouse.x>=527 && mouse.y>=149 && mouse.y<=149+std::clamp(vp.y*0.40f,220.0f,330.0f)) stat_scroll_=std::clamp(stat_scroll_-GetMouseWheelMove()*38,0.0f,stat_max_);
        else scroll_=std::clamp(scroll_-GetMouseWheelMove()*38,0.0f,max_scroll_);
        return false;
    }
    void draw(const FontSet& font,SpriteAtlas& atlas,const rts::StatsTable& stats,Vector2 vp,const rts::World* current=nullptr) {
        if(!rules_) {rts::WorldInit init;init.width=1;init.height=1;init.terrain={rts::Terrain::Plain};init.keep={0,0};init.stats=stats;init.buildings.push_back({rts::BldType::Keep,{0,0},stats.of(rts::BldType::Keep).max_hp,stats.of(rts::BldType::Keep).max_hp});rules_=std::make_unique<rts::World>(init);}
        archive_backdrop(vp);
        const GuideViewport viewport(vp);vp=viewport.size;
        BeginMode2D(viewport.camera());
        archive_backdrop(vp);
        const Color gold{228,201,149,255},white{210,219,207,255};
        font.draw("SANCTUM / FIELD ARCHIVE",{30,26},14,Color{153,153,126,255});
        font.draw("圣城军备档案",{28,49},34,gold);
        font.draw("等级预览 · 不含战场加成",{340,64},17,Color{151,167,158,255});
        const Rectangle back=guide_back_button(vp);
        DrawRectangleLinesEx(back,1,Color{143,124,81,255});font.draw("返回",{back.x+24,back.y+12},20,gold);
        DrawLineEx({28,91},{vp.x-28,91},1,Color{91,92,71,255});
        for(int t=0;t<4;++t) {
            Rectangle r{24+static_cast<float>(t)*160,100,150,36};
            DrawRectangleRec(r,t==tab_?Color{106,59,45,255}:Color{34,42,39,255});
            if(t==tab_) DrawRectangleRec({r.x,r.y+33,r.width,3},gold);
            font.draw(std::string(guide_tabs[static_cast<std::size_t>(t)]),{r.x+18,r.y+8},20,t==tab_?gold:white);
        }
        if(tab_!=3) {
            for(auto r:{Rectangle{vp.x-270,100,36,36},level_box(vp),Rectangle{vp.x-74,100,36,36}}) {DrawRectangleRec(r,Color{22,31,31,255});DrawRectangleLinesEx(r,1,Color{144,122,77,255});}
            font.draw("-",{vp.x-258,106},24,gold);font.draw("+",{vp.x-64,106},24,gold);
            font.draw(editing_?level_text_+"_":std::to_string(level_)+" 级",{vp.x-208,108},20,gold);
        } else font.draw("资源点无等级",{vp.x-260,109},18,white);
        DrawRectangleRec({24,146,210,vp.y-191},Color{18,27,28,190});
        std::string ident,name,notes;std::ostringstream values;values.precision(3);
        auto label=[&](int i)->std::string {if(tab_==0||tab_==2) return std::string(game::display_name(static_cast<rts::UnitType>(i+(tab_==2?5:0))));if(tab_==1) return std::string(game::display_name(static_cast<rts::BldType>(i)));return std::string(std::array<std::string_view,3>{"石材点","木材点","金币点"}[static_cast<std::size_t>(i)]);};
        for(int i=0;i<count();++i) {float y=153+static_cast<float>(i)*29;if(i==selected_) {DrawRectangleRec({24,y,210,28},Color{71,65,48,255});DrawRectangleRec({24,y,3,28},gold);}font.draw(label(i),{34,y+4},20,white);}
        name=label(selected_);
        const auto scaled=[&](std::int64_t base,bool hp) {return base<=0?std::int64_t{0}:rts::apply_permille(base,{rts::level_permille(level_,hp?stats.global.hp_permille_per_level:stats.global.dmg_permille_per_level)});};
        const auto with_delta=[&](std::int64_t base,bool hp) {const auto value=scaled(base,hp);return std::to_string(value)+(level_>1?" (+"+std::to_string(value-base)+")":"");};
        const auto requirement=[&](int needed) {values<<"需要堡垒 "<<needed<<" 级";if(current) values<<"\n当前堡垒 "<<current->unit_level_cap()<<" "<<(current->unit_level_cap()>=needed?"满足":"未满足");values<<"\n";};

        if(tab_==0||tab_==2) {
            int index=selected_+(tab_==2?5:0);auto type=static_cast<rts::UnitType>(index);const auto& st=stats.of(type);ident=rts::ident_of(type);notes=unit_notes[static_cast<std::size_t>(index)];
            values<<"血量 "<<with_delta(st.max_hp,true)<<"\n伤害 "<<with_delta(st.damage,false)<<"\n射程 "<<st.range<<" 格   视野 "<<st.vision<<" 格\n移速 "<<st.speed*20<<" 格/秒\n攻击间隔 "<<static_cast<float>(st.cooldown_ticks)/20<<" 秒";
            if(tab_==0) {values<<"\n招募 "<<rules_->train_cost_gold(type,level_)<<" 金币\n训练 "<<static_cast<float>(rules_->train_ticks_at(type,level_))/20<<" 秒\n";requirement(level_);}
        } else if(tab_==1) {
            auto type=static_cast<rts::BldType>(selected_);const auto& st=stats.of(type);ident=rts::ident_of(type);notes=building_notes[static_cast<std::size_t>(selected_)];
            values<<"血量 "<<with_delta(st.max_hp,true)<<"\n伤害 "<<with_delta(st.damage,false)<<"\n射程 "<<st.range<<" 格   视野 "<<st.vision<<" 格\n建造 "<<st.cost_stone<<" 石 / "<<st.cost_wood<<" 木\n基础周期产出 "<<st.income_amount<<"\n下一次升级 "<<level_<<" 到 "<<level_+1<<"\n"<<rules_->bld_upgrade_cost_stone(type,level_)<<" 石 / "<<rules_->bld_upgrade_cost_wood(type,level_)<<" 木\n工时 "<<st.upgrade_ticks<<"\n";
            if(type!=rts::BldType::Keep) {values<<"本级";requirement((level_-1)*stats.global.building_level_cap_divisor+1);values<<"下一级";requirement(level_*stats.global.building_level_cap_divisor+1);}
            else values<<"无堡垒等级限制\n";
        } else {
            ident=std::array<std::string_view,3>{"StonePt","WoodPt","GoldPt"}[static_cast<std::size_t>(selected_)];notes=std::string(resource_notes[static_cast<std::size_t>(selected_)])+" 产出不随等级增长。";
            values<<"基础周期产出 "<<stats.of(rts::gatherer_of(static_cast<rts::Resource>(selected_))).income_amount<<"\n周期 "<<static_cast<float>(stats.global.income_period_ticks)/20<<" 秒";
        }
        const float hero_top=149,hero_height=std::clamp(vp.y*0.40f,220.0f,330.0f);
        const float details_x=540;
        archive_paper({252,hero_top,260,hero_height});
        DrawRectangleRec({527,hero_top,vp.x-551,hero_height},Color{29,39,39,235});
        DrawRectangleLinesEx({527,hero_top,vp.x-551,hero_height},1,Color{79,89,73,255});
        char serial[48];std::snprintf(serial,sizeof(serial),"ARCHIVE / %02d.%02d",tab_+1,selected_+1);
        font.draw(serial,{270,hero_top+14},13,Color{123,97,56,255});
        const auto& sprite=atlas.get(ident,"idle","SE");
        auto cached=bounds_.find(sprite.texture.id);
        if(cached==bounds_.end()) {
            int left=sprite.texture.width,top=sprite.texture.height,right=0,bottom=0;
            for(int yy=0;yy<sprite.texture.height;++yy) for(int xx=0;xx<sprite.texture.width;++xx)
                if(atlas.opaque_at(sprite,xx,yy)) {left=std::min(left,xx);right=std::max(right,xx);top=std::min(top,yy);bottom=std::max(bottom,yy);}
            if(right<left || bottom<top) {left=0;top=0;right=sprite.texture.width-1;bottom=sprite.texture.height-1;}
            cached=bounds_.emplace(sprite.texture.id,Rectangle{static_cast<float>(left),static_cast<float>(top),static_cast<float>(right-left+1),static_cast<float>(bottom-top+1)}).first;
        }
        const auto source=cached->second;
        const float scale=std::min(214.0f/source.width,(hero_height-70)/source.height);
        const float image_w=source.width*scale,image_h=source.height*scale;
        const float floor=hero_top+hero_height-30;
        DrawEllipse(382,static_cast<int>(floor),73,9,Color{83,67,41,45});
        DrawCircleLines(382,static_cast<int>(hero_top+hero_height/2),static_cast<float>(std::min(90,static_cast<int>(hero_height/2-30))),Color{114,92,55,60});
        DrawTexturePro(sprite.texture,source,{382-image_w/2,floor-image_h,image_w,image_h},{0,0},0,WHITE);
        font.draw("基础档案",{270,hero_top+hero_height-24},13,Color{123,97,56,255});
        font.draw(name,{details_x+6,hero_top+17},29,gold);
        font.draw("属性可滚动 · 增量相对一级",{details_x+6,hero_top+57},15,Color{157,177,166,255});
        std::vector<std::string> stat_rows;std::string line;std::istringstream rows(values.str());
        while(std::getline(rows,line)) stat_rows.push_back(line);
        stat_max_=std::max(0.0f,static_cast<float>(stat_rows.size())*28-(hero_height-92));
        stat_scroll_=bottom_preview_?stat_max_:std::clamp(stat_scroll_,0.0f,stat_max_);
        const auto stat_clip=viewport.pixels({details_x+6,hero_top+84,vp.x-details_x-36,hero_height-92});
        BeginScissorMode(static_cast<int>(stat_clip.x),static_cast<int>(stat_clip.y),static_cast<int>(stat_clip.width),static_cast<int>(stat_clip.height));
        float y=hero_top+86-stat_scroll_;
        for(const auto& row:stat_rows) {font.draw(row,{details_x+6,y},17,white);DrawLineEx({details_x+6,y+25},{vp.x-46,y+25},1,Color{64,78,69,135});y+=28;}
        EndScissorMode();
        if(stat_max_>0) {const float track=hero_height-96;DrawRectangleRec({vp.x-34,hero_top+86,3,track},Color{66,77,66,255});DrawRectangleRec({vp.x-34,hero_top+86+(track-24)*stat_scroll_/stat_max_,3,24},gold);}
        const float note_top=hero_top+hero_height+14;
        DrawRectangleRec({252,note_top,vp.x-276,vp.y-note_top-46},Color{27,36,35,235});
        DrawRectangleRec({252,note_top,3,25},Color{155,92,62,255});
        font.draw("战术备忘",{269,note_top+9},18,gold);
        const float top=note_top+40,bottom=vp.y-54,width=vp.x-324;float ty=top-scroll_;
        const auto clip=viewport.pixels({269,top,width,std::max(1.0f,bottom-top)});
        BeginScissorMode(static_cast<int>(clip.x),static_cast<int>(clip.y),static_cast<int>(clip.width),static_cast<int>(clip.height));
        line.clear();auto flush=[&]() {font.draw(line,{269,ty},19,white);ty+=27;line.clear();};
        for(std::size_t i=0;i<notes.size();) {int bytes=0;GetCodepointNext(notes.c_str()+i,&bytes);auto glyph=notes.substr(i,static_cast<std::size_t>(bytes));if(!line.empty()&&font.measure(line+glyph,19).x>width && std::string_view("，。；、！？：）").find(glyph)==std::string_view::npos) flush();line+=glyph;i+=static_cast<std::size_t>(bytes);}if(!line.empty()) flush();EndScissorMode();max_scroll_=std::max(0.0f,ty+scroll_-bottom);
        font.draw("方向键选择 · 滚轮阅读 · 点击等级输入 · 回车确认 · Esc 返回",{28,vp.y-34},15,Color{169,158,121,255});
        EndMode2D();

    }
};
}
